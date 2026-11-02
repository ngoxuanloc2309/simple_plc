/*
 * MODBUS Slave Library: A portable MODBUS slave for MODBUS ASCII/RTU/TCP.
 * Copyright (c) 2008 Christian Walter <cwalter@embedded-solutions.at>
 * Copyright (c) 2020 Embedded Experts GmbH <modbus@embedded-experts.at>
 *
 * All rights reserved. Use of this file is subject to license terms.
 */

/* ----------------------- System includes ----------------------------------*/
#include <stdlib.h>
#include <string.h>

/* ----------------------- Platform includes --------------------------------*/
#include "mbport.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mbs.h"
#include "common/mbportlayer.h"
#include "common/mbutils.h"
#include "internal/mbsiframe.h"
#include "internal/mbsi.h"
#if MBS_UDP_ENABLED == 1

/* ----------------------- MBAP Header --------------------------------------*/
/*
 *
 * <------------------------ MODBUS UDP/IP ADU(1) ------------------------->
 *              <----------- MODBUS PDU (1') ---------------->
 *  +-----------+---------------+------------------------------------------+
 *  | TID | PID | Length | UID  |Code | Data                               |
 *  +-----------+---------------+------------------------------------------+
 *  |     |     |        |      |
 * (2)   (3)   (4)      (5)    (6)
 *
 * (2)  ... MBS_UDP_TID_OFF     = 0 (Transaction Identifier - 2 Byte)
 * (3)  ... MBS_UDP_PID_OFF     = 2 (Protocol Identifier - 2 Byte)
 * (4)  ... MBS_UDP_LEN_OFF     = 4 (Number of bytes - 2 Byte)
 * (5)  ... MBS_UDP_UID_OFF     = 6 (Unit Identifier - 1 Byte)
 * (6)  ... MBS_UDP_PDU_OFF     = 7 (MODBUS PDU)
 *
 * (1)  ... MODBUS UDP/IP ADU (application data unit)
 * (1') ... MODBUS PDU (protocol data unit)
 */

#define MBS_UDP_TID_OFF          ( 0 )
#define MBS_UDP_PID_OFF          ( 2 )
#define MBS_UDP_LEN_OFF          ( 4 )
#define MBS_UDP_UID_OFF          ( 6 )
#define MBS_UDP_MBAP_HEADER_SIZE ( 7 )
#define MBS_UDP_MB_PDU_OFF       ( 7 )

#define MBS_UDP_PROTOCOL_ID ( 0 ) /* 0 = Modbus Protocol */

/* ----------------------- Defines ------------------------------------------*/
#define MBS_UDP_PDU_SIZE_MIN ( 7 )
#define MBS_UDP_PDU_SIZE_MAX ( 260 )

#define MBS_UDP_BUFFER_SIZE ( MBS_UDP_PDU_SIZE_MAX )

#define IDX_INVALID ( 255 )

/* ----------------------- Type definitions ---------------------------------*/
typedef struct
{
    UBYTE         ubIdx;
    xMBPUDPHandle xMBUDPServerHdl;
    UBYTE         arubBuffer[MBS_UDP_BUFFER_SIZE];
    USHORT        usBufferPos;
    CHAR *        pucClientAddress;
    USHORT        usClientPort;
} xMBSUDPFrameHandle;

/* ----------------------- Static variables ---------------------------------*/
STATIC BOOL               bIsInitialized = FALSE;
STATIC xMBSUDPFrameHandle arxMBSUDPFrameHdl[MBS_UDP_MAX_INSTANCES];

/* ----------------------- Static functions ---------------------------------*/
STATIC void
vMBSUDPResetHdl( xMBSUDPFrameHandle *pxFrameHdl, BOOL bClose );
STATIC void
vMBSUDPStateReset( xMBSUDPFrameHandle *pxFrameHdl );
STATIC eMBErrorCode
eMBPUDPClientNewDataCB( xMBHandle xHdl );

STATIC eMBErrorCode
eMBSUDPFrameSend( xMBSHandle xHdl, USHORT usMBPDULength );
STATIC eMBErrorCode
eMBSUDPFrameReceive( xMBSHandle xHdl, UBYTE *pubSlaveAddress, USHORT *pusMBPDULength ) MB_CDECL_SUFFIX;
STATIC eMBErrorCode
eMBSUDPFrameClose( xMBSHandle xHdl );

#if( defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 ) )
void
vMBSLogUDPFrame( eMBPortLogLevel eLevel, xMBSUDPFrameHandle *pxIntHdl, char *szMsg, const UBYTE *pubPayload,
                 USHORT usLength );
#endif

/* ----------------------- Start implementation -----------------------------*/

eMBErrorCode
eMBSUDPIntInit( xMBSInternalHandle *pxIntHdl, CHAR *pcBindAddress, USHORT usUDPPort )
{
    eMBErrorCode        eStatus = MB_EINVAL;
    xMBSUDPFrameHandle *pxFrameHdl = NULL;
    UBYTE               ubIdx;

#if MBS_ENABLE_FULL_API_CHECKS == 1
    if( bMBSIsHdlValid( pxIntHdl ) )
#else
    if( TRUE )
#endif
    {
        MBP_ENTER_CRITICAL_SECTION();
        if( !bIsInitialized )
        {
            for( ubIdx = 0; ubIdx < ( UBYTE )MB_UTILS_NARRSIZE( arxMBSUDPFrameHdl ); ubIdx++ )
            {
                arxMBSUDPFrameHdl[ubIdx].ubIdx = ubIdx;
                /* Initialze to NULL cause MBP_UDP_CLIENTADDR_FREE is used. */
                arxMBSUDPFrameHdl[ubIdx].pucClientAddress = NULL;
                vMBSUDPResetHdl( &arxMBSUDPFrameHdl[ubIdx], FALSE );
            }
            bIsInitialized = TRUE;
        }

        for( ubIdx = 0; ubIdx < ( UBYTE )MB_UTILS_NARRSIZE( arxMBSUDPFrameHdl ); ubIdx++ )
        {
            if( IDX_INVALID == arxMBSUDPFrameHdl[ubIdx].ubIdx )
            {
                pxFrameHdl = &arxMBSUDPFrameHdl[ubIdx];
                pxFrameHdl->ubIdx = ubIdx;
                break;
            }
        }
        MBP_EXIT_CRITICAL_SECTION();

        if( NULL != pxFrameHdl )
        {
            eStatus = MB_EPORTERR;
            if( MB_ENOERR == eMBPUDPClientInit( &( pxFrameHdl->xMBUDPServerHdl ), pxIntHdl, pcBindAddress, usUDPPort,
                                                eMBPUDPClientNewDataCB ) )
            {
                pxIntHdl->pubFrameMBPDUBuffer = NULL;
                pxIntHdl->pFrameSendFN = eMBSUDPFrameSend;
                pxIntHdl->pFrameRecvFN = eMBSUDPFrameReceive;
                pxIntHdl->pFrameCloseFN = eMBSUDPFrameClose;
                pxIntHdl->xFrameHdl = pxFrameHdl;
                eStatus = MB_ENOERR;
            }
            else
            {
                MBP_ASSERT( ubIdx < MBS_TCP_MAX_INSTANCES );
                vMBSUDPResetHdl( &arxMBSUDPFrameHdl[ubIdx], TRUE );
            }
        }
        else
        {
            eStatus = MB_ENORES;
        }
    }
    else
    {
        eStatus = MB_EINVAL;
    }
    return eStatus;
}

STATIC eMBErrorCode
eMBSUDPFrameSend( xMBSHandle xHdl, USHORT usMBPDULength )
{
    eMBErrorCode        eStatus = MB_EINVAL, eStatus2;
    xMBSInternalHandle *pxIntHdl = xHdl;
    xMBSUDPFrameHandle *pxFrameHdl;
    USHORT              usMBAPLengthField;
    USHORT              usLen;
#if MBS_ENABLE_PROT_ANALYZER_INTERFACE == 1
    xMBAnalyzerFrame xAnalyzerFrame;
    xMBPTimeStamp    xTimeStamp;
#endif
#if MBS_ENABLE_FULL_API_CHECKS == 1
    if( bMBSIsHdlValid( pxIntHdl ) )
#else
    if( TRUE )
#endif
    {
        if( MB_IS_VALID_HDL( ( xMBSUDPFrameHandle * )pxIntHdl->xFrameHdl, arxMBSUDPFrameHdl ) )
        {
            pxFrameHdl = pxIntHdl->xFrameHdl;
            MBP_ENTER_CRITICAL_SECTION();
            if( 0 == usMBPDULength )
            {
                /* Reset of state variables is done at end of function. */
                eStatus = MB_ENOERR;
            }
            else if( ( NULL != pxFrameHdl->pucClientAddress ) && ( usMBPDULength >= MB_PDU_SIZE_MIN ) &&
                     ( usMBPDULength <= MB_PDU_SIZE_MAX ) )
            {
                usMBAPLengthField = ( USHORT )( usMBPDULength + 1 );
                pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF] = ( UBYTE )( usMBAPLengthField >> 8U );
                pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF + 1] = ( UBYTE )( usMBAPLengthField & 0xFF );
                usLen = usMBPDULength + MBS_UDP_MBAP_HEADER_SIZE;
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_UDP ) )
                {
                    vMBSLogUDPFrame( MB_LOG_DEBUG, pxFrameHdl,
                                     "Sending frame: ", ( const UBYTE * )&pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF],
                                     usLen );
                }
#endif
                eStatus2 = eMBPUDPConWrite( pxFrameHdl->xMBUDPServerHdl, pxFrameHdl->pucClientAddress,
                                            pxFrameHdl->usClientPort, pxFrameHdl->arubBuffer, usLen );
                switch( eStatus2 )
                {
                    case MB_ENOERR:
#if MBS_ENABLE_STATISTICS_INTERFACE == 1
                        /* Account for sent frame and bytes. */
                        pxIntHdl->xFrameStat.ulNBytesSent += usLen;
                        pxIntHdl->xFrameStat.ulNPacketsSent += 1;
#endif
#if MBS_ENABLE_PROT_ANALYZER_INTERFACE == 1
                        /* Pass frame to protocol analyzer. */
                        xAnalyzerFrame.eFrameDir = MB_FRAME_SEND;
                        xAnalyzerFrame.eFrameType = MB_FRAME_UDP;
                        /* Its better to take these values directly out of the request
                         * in case this part changes later because otherwise bugs could
                         * be introduced easily.
                         */
                        xAnalyzerFrame.x.xUDPHeader.usMBAPTransactionId =
                            ( USHORT )( pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF] << 8U );
                        xAnalyzerFrame.x.xUDPHeader.usMBAPTransactionId |=
                            ( USHORT )( pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF + 1] );
                        xAnalyzerFrame.x.xUDPHeader.usMBAPProtocolId = MBS_UDP_PROTOCOL_ID;
                        xAnalyzerFrame.x.xUDPHeader.usMBAPLength =
                            ( USHORT )( pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF] << 8U );
                        ;
                        xAnalyzerFrame.x.xUDPHeader.usMBAPLength |=
                            ( USHORT )( pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF + 1] );
                        ;
                        xAnalyzerFrame.x.xUDPHeader.ubUnitIdentifier = pxFrameHdl->arubBuffer[MBS_UDP_UID_OFF];
                        xAnalyzerFrame.ubDataPayload = ( const UBYTE * )&pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF];
                        xAnalyzerFrame.usDataPayloadLength = usLen;
                        if( NULL != pxIntHdl->pvMBAnalyzerCallbackFN )
                        {
                            vMBPGetTimeStamp( &xTimeStamp );
                            pxIntHdl->pvMBAnalyzerCallbackFN( pxIntHdl, pxIntHdl->pvCtx, &xTimeStamp, &xAnalyzerFrame );
                        }
#endif
                        eStatus = MB_ENOERR;
                        break;
                    case MB_EIO:
                        eStatus = MB_EIO;
                        break;
                    default:
                        eStatus = MB_EPORTERR;
                        break;
                }
            }
            else
            {
                eStatus = MB_EINVAL;
            }

            pxIntHdl->pubFrameMBPDUBuffer = NULL;
            vMBSUDPStateReset( pxFrameHdl );

            MBP_EXIT_CRITICAL_SECTION();
        }
    }
    return eStatus;
}

STATIC eMBErrorCode
eMBSUDPFrameReceive( xMBSHandle xHdl, UBYTE *pubSlaveAddress, USHORT *pusMBPDULength ) MB_CDECL_SUFFIX
{
    eMBErrorCode        eStatus = MB_EINVAL;
    xMBSInternalHandle *pxIntHdl = xHdl;
    xMBSUDPFrameHandle *pxFrameHdl;

    USHORT usMBAPTransactionIDField;
    USHORT usMBAPLengthField;
    UBYTE  ubMBAPUnitIDield;
    USHORT usMBAPProtocolIDField;
    BOOL   bFrameDamaged = FALSE;
#if MBS_ENABLE_PROT_ANALYZER_INTERFACE == 1
    xMBAnalyzerFrame xAnalyzerFrame;
    xMBPTimeStamp    xTimeStamp;
#endif
#if MBS_ENABLE_FULL_API_CHECKS == 1
    if( bMBSIsHdlValid( pxIntHdl ) )
#else
    if( TRUE )
#endif
    {
        if( MB_IS_VALID_HDL( ( xMBSUDPFrameHandle * )pxIntHdl->xFrameHdl, arxMBSUDPFrameHdl ) )
        {
            eStatus = MB_EIO;
            pxFrameHdl = pxIntHdl->xFrameHdl;

            MBP_ENTER_CRITICAL_SECTION();
            /* Check if there is an client active. */
            if( NULL != pxFrameHdl->pucClientAddress )
            {
                MBP_EXIT_CRITICAL_SECTION();
                bFrameDamaged = TRUE;
                if( pxFrameHdl->usBufferPos >= MB_PDU_SIZE_MIN )
                {
                    usMBAPTransactionIDField = ( USHORT )( ( USHORT )pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF] << 8 );
                    usMBAPTransactionIDField |= ( USHORT )pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF + 1];
                    ( void )usMBAPTransactionIDField;
                    usMBAPProtocolIDField = ( USHORT )( ( USHORT )pxFrameHdl->arubBuffer[MBS_UDP_PID_OFF] << 8 );
                    usMBAPProtocolIDField |= ( USHORT )pxFrameHdl->arubBuffer[MBS_UDP_PID_OFF + 1];
                    usMBAPLengthField = ( USHORT )( ( USHORT )pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF] << 8 );
                    usMBAPLengthField |= ( USHORT )pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF + 1];
                    ubMBAPUnitIDield = pxFrameHdl->arubBuffer[MBS_UDP_UID_OFF];

                    if( ( MBS_UDP_PROTOCOL_ID == usMBAPProtocolIDField ) &&
                        ( usMBAPLengthField > ( 1 + MB_PDU_SIZE_MIN ) ) &&
                        ( usMBAPLengthField < ( 1 + MB_PDU_SIZE_MAX ) ) &&
                        ( ( USHORT )( usMBAPLengthField + MBS_UDP_UID_OFF ) == pxFrameHdl->usBufferPos ) )
                    {
                        /* Frame is valid. */
                        bFrameDamaged = FALSE;

#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                        if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_UDP ) )
                        {
                            vMBSLogUDPFrame( MB_LOG_DEBUG, pxFrameHdl, "Received frame: ",
                                             ( const UBYTE * )&pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF],
                                             pxFrameHdl->usBufferPos );
                        }
#endif
#if MBS_ENABLE_STATISTICS_INTERFACE == 1
                        pxIntHdl->xFrameStat.ulNPacketsReceived += 1;
#endif
#if MBS_ENABLE_PROT_ANALYZER_INTERFACE == 1
                        xAnalyzerFrame.eFrameType = MB_FRAME_UDP;
                        xAnalyzerFrame.x.xUDPHeader.usMBAPTransactionId = usMBAPTransactionIDField;
                        xAnalyzerFrame.x.xUDPHeader.usMBAPProtocolId = usMBAPProtocolIDField;
                        xAnalyzerFrame.x.xUDPHeader.usMBAPLength =
                            ( USHORT )( pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF] << 8U );
                        xAnalyzerFrame.x.xUDPHeader.usMBAPLength |=
                            ( USHORT )( pxFrameHdl->arubBuffer[MBS_UDP_LEN_OFF + 1] );
                        xAnalyzerFrame.x.xUDPHeader.ubUnitIdentifier = pxFrameHdl->arubBuffer[MBS_UDP_UID_OFF];
#endif
                        if( MB_SER_BROADCAST_ADDR == ubMBAPUnitIDield )
                        {
                            *pubSlaveAddress = MBS_ANY_ADDR;
                        }
                        else
                        {
                            *pubSlaveAddress = ubMBAPUnitIDield;
                        }
                        *pusMBPDULength = ( USHORT )( usMBAPLengthField - 1 );
                        pxIntHdl->pubFrameMBPDUBuffer = &pxFrameHdl->arubBuffer[MBS_UDP_MB_PDU_OFF];
                        eStatus = MB_ENOERR;
                    }
                }

                if( bFrameDamaged )
                {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                    if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_UDP ) )
                    {
                        vMBSLogUDPFrame(
                            MB_LOG_DEBUG, pxFrameHdl, "Received frame with incorrect length or protocol id: ",
                            ( const UBYTE * )&pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF], pxFrameHdl->usBufferPos );
                    }
#endif
#if MBS_ENABLE_STATISTICS_INTERFACE == 1
                    /* Account for checksum failure. */
                    pxIntHdl->xFrameStat.ulNChecksumErrors += 1;
#endif
#if MBS_ENABLE_PROT_ANALYZER_INTERFACE == 1
                    xAnalyzerFrame.eFrameType = MB_FRAME_DAMAGED;
#endif
                    eStatus = MB_EIO;
                }

#if MBS_ENABLE_PROT_ANALYZER_INTERFACE == 1
                xAnalyzerFrame.eFrameDir = MB_FRAME_RECEIVE;
                xAnalyzerFrame.ubDataPayload = ( const UBYTE * )&pxFrameHdl->arubBuffer[MBS_UDP_TID_OFF];
                xAnalyzerFrame.usDataPayloadLength = pxFrameHdl->usBufferPos;
                if( NULL != pxIntHdl->pvMBAnalyzerCallbackFN )
                {
                    vMBPGetTimeStamp( &xTimeStamp );
                    pxIntHdl->pvMBAnalyzerCallbackFN( pxIntHdl, pxIntHdl->pvCtx, &xTimeStamp, &xAnalyzerFrame );
                }
#endif
            }
            else
            {
                MBP_EXIT_CRITICAL_SECTION();
            }
        }
    }
    return eStatus;
}

STATIC eMBErrorCode
eMBSUDPFrameClose( xMBSHandle xHdl )
{
    eMBErrorCode        eStatus = MB_EINVAL;
    xMBSInternalHandle *pxIntHdl = xHdl;
    xMBSUDPFrameHandle *pxFrameHdl;

    MBP_ENTER_CRITICAL_SECTION();
#if MBS_ENABLE_FULL_API_CHECKS == 1
    if( bMBSIsHdlValid( pxIntHdl ) )
#else
    if( TRUE )
#endif
    {
        pxFrameHdl = pxIntHdl->xFrameHdl;
        if( MB_IS_VALID_HDL( pxFrameHdl, arxMBSUDPFrameHdl ) )
        {
            vMBSUDPResetHdl( pxFrameHdl, TRUE );
            eStatus = MB_ENOERR;
        }
    }
    MBP_EXIT_CRITICAL_SECTION();
    return eStatus;
}

STATIC eMBErrorCode
eMBPUDPClientNewDataCB( xMBHandle xHdl )
{
    eMBErrorCode        eStatus = MB_EINVAL;
    xMBSInternalHandle *pxIntHdl = xHdl;
    xMBSUDPFrameHandle *pxFrameHdl;

    MBP_ENTER_CRITICAL_SECTION();
#if MBS_ENABLE_FULL_API_CHECKS == 1
    if( bMBSIsHdlValid( pxIntHdl ) )
#else
    if( TRUE )
#endif
    {
        if( MB_IS_VALID_HDL( ( xMBSUDPFrameHandle * )pxIntHdl->xFrameHdl, arxMBSUDPFrameHdl ) )
        {
            pxFrameHdl = pxIntHdl->xFrameHdl;
            /* Check if this instance is not busy */
            if( NULL == pxFrameHdl->pucClientAddress )
            {
                eStatus = eMBPUDPConReadExt( pxFrameHdl->xMBUDPServerHdl, &pxFrameHdl->pucClientAddress,
                                             &pxFrameHdl->usClientPort, &pxFrameHdl->arubBuffer[0],
                                             &pxFrameHdl->usBufferPos, MBS_UDP_BUFFER_SIZE );
                switch( eStatus )
                {
                    case MB_ENOERR:
#if MBS_ENABLE_STATISTICS_INTERFACE == 1
                        pxIntHdl->xFrameStat.ulNBytesReceived += pxFrameHdl->usBufferPos;
#endif
                        break;
                    default:
                    case MB_EIO:
                        break;
                }
                if( MB_ENOERR == eStatus )
                {
                    if( MB_ENOERR != eMBPEventPost( pxIntHdl->xFrameEventHdl, ( xMBPEventType )MBS_EV_RECEIVED ) )
                    {
                        eStatus = MB_EPORTERR;
                    }
                }
                /* Cleanup the state in case of an error */
                if( MB_ENOERR != eStatus )
                {
                    vMBSUDPStateReset( pxFrameHdl );
                }
            }
            else
            {
                eStatus = MB_EAGAIN;
            }
        }
    }
    MBP_EXIT_CRITICAL_SECTION();

    return eStatus;
}

STATIC void
vMBSUDPResetHdl( xMBSUDPFrameHandle *pxFrameHdl, BOOL bClose )
{
    eMBErrorCode eStatus;

    if( MB_IS_VALID_HDL( pxFrameHdl, arxMBSUDPFrameHdl ) )
    {
        if( bClose && ( MBP_UDPHDL_INVALID != pxFrameHdl->xMBUDPServerHdl ) )
        {
            eStatus = eMBPUDPClientClose( pxFrameHdl->xMBUDPServerHdl );
            MBP_ASSERT( MB_ENOERR == eStatus );
        }
        pxFrameHdl->xMBUDPServerHdl = MBP_UDPHDL_INVALID;
        vMBSUDPStateReset( pxFrameHdl );
        pxFrameHdl->ubIdx = IDX_INVALID;
    }
}

STATIC void
vMBSUDPStateReset( xMBSUDPFrameHandle *pxFrameHdl )
{
    MBP_UDP_CLIENTADDR_FREE( pxFrameHdl->pucClientAddress );
    pxFrameHdl->usBufferPos = 0;
    pxFrameHdl->usClientPort = 0;
}

#if( defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 ) )
void
vMBSLogUDPFrame( eMBPortLogLevel eLevel, xMBSUDPFrameHandle *pxIntHdl, char *szMsg, const UBYTE *pubPayload,
                 USHORT usLength )
{
    USHORT usIdx;
    CHAR   arubBuffer[MBS_UDP_BUFFER_SIZE * 2U + 1];

    MBP_ASSERT( usLength < MBS_UDP_BUFFER_SIZE );
    if( usLength > 0 )
    {
        for( usIdx = 0; usIdx < usLength; usIdx++ )
        {
            sprintf( &arubBuffer[usIdx * 2], "%02X", pubPayload[usIdx] );
        }
    }
    else
    {
        strcpy( arubBuffer, "empty" );
    }
    vMBPPortLog( eLevel, MB_LOG_UDP, "[IDX=%hu] %s%s\n", ( USHORT )pxIntHdl->ubIdx, szMsg, arubBuffer );
}
#endif

#endif
