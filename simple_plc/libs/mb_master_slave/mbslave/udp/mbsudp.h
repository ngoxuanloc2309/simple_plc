/*
 * MODBUS Slave Library: A portable MODBUS slave for MODBUS ASCII/RTU/TCP.
 * Copyright (c) 2008 Christian Walter <cwalter@embedded-solutions.at>
 * Copyright (c) 2020 Embedded Experts GmbH <modbus@embedded-experts.at>
 *
 * All rights reserved. Use of this file is subject to license terms.
 */

#ifndef _MBSUDP_H
#define _MBSUDP_H

#ifdef __cplusplus
PR_BEGIN_EXTERN_C
#endif

#if MBS_UDP_ENABLED == 1

/*!
 * \if INTERNAL_DOCS
 * \addtogroup mbs_udp_int
 * @{
 * \endif
 */

/* ----------------------- Defines ------------------------------------------*/

/* ----------------------- Type definitions ---------------------------------*/

/* ----------------------- Function prototypes ------------------------------*/
eMBErrorCode
eMBSUDPIntInit( xMBSInternalHandle *pxIntHdl, CHAR *pcBindAddress, USHORT usUDPPort );

/*!
 * \if INTERNAL_DOCS
 * @}
 * \endif
 */

#endif

#ifdef __cplusplus
PR_END_EXTERN_C
#endif

#endif
