/*  Copyright (C) 2013  Nithin Nellikunnu, nithin.nn@gmail.com
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <list>
#include <vector>
#include <map>

#include "types.hpp"
#include "error.hpp"
#include "logger.hpp"
#include "macros.hpp"
#include "gtp_macro.hpp"
#include "task.hpp"
#include "timer.hpp"
#include "transport.hpp"
#include "gtp_types.hpp"
#include "gtp_util.hpp"
#include "gtp_if.hpp"
#include "gtp_ie.hpp"
#include "gtp_msg.hpp"
#include "sim_cfg.hpp"
#include "message.hpp"
#include "gtp_stats.hpp"
#include "gtp_peer.hpp"
#include "scenario.hpp"
#include "tunnel.hpp"
#include "traffic.hpp"
#include "session.hpp"

static UeSessionMap  s_ueSessionMap;
static U32           g_sessionId = 0;

#define UE_SSN_FINISH_TASK(_ueSsn)                             \
do                                                             \
{                                                              \
   ((_ueSsn)->m_currTaskIndx++);                               \
   (_ueSsn)->m_currTask = m_pScn->m_jobSeq[m_currTaskIndx];   \
} while(0)  

#define UE_SSN_FINISH_REQ_TASK   UE_SSN_FINISH_TASK
#define UE_SSN_FINISH_RSP_TASK   UE_SSN_FINISH_TASK

#define IS_SCN_COMPLETED() (m_currTaskIndx == m_pScn->m_jobSeq.size())

/**
 * @brief
 *    Constructor
 *
 * @param pScn
 */
UeSession::UeSession(Scenario *pScn, GtpImsiKey imsi)
{
   m_pScn = pScn;
   m_n3req = Config::getInstance()->getN3Requests();
   m_retryCnt = 0;
   m_t3time = Config::getInstance()->getT3Timer();
   m_sessionId = ++g_sessionId;
   m_nodeType = Config::getInstance()->getNodeType();
   m_peerEp.ipAddr = Config::getInstance()->getRemoteIpAddr();
   m_peerEp.port = Config::getInstance()->getRemoteGtpcPort();
   m_currTaskIndx = 0;
   m_currTask = m_pScn->m_jobSeq[m_currTaskIndx];
   m_bitmask = 0;
   m_imsiKey = imsi;
   m_deadCallWait = Config::getInstance()->getDeadCallWait();
   m_bearerVec.reserve(GTP_MAX_BEARERS);
   for (U32 i = 0; i < GTP_MAX_BEARERS; i++)
   {
      m_bearerVec[i] = NULL;
   }

   LOG_DEBUG("Creating UE Session [%d]", m_sessionId);
}


/**
 * @brief
 *    Destructor for UeSession
 */
UeSession::~UeSession()
{
   s_ueSessionMap.erase(m_imsiKey);

   if (NULL != m_currUeProc.sentMsg)
      delete m_currUeProc.sentMsg;

   if (NULL != m_prevUeProc.sentMsg)
      delete m_prevUeProc.sentMsg;

   for (GtpcPdnLstItr pPdn = m_pdnLst.begin(); pPdn != m_pdnLst.end(); pPdn++)
   {
      /* delete the c-plane tunnels */
      GtpcTun *pCTun = (*pPdn)->pCTun;
      if (NULL != pCTun)
      {
         deleteCTun(pCTun);
         pCTun = NULL;
      }
      
      for (U32 i = 0; i < GTP_MAX_BEARERS; i++)
      {
         GtpBearer *bearer = m_bearerVec[i];
         if (NULL != bearer)
         {
            if (GSIM_CHK_BEARER_MASK((*pPdn)->bearerMask, bearer->getEbi()))
            {
               GSIM_UNSET_BEARER_MASK((*pPdn)->bearerMask, bearer->getEbi());
               delete bearer;
            }
         }
      }

      delete *pPdn;
   }

   LOG_DEBUG("Deleting UE Session [%d]", m_sessionId);
}

RETVAL UeSession::run(VOID *arg)
{
   RETVAL         ret = ROK;

   LOG_TRACE("Running UeSession [%d]", m_sessionId);
   m_lastRunTime = getMilliSeconds();
   resumeTask();

   if (GSIM_CHK_MASK(m_bitmask, GSIM_UE_SSN_SCN_COMPLETE))
   {
      handleDeadCall(arg);
   }
   else
   {
      if (NULL != arg)
      {
         LOG_TRACE("Processing Recv() Task");
         ret = handleRecv((UdpData_t*)arg);
      }
      else
      {
         JobType_t taskType = m_currTask->type();
         if (taskType == JOB_TYPE_SEND)
         {
            LOG_TRACE("Processing Send() Task");
            ret = handleSend();
         }
         else if (taskType == JOB_TYPE_WAIT)
         {
            LOG_TRACE("Processing Wait() Task");
            ret = handleWait();
         }
      }
   }

   LOG_EXITFN(ret);
}

RETVAL UeSession::handleSend()
{
   LOG_ENTERFN();

   RETVAL ret = ROK;

   if GSIM_CHK_MASK(this->m_bitmask, GSIM_UE_SSN_WAITING_FOR_RSP)
   {
      LOG_DEBUG("Processing Request Timeout")
      ret = handleOutReqTimeout();
      if (ERR_MAX_RETRY_EXCEEDED == ret)
      {
         m_currTask->m_numTimeOut++;
         Stats::incStats(GSIM_STAT_NUM_SESSIONS_FAIL);
         delete m_currUeProc.sentMsg;
         m_currUeProc.sentMsg = NULL;

         /* request retry exceeded n3-requests. terminate the 
          * UE session Task
          */
         ret = ROK_OVER;
      }
   }
   else
   {
      GtpMsg *gtpMsg = m_currTask->getGtpMsg();
      GtpMsgCategory_t msgCat = gtpGetMsgCategory(gtpMsg->type());
      if (GTP_MSG_CAT_REQ == msgCat)
      {
         /* a request task gets over only when a timeout occurs after
          * maximum n3-request retris, or when an associated response
          * message is received. so after processing sending the request
          * out do not finish the task
          */
         ret = handleOutReqMsg(gtpMsg);
         if (ROK != ret)
         {
            /* sending a request message failed, terminate the UE
             * Session Task
             */
            ret = ROK_OVER;
            LOG_ERROR("Sending request message to peer, Error [%d]", ret);
         }
         else
         {
            /* pausing this task until next wakeup time, for retransmissing
             * the request message
             */
            pause();
         }
      }
      else
      {
         ret = handleOutRspMsg(gtpMsg);
         if (ROK != ret)
         {
            /* sending a response message failed, terminate the UE
             * Session Task
             */
            ret = ROK_OVER;
            LOG_ERROR("Sending response message to peer, Error [%d]", ret);
         }
         else
         {
            this->stop();
         }
      }
   }

   LOG_EXITFN(ret);
}

RETVAL UeSession::handleOutReqMsg(GtpMsg *gtpMsg)
{
   LOG_ENTERFN();

   RETVAL   ret = ROK;
   GtpcPdn  *pPdn = NULL;

   if (GTPC_MSG_CS_REQ == gtpMsg->type())
   {
      LOG_DEBUG("Creating PDN Connection");
      Stats::incStats(GSIM_STAT_NUM_SESSIONS_CREATED);
      Stats::incStats(GSIM_STAT_NUM_SESSIONS);
      pPdn = createPdn();
      m_pdnLst.push_back(pPdn);
      m_pCurrPdn = pPdn;
   }
   else
   {
      pPdn = m_pCurrPdn;
   }

   LOG_DEBUG("Storing OUT Message");
   createBearers(pPdn, gtpMsg, 0);

   LOG_DEBUG("Encoding OUT Message");
   m_currUeProc.seqNumber = generateSeqNum(&m_peerEp, GTP_MSG_CAT_REQ);
   m_currReqType = gtpMsg->type();
   UdpData_t *pNwData = new UdpData_t;
   encGtpcOutMsg(pPdn, gtpMsg, &pNwData->buf, &m_peerEp);

   /* initial message, send the message over default send socket */
   pNwData->connId = 0;
   pNwData->peerEp = m_peerEp;

   LOG_DEBUG("Sending GTPC Message [%s]", gtpGetMsgName(msgType));
   Buffer *buf = new Buffer(pNwData->buf);
   sendMsg(pNwData->connId, &pNwData->peerEp, buf);
   m_currTask->m_numSnd++;

   m_currUeProc.sentMsg = pNwData;
   GSIM_SET_MASK(this->m_bitmask, GSIM_UE_SSN_WAITING_FOR_RSP);

   // update when the task thas to wakeup next time
   m_wakeTime = m_lastRunTime + m_t3time;

   LOG_EXITFN(ret);
}

/**
 * @brief Request timeout handler. If maximum retries exceeds returns
 *    failure, so that the session is terminated. otherwise resends
 *    the request message and sets the next wakeup time
 * 
 * @return 
 */
RETVAL UeSession::handleOutReqTimeout()
{
   LOG_ENTERFN();

   RETVAL ret = ROK;

   /* Recived task is run because GTP-C message request timedout
    * waiting for a response, retransmit the request message
    */
   if (m_retryCnt >= m_n3req)
   {
      delete m_currUeProc.sentMsg;
      m_currUeProc.sentMsg = NULL;
      LOG_DEBUG("Maximum Retries reached");
      ret = ERR_MAX_RETRY_EXCEEDED;
   }
   else
   {
      /* we have already processed this message, the task is run again
       * after retransmission timeout expiry
       */
      LOG_DEBUG("Retransmissing GTP Message");
      Buffer *buf = new Buffer(m_currUeProc.sentMsg->buf);
      sendMsg(m_currUeProc.sentMsg->connId, &m_currUeProc.sentMsg->peerEp, buf);

      m_currTask->m_numSndRetrans++;
      m_retryCnt++;

      // if response is not received within T3 timer expiry
      // wakeup and retransmit request message
      m_wakeTime = m_lastRunTime + m_t3time;
      pause();
   }

   LOG_EXITFN(ret);
}

RETVAL UeSession::handleOutRspMsg(GtpMsg *gtpMsg)
{
   LOG_ENTERFN();

   RETVAL   ret = ROK;
   GtpcPdn  *pPdn = m_pCurrPdn;

   LOG_DEBUG("Encoding OUT Message");
   UdpData_t *pNwData = new UdpData_t;
   encGtpcOutMsg(pPdn, m_currTask->getGtpMsg(), &pNwData->buf, &m_peerEp);

   /* send the response/triggered message over the same socket
    * over which the request/command is received
    */
   pNwData->connId = m_currUeProc.connId;
   pNwData->peerEp = pPdn->pCTun->m_peerEp;

   LOG_DEBUG("Sending GTPC Message [%s]", gtpGetMsgName(msgType));
   Buffer *buf = new Buffer(pNwData->buf);
   sendMsg(pNwData->connId, &pNwData->peerEp, buf);
   m_currTask->m_numSnd++;

   delete m_prevUeProc.sentMsg;
   m_prevUeProc.sentMsg = pNwData;
   m_prevUeProc.rspType = gtpMsg->type();

   UE_SSN_FINISH_TASK(this);

   if (IS_SCN_COMPLETED())
   {
      handleCompletedTask();
   }

   LOG_EXITFN(ret);
}

RETVAL UeSession::handleRecv(UdpData_t *data)
{
   LOG_ENTERFN();

   RETVAL ret = ROK;

   /* Receive task is run because a GTPC message is received for this
    * session
    */
   GtpMsg gtpMsg(&data->buf);
   GtpMsgCategory_t msgCat = gtpGetMsgCategory(gtpMsg.type());

   if (msgCat == GTP_MSG_CAT_REQ)
   {
      LOG_DEBUG("Processing Incoming Request message");
      ret = handleIncReqMsg(&gtpMsg, data);
      if (ROK != ret)
      {
         LOG_ERROR("Processing Incoming Request Message, Error [%d]", ret);
      }
   }
   else if (msgCat == GTP_MSG_CAT_RSP)
   {
      LOG_DEBUG("Processing Incoming Response message");
      ret = handleIncRspMsg(&gtpMsg, data);
      if (ROK != ret)
      {
         LOG_ERROR("Processing Incoming Response Message, Error [%d]", ret);
      }
   }

   delete data;
   m_wakeTime = 0;
   LOG_EXITFN(ret);
}

JobType_t UeSession::nextTaskType()
{
   JobType_t  taskType = JOB_TYPE_INV;

   if (m_currTaskIndx + 1 < m_pScn->m_jobSeq.size())
   {
      taskType = m_pScn->m_jobSeq[m_currTaskIndx + 1]->type();
   }

   return taskType;
}

RETVAL UeSession::handleIncReqMsg(GtpMsg *rcvdReq, UdpData_t *rcvdData)
{
   LOG_ENTERFN();

   if (isExpectedReq(rcvdReq))
   {
      m_currTask->m_numRcv++;
   }
   else if (isPrevProcReq(rcvdReq))
   {
      m_prevUeProc.procTask->m_numRcvRetrans++;

      /* resend the request response */
      Buffer *buf = new Buffer(m_prevUeProc.sentMsg->buf);
      sendMsg(m_prevUeProc.sentMsg->connId, &m_prevUeProc.sentMsg->peerEp, buf);
      LOG_EXITFN(ROK);
   }
   else
   {
      m_currTask->m_numUnexp++;
      LOG_EXITFN(ROK);
   }

   GtpcPdn *pdn = NULL;
   if (GTPC_MSG_CS_REQ == rcvdReq->type())
   {
      LOG_DEBUG("Creating PDN Connection");
      Stats::incStats(GSIM_STAT_NUM_SESSIONS_CREATED);
      Stats::incStats(GSIM_STAT_NUM_SESSIONS);
      pdn = createPdn();
      m_pdnLst.push_back(pdn);
      m_pCurrPdn = pdn;
   }
   else
   {
      pdn = m_pCurrPdn;
   }

   m_currUeProc.connId = rcvdData->connId;
   m_currUeProc.seqNumber = rcvdReq->seqNumber();
   m_currReqType = rcvdReq->type();

   updatePeerSeqNumber(&rcvdData->peerEp, m_currUeProc.seqNumber);
   decAndStoreGtpcIncMsg(pdn, rcvdReq, &rcvdData->peerEp);

   m_prevUeProc.connId = m_currUeProc.connId;
   m_prevUeProc.seqNumber = m_currUeProc.seqNumber;
   m_prevUeProc.reqType = rcvdReq->type();
   m_prevUeProc.procTask = m_currTask;

   /* finish the recv task and send the response immediately */
   UE_SSN_FINISH_TASK(this);
   this->run();

   LOG_EXITFN(ROK);
}

BOOL UeSession::isExpectedRsp(GtpMsg *rspMsg)
{
   LOG_ENTERFN();

   BOOL expected = FALSE;

   Job *task = m_pScn->m_jobSeq[m_currTaskIndx + 1];
   GtpMsg *expectedRspMsg = task->getGtpMsg();

   if ((expectedRspMsg->type() == rspMsg->type()) && \
       (m_currUeProc.seqNumber == rspMsg->seqNumber()))
   {
      expected = TRUE;
   }

   LOG_EXITFN(expected);
}

BOOL UeSession::isExpectedReq(GtpMsg *reqMsg)
{
   LOG_ENTERFN();

   BOOL expected = FALSE;

   GtpMsg *expectedReqMsg = m_currTask->getGtpMsg();
   if ((expectedReqMsg->type() == reqMsg->type()) && \
       (m_currUeProc.seqNumber < reqMsg->seqNumber()))
   {
      expected = TRUE;
   }

   LOG_EXITFN(expected);
}

BOOL UeSession::isPrevProcRsp(GtpMsg *rspMsg)
{
   LOG_ENTERFN();

   BOOL prevProcRsp = FALSE;

   if ((m_currTaskIndx > 0) && \
       (m_prevUeProc.rspType == rspMsg->type()) && \
       (m_prevUeProc.seqNumber == rspMsg->seqNumber()))
   {
      prevProcRsp = TRUE;
   }

   LOG_EXITFN(prevProcRsp);
}

BOOL UeSession::isPrevProcReq(GtpMsg *reqMsg)
{
   LOG_ENTERFN();

   BOOL prevProcReq = FALSE;

   if ((m_currTaskIndx > 0) && \
       (m_prevUeProc.reqType == reqMsg->type()) && \
       (m_prevUeProc.seqNumber == reqMsg->seqNumber()))
   {
      prevProcReq = TRUE;
   }

   LOG_EXITFN(prevProcReq);
}

PUBLIC RETVAL UeSession::handleIncRspMsg(GtpMsg *rspMsg, UdpData_t *rcvdData)
{
   LOG_ENTERFN();

   if (isExpectedRsp(rspMsg))
   {
      LOG_DEBUG("Expected response message received");

      m_prevUeProc.connId = rcvdData->connId;
      m_prevUeProc.seqNumber = m_currUeProc.seqNumber;
      m_prevUeProc.reqType = m_currReqType;
      m_prevUeProc.rspType = rspMsg->type();
      m_prevUeProc.procTask = m_currTask;
      UE_SSN_FINISH_REQ_TASK(this);

      m_currTask->m_numRcv++;
      decAndStoreGtpcIncMsg(m_pCurrPdn, rspMsg, &rcvdData->peerEp);
      GSIM_UNSET_MASK(this->m_bitmask, GSIM_UE_SSN_WAITING_FOR_RSP);

      delete m_currUeProc.sentMsg;
      m_currUeProc.sentMsg = NULL;
      UE_SSN_FINISH_RSP_TASK(this);

      if (IS_SCN_COMPLETED())
      {
         handleCompletedTask();
      }
   }
   else if (isPrevProcRsp(rspMsg))
   {
      /* may be a retransmitted response for previous procedure */
      LOG_DEBUG("Response Message for previous procedure received");
      m_prevUeProc.procTask->m_numRcvRetrans++;
   }
   else
   {
      /* unexpecte response message received */
      LOG_DEBUG("Unexpected response Message received");
      m_currTask->m_numUnexp++;
   }

   LOG_EXITFN(ROK);
}

RETVAL UeSession::handleWait()
{
   LOG_ENTERFN();

   m_wakeTime = m_lastRunTime + m_currTask->wait();
   UE_SSN_FINISH_TASK(this);

   /* pause the task until wake up time */
   pause();

   LOG_EXITFN(ROK);
}

VOID UeSession::storeRcvdMsg(UdpData_t *rcvdData)
{
   LOG_ENTERFN();

   LOG_EXITVOID();
}

/**
 * @brief
 *    Creates a new UE Session with imsi = imsiKey
 *    Used when the session is created by this simulator entity
 *
 * @param imsiKey
 * @param pGtpMsg
 *
 * @return 
 */
UeSession* UeSession::createUeSession(GtpImsiKey imsiKey)
{
   U8    *pImsi = imsiKey.val;

   Scenario *pScn = Scenario::getInstance();
   UeSession *pUeSsn = new UeSession(pScn, imsiKey);
   s_ueSessionMap.insert(UeSessionMapPair(imsiKey, pUeSsn));

   LOG_INFO("Creating UE Session [%x%x%x%x%x%x%x%x]",\
         pImsi[0], pImsi[1], pImsi[2], pImsi[3], pImsi[4], pImsi[5],\
         pImsi[6], pImsi[7]);

   return pUeSsn;
}

/**
 * @brief
 *    returns the ue session given the control plane teid
 *
 * @param teid
 *
 * @return 
 */
UeSession* UeSession::getUeSession(GtpTeid_t teid)
{
   LOG_ENTERFN();

   UeSession   *pUeSession = NULL;
   GtpcTun     *pCTun = findCTun(teid);

   if (NULL != pCTun)
   {
      pUeSession = pCTun->m_pUeSession;
   }

   LOG_EXITFN(pUeSession);
}

UeSession* UeSession::getUeSession(GtpImsiKey imsiKey)
{
   LOG_ENTERFN();

   UeSession *pUeSession = NULL;

   UeSessionMapItr   itr = s_ueSessionMap.find(imsiKey);
   if (itr != s_ueSessionMap.end())
   {
      pUeSession = itr->second;
   }

   LOG_EXITFN(pUeSession);
}

GtpcPdn* UeSession::createPdn()
{
   LOG_ENTERFN();

   GtpcPdn *pPdn = NULL;

   try
   {
      pPdn = new GtpcPdn;
      pPdn->pUeSession = this;

      LOG_DEBUG("Creating GTP-C Tunnel");
      pPdn->pCTun = createCTun(pPdn);
   }
   catch (exception &e)
   {
      LOG_FATAL("Memory allocation GtpcPdn");
      throw ERR_PDN_CREATION;
   }

   LOG_EXITFN(pPdn);
}

/**
 * @brief Creates bearer contexts by parsing Bearer Context IE of GTP
 *    message
 *
 * @param pPdn PDN which the bearers are associated
 * @param pGtpMsg Gtp Message
 * @param instance instance of the Bearer Context ID
 */
VOID UeSession::createBearers
(
   GtpcPdn        *pPdn,
   GtpMsg         *pGtpMsg,
   GtpInstance_t  instance
)
{
   LOG_ENTERFN();

   if (pGtpMsg->type() == GTPC_MSG_CS_REQ)
   {
      U32 bearerCnt = pGtpMsg->getIeCount(GTP_IE_BEARER_CNTXT, instance);
      for (U32 i = 1; i <= bearerCnt; i++)
      {
         GtpIe *pIe = pGtpMsg->getIe(GTP_IE_BEARER_CNTXT, instance, i);
         GtpBearerContext *pBearerCntxt = dynamic_cast<GtpBearerContext *>(pIe);

         GtpEbi_t ebi = pBearerCntxt->getEbi();

         GtpBearer *pBearer = new GtpBearer(pPdn, ebi);
         GSIM_SET_BEARER_MASK(pPdn->bearerMask, ebi);
         m_bearerVec[GTP_BEARER_INDEX(ebi)] = pBearer;
      }
   }

   LOG_EXITVOID();
}

VOID UeSession::decAndStoreGtpcIncMsg
(
GtpcPdn           *pPdn,
GtpMsg            *pGtpMsg,
const IPEndPoint  *pPeerEp
)
{
   LOG_ENTERFN();

   pGtpMsg->decode();
   GtpMsgType_t rcvdMsgTye = pGtpMsg->type();
   if (rcvdMsgTye == GTPC_MSG_CS_REQ || rcvdMsgTye == GTPC_MSG_CS_RSP)
   {
      GtpFteid *pFteid = dynamic_cast<GtpFteid *>\
            (pGtpMsg->getIe(GTP_IE_FTEID, 0, 1));
      pPdn->pCTun->m_remTeid = pFteid->getTeid();
   }

   pPdn->pCTun->m_peerEp.ipAddr.ipAddrType = IP_ADDR_TYPE_V4;
   pPdn->pCTun->m_peerEp.port = pPeerEp->port;
   pPdn->pCTun->m_peerEp.ipAddr = pPeerEp->ipAddr;

   if (pGtpMsg->type() == GTPC_MSG_CS_REQ)
   {
      createBearers(pPdn, pGtpMsg, 0);
   }

   LOG_EXITVOID();
}

VOID UeSession::encGtpcOutMsg(GtpcPdn *pPdn, GtpMsg *pGtpMsg,\
   Buffer *pGtpBuf, IPEndPoint *peerEp)
{
   LOG_ENTERFN();

   U8  buf[GTP_MSG_BUF_LEN];
   U32 len = 0;

   /* Modify the header parameters dynamically */
   GtpMsgHdr msgHdr;
   msgHdr.teid = pPdn->pCTun->m_remTeid;
   msgHdr.seqN = m_currUeProc.seqNumber;
   GSIM_SET_MASK(msgHdr.pres, GTP_MSG_HDR_TEID_PRES);
   GSIM_SET_MASK(msgHdr.pres, GTP_MSG_HDR_SEQ_PRES);
   pGtpMsg->setMsgHdr(&msgHdr);

   GtpMsgType_t msgType = pGtpMsg->type();
   if (GTPC_MSG_CS_REQ == msgType)
   {
      pGtpMsg->setImsi(&m_imsiKey);

      RETVAL ret = pGtpMsg->setSenderFteid(pPdn->pCTun->m_locTeid, \
            &pPdn->pCTun->m_localEp.ipAddr);
      if (ROK != ret)
      {
         LOG_ERROR("Encoding of sender Fteid Failed");
         throw ret;
      }
   }
   else if (GTPC_MSG_CS_RSP == msgType)
   {
      RETVAL ret = pGtpMsg->setSenderFteid(pPdn->pCTun->m_locTeid, \
            &pPdn->pCTun->m_localEp.ipAddr);
      if (ROK != ret)
      {
         LOG_ERROR("Encoding of sender Fteid Failed");
         throw ret;
      }
   }

   /* Modify the GTP-U TEID in all the bearers */
   U32 bearerCnt = pGtpMsg->getIeCount(GTP_IE_BEARER_CNTXT, 0);
   for (U32 i = 1; i <= bearerCnt; i++)
   {
      GtpIe *pIe = pGtpMsg->getIe(GTP_IE_BEARER_CNTXT, 0, i);
      GtpBearerContext *pBearerCntxt = dynamic_cast<GtpBearerContext *>(pIe);

      GtpBearer *pBearer = this->getBearer(pBearerCntxt->getEbi());
      pBearerCntxt->setGtpuTeid(pBearer->localTeid(), 0);
   }

   MEMSET(buf, 0, GTP_MSG_BUF_LEN);
   pGtpMsg->encode(buf, &len);

   BUFFER_CPY(pGtpBuf, buf, len);

   LOG_EXITVOID();
}

RETVAL UeSession::handleDeadCall(VOID *arg)
{
   LOG_ENTERFN();

   RETVAL ret = ROK;

   if (NULL == arg)
   {
      /* Session run invoked because of deadcall timer expiry */
      if (m_lastRunTime >= m_wakeTime)
      {
         ret = ROK_OVER;
      }
   }
   else
   {
      UdpData_t *data = (UdpData_t*)arg;
      GtpMsg gtpMsg(&data->buf);
      GtpMsgCategory_t msgCat = gtpGetMsgCategory(gtpMsg.type());

      if (GTP_MSG_CAT_REQ == msgCat)
      {
         if (isPrevProcReq(&gtpMsg))
         {
            m_prevUeProc.procTask->m_numRcvRetrans++;
            Buffer *buf = new Buffer(m_prevUeProc.sentMsg->buf);
            sendMsg(m_prevUeProc.sentMsg->connId,\
                  &m_prevUeProc.sentMsg->peerEp, buf);
         }
      }
      else if (GTP_MSG_CAT_RSP == msgCat)
      {
         if (isPrevProcRsp(&gtpMsg))
         {
            m_prevUeProc.procTask->m_numRcvRetrans++;
         }
      }

      delete data;
   }

   LOG_EXITFN(ret);
}

GtpBearer* UeSession::getBearer(GtpEbi_t ebi)
{
   LOG_ENTERFN();

   GtpBearer *pBearer = m_bearerVec[GTP_BEARER_INDEX(ebi)];

   LOG_EXITFN(pBearer);
}

GtpcPdnLst* UeSession::getPdnList()
{
   return &m_pdnLst;
}

/**
 * @brief Contructor
 *
 * @param pPdn
 * @param ebi
 */
GtpBearer::GtpBearer(GtpcPdn  *pPdn, GtpEbi_t ebi)
{
   m_pPdn = pPdn;
   m_ebi = ebi;
   m_pUTun = new GtpuTun;
}


/**
 * @brief Destructor
 */
GtpBearer::~GtpBearer()
{
   delete m_pUTun;
}

PUBLIC VOID cleanupUeSessions()
{
   UeSessionMapItr ueItr = s_ueSessionMap.begin();
   for (; ueItr != s_ueSessionMap.end(); ueItr++)
   {
      delete ueItr->second;
   }
}

PUBLIC GtpcTun* getS11S4CTun(UeSession *pUeSession)
{
   LOG_ENTERFN();

   GtpcPdnLst  *pPdnLst = pUeSession->getPdnList();
   GtpcTun     *pCTun = NULL;
   
   for (GtpcPdnLstItr b = pPdnLst->begin(); b != pPdnLst->end(); b++)
   {
      pCTun = (*b)->pCTun;
   }

   LOG_EXITFN(pCTun);
}

GtpcTun* UeSession::createCTun(GtpcPdn *pPdn)
{
   LOG_ENTERFN();

   GtpcTun     *pCTun = NULL;

   try
   {
      GtpIfType_t ifType = m_pScn->ifType();
      if (GTP_IF_S11_C_MME == ifType)
      {
         pCTun = getS11S4CTun(pPdn->pUeSession);
         if (NULL != pCTun)
         {
            /* S11/S4 tunnel already exists, increase the reference count */
            pCTun->m_refCount++;
         }
         else
         {
            /* This is the first C tun over S11/S4 interface, so create
             * new C tunnel 
             */
            pCTun = new GtpcTun();
            pCTun->m_pPdn = pPdn;
            pCTun->m_pUeSession = pPdn->pUeSession;
         }
      }
      else
      {
         pCTun = new GtpcTun();  
         pCTun->m_pPdn = pPdn;
         pCTun->m_pUeSession = pPdn->pUeSession;
      }
   }
   catch (exception &e)
   {
      throw ERR_CTUN_CREATION;
   }

   LOG_EXITFN(pCTun);
}

VOID UeSession::handleCompletedTask()
{
   LOG_ENTERFN();

   /* finished processing all messages in the scenario, delete task */
   LOG_DEBUG("Scenario end for UE, IMSI [%x%x%x%x%x%x%x%x]",\
         m_imsiKey.val[0], m_imsiKey.val[1], m_imsiKey.val[2],\
         m_imsiKey.val[3], m_imsiKey.val[4], m_imsiKey.val[5],\
         m_imsiKey.val[6], m_imsiKey.val[7]);

   Stats::incStats(GSIM_STAT_NUM_SESSIONS_SUCC);
   Stats::decStats(GSIM_STAT_NUM_SESSIONS);

   /* the scenario for this UE session is complete, wait for deal-call
    * timer expiry to cleanup the sessions. This is required to handle
    * any delayed or retransmitted response or request messages
    */
   GSIM_SET_MASK(m_bitmask, GSIM_UE_SSN_SCN_COMPLETE);
   m_wakeTime = m_lastRunTime + m_deadCallWait;
   pause();

   LOG_EXITVOID();
}
