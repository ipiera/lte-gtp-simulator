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

#ifndef _TRANSPORT_HPP_
#define _TRANSPORT_HPP_

EXTERN RETVAL initTransport();

EXTERN RETVAL setupStdinSock();

EXTERN RETVAL sendMsg
(
TransConnId          connId,
IPEndPoint           *pDst,
Buffer               *pBuf
);

EXTERN VOID socketPoll(S32 wait);

#endif
