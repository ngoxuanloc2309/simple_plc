/*
 * MODBUS Slave Library: A portable MODBUS slave for MODBUS ASCII/RTU/TCP.
 * Copyright (c) 2008 Christian Walter <cwalter@embedded-solutions.at>
 * Copyright (c) 2020 Embedded Experts GmbH <modbus@embedded-experts.at>
 *
 * All rights reserved. Use of this file is subject to license terms.
 */

/* ----------------------- System includes ----------------------------------*/
#include <stdlib.h>

/* ----------------------- Platform includes --------------------------------*/
#include "mbport.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mbs.h"
#include "common/mbutils.h"
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
#include "common/mbportlayer.h"
#endif
#include "internal/mbsiframe.h"
#include "internal/mbsi.h"
#include "mbsfunctions.h"

/* ----------------------- Defines ------------------------------------------*/
#define MB_PDU_FUNC_DEVID_REQ_SIZE         ( 3 )
#define MB_PDU_FUNC_DEVID_REQ_MEI_OFF      ( MB_PDU_DATA_OFF )
#define MB_PDU_FUNC_DEVID_REQ_DEVID_OFF    ( MB_PDU_FUNC_DEVID_REQ_MEI_OFF + 1 )
#define MB_PDU_FUNC_DEVID_REQ_OBJECTID_OFF ( MB_PDU_FUNC_DEVID_REQ_DEVID_OFF + 1 )

#define MB_PDU_FUNC_DEVID_RESP_MEI_OFF           ( MB_PDU_DATA_OFF )
#define MB_PDU_FUNC_DEVID_RESP_DEVID_OFF         ( MB_PDU_FUNC_DEVID_RESP_MEI_OFF + 1 )
#define MB_PDU_FUNC_DEVID_RESP_CONFORMITY_OFF    ( MB_PDU_FUNC_DEVID_RESP_DEVID_OFF + 1 )
#define MB_PDU_FUNC_DEVID_RESP_MORE_FOLLOWS_OFF  ( MB_PDU_FUNC_DEVID_RESP_CONFORMITY_OFF + 1 )
#define MB_PDU_FUNC_DEVID_RESP_NEXT_OBJECTID_OFF ( MB_PDU_FUNC_DEVID_RESP_MORE_FOLLOWS_OFF + 1 )
#define MB_PDU_FUNC_DEVID_RESP_NOBJECTS_OFF      ( MB_PDU_FUNC_DEVID_RESP_NEXT_OBJECTID_OFF + 1 )
#define MB_PDU_FUNC_DEVID_RESP_OBJECTS_OFF       ( MB_PDU_FUNC_DEVID_RESP_NOBJECTS_OFF + 1 )

#define MB_PDU_FUNC_READ_DEVID_DEVID_BASIC_DEVICE_IDENTIFICATION    0x01
#define MB_PDU_FUNC_READ_DEVID_DEVID_REGULAR_DEVICE_IDENTIFICATION  0x02
#define MB_PDU_FUNC_READ_DEVID_DEVID_EXTENDED_DEVICE_IDENTIFICATION 0x03
#define MB_PDU_FUNC_READ_DEVID_DEVID_SPECIFIC_IDENTIFICATION_OBJECT 0x04

#define MB_PDU_FUNC_DIAG_MIN_REQ_SIZE        ( 2 )
#define MB_PDU_FUNC_DIAG_REQ_SUBFUNCTION_OFF ( MB_PDU_DATA_OFF )
#define MB_PDU_FUNC_DIAG_REQ_DATA_OFF        ( MB_PDU_FUNC_DIAG_REQ_SUBFUNCTION_OFF + 2 )
/* ----------------------- Type definitions ---------------------------------*/

/* ----------------------- Static variables ---------------------------------*/

/* ----------------------- Static functions ---------------------------------*/

/* ----------------------- Start implementation -----------------------------*/
#if MBS_ENABLE_SER_DIAG == 1
eMBException
eMBSFuncGetCommEventCounter( UBYTE *pubMBPDU, USHORT *pusMBPDULen,
                             const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    UBYTE *      pubFrameCur;
    eMBException eStatus = MB_PDU_EX_SLAVE_DEVICE_FAILURE;

    /* Length check */
    if( MB_PDU_SIZE_MIN == *pusMBPDULen )
    {
        /* Set the current PDU data pointer to the beginning. */
        pubFrameCur = &pubMBPDU[MB_PDU_FUNC_OFF];
        *pusMBPDULen = MB_PDU_FUNC_OFF;
        /* First byte contains the function code. */
        *pubFrameCur++ = MBS_FUNC_GET_COMM_EVENT_COUNTER;
        *pusMBPDULen += ( USHORT )1;
        /* Device is not busy (We do not support this */
        *pubFrameCur++ = 0x00;
        *pubFrameCur++ = 0x00;
        *pusMBPDULen += ( USHORT )2;
        /* Write comm event counter */
        *pubFrameCur++ = ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.usEventCount >> 8U );
        *pubFrameCur++ = ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.usEventCount & 0xFFU );
        *pusMBPDULen += ( USHORT )2;
        eStatus = MB_PDU_EX_NONE;
    }
    return eStatus;
}

eMBException
eMBSFuncGetCommEventLog( UBYTE *pubMBPDU, USHORT *pusMBPDULen, const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    UBYTE *      pubFrameCur;
    eMBException eStatus = MB_PDU_EX_SLAVE_DEVICE_FAILURE;
    UBYTE        ubCnt, ubCurIdx;
    /* Length check */
    if( MB_PDU_SIZE_MIN == *pusMBPDULen )
    {
        /* Set the current PDU data pointer to the beginning. */
        pubFrameCur = &pubMBPDU[MB_PDU_FUNC_OFF];
        *pusMBPDULen = MB_PDU_FUNC_OFF;
        /* First byte contains the function code. */
        *pubFrameCur++ = MBS_FUNC_GET_COMM_EVENT_LOG;
        *pusMBPDULen += ( USHORT )1;
        /* Device is not busy (We do not support this */
        *pubFrameCur++ = 0x00;
        *pubFrameCur++ = 0x00;
        *pusMBPDULen += ( USHORT )2;
        /* Write comm event counter */
        *pubFrameCur++ = ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.usEventCount >> 8U );
        *pubFrameCur++ = ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.usEventCount & 0xFFU );
        *pusMBPDULen += ( USHORT )2;
        /* Write comm message counter */
        *pubFrameCur++ = ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNPacketsReceived >> 8U );
        *pubFrameCur++ = ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNPacketsReceived & 0xFFU );
        *pusMBPDULen += ( USHORT )2;
        for( ubCnt = 0; ubCnt < MB_UTILS_NARRSIZE( pxMBSRegisterCB->pxIntHdl->arubEventLog ); ubCnt++ )
        {
            ubCurIdx = ( UBYTE )( ( ( pxMBSRegisterCB->pxIntHdl->arubEventLogCurPos +
                                      MB_UTILS_NARRSIZE( pxMBSRegisterCB->pxIntHdl->arubEventLog ) ) -
                                    ubCnt ) -
                                  1 );
            ubCurIdx = ( UBYTE )( ubCurIdx % MB_UTILS_NARRSIZE( pxMBSRegisterCB->pxIntHdl->arubEventLog ) );
            *pubFrameCur++ = pxMBSRegisterCB->pxIntHdl->arubEventLog[ubCurIdx];
            *pusMBPDULen += ( USHORT )1;
            if( 0x00 == pxMBSRegisterCB->pxIntHdl->arubEventLog[ubCurIdx] )
            {
                break;
            }
        }
        eStatus = MB_PDU_EX_NONE;
    }
    return eStatus;
}

eMBException
eMBSFuncReadDeviceIdentification( UBYTE *pubMBPDU, USHORT *pusMBPDULen,
                                  const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    UBYTE        ubMEIType;
    UBYTE        ubReadDeviceIDCode;
    UBYTE        ubObjectID, ubObjectsWritten, ubLastObjectIDToQuery;
    USHORT       usCurObjectID;
    eMBException eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS, eStatus2;
    UBYTE        ubCurPDUPos;
    USHORT       usBytesWritten;
    USHORT       usNBytesLeft;
    /* Length check */
    if( ( MB_PDU_FUNC_DEVID_REQ_SIZE + MB_PDU_SIZE_MIN ) == *pusMBPDULen )
    {
        ubMEIType = pubMBPDU[MB_PDU_FUNC_DEVID_REQ_MEI_OFF];
        ubReadDeviceIDCode = pubMBPDU[MB_PDU_FUNC_DEVID_REQ_DEVID_OFF];
        ubObjectID = pubMBPDU[MB_PDU_FUNC_DEVID_REQ_OBJECTID_OFF];
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
        if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
        {
            vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                         "Read device identification (MEI=" MBP_FORMAT_UINT_AS_HEXBYTE
                         ", Device ID Code = " MBP_FORMAT_UINT_AS_HEXBYTE ", Object ID = " MBP_FORMAT_UINT_AS_HEXBYTE
                         ")\n",
                         ubMEIType, ubReadDeviceIDCode, ubObjectID );
        }
#endif
        if( ( ubMEIType == 0x0E ) &&
            ( ubReadDeviceIDCode >= MB_PDU_FUNC_READ_DEVID_DEVID_BASIC_DEVICE_IDENTIFICATION ) &&
            ( ubReadDeviceIDCode <= MB_PDU_FUNC_READ_DEVID_DEVID_SPECIFIC_IDENTIFICATION_OBJECT ) )
        {
            switch( ubReadDeviceIDCode )
            {
                case MB_PDU_FUNC_READ_DEVID_DEVID_BASIC_DEVICE_IDENTIFICATION:
                    if( ubObjectID > ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_MAJORMINORREVISION )
                    {
                        ubObjectID = ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_VENDORNAME;
                    }
                    ubLastObjectIDToQuery = ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_MAJORMINORREVISION;
                    break;
                case MB_PDU_FUNC_READ_DEVID_DEVID_REGULAR_DEVICE_IDENTIFICATION:
                    if( ubObjectID > ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_USERAPPLICATIONNAME )
                    {
                        ubObjectID = ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_VENDORNAME;
                    }
                    ubLastObjectIDToQuery = ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_USERAPPLICATIONNAME;
                    break;
                case MB_PDU_FUNC_READ_DEVID_DEVID_EXTENDED_DEVICE_IDENTIFICATION:
                    ubLastObjectIDToQuery = ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_USER_END;
                    break;
                case MB_PDU_FUNC_READ_DEVID_DEVID_SPECIFIC_IDENTIFICATION_OBJECT:
                    ubLastObjectIDToQuery = ubObjectID;
                    break;
                default:
                    /* default case protected by above if */
                    ubLastObjectIDToQuery = ubObjectID;
                    MBP_ASSERT( 0 );
                    break;
            }

            /* Now prepare response */
            pubMBPDU[MB_PDU_FUNC_DEVID_RESP_MEI_OFF] = 0x0E;
            pubMBPDU[MB_PDU_FUNC_DEVID_RESP_DEVID_OFF] = ubReadDeviceIDCode;
            pubMBPDU[MB_PDU_FUNC_DEVID_RESP_CONFORMITY_OFF] = 0x83;
            pubMBPDU[MB_PDU_FUNC_DEVID_RESP_MORE_FOLLOWS_OFF] = 0x00;
            pubMBPDU[MB_PDU_FUNC_DEVID_RESP_NEXT_OBJECTID_OFF] = 0x00;
            pubMBPDU[MB_PDU_FUNC_DEVID_RESP_NOBJECTS_OFF] = 0x00;
            ubCurPDUPos = MB_PDU_FUNC_DEVID_RESP_OBJECTS_OFF;

#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
            if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
            {
                vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                             "  Returning objects from " MBP_FORMAT_USHORT " to " MBP_FORMAT_USHORT ".\n",
                             ( USHORT )ubObjectID, ( USHORT )ubLastObjectIDToQuery );
            }
#endif
            for( usCurObjectID = ( USHORT )ubObjectID, ubObjectsWritten = 0; usCurObjectID <= ubLastObjectIDToQuery; )
            {
                /* 2 bytes in addition are needed for describing the object */
                usNBytesLeft = ( USHORT )( ( MB_PDU_SIZE_MAX - ubCurPDUPos ) - 2 );
                if( NULL != pxMBSRegisterCB->peMBSSerDiagCB )
                {
                    eStatus2 =
                        pxMBSRegisterCB->peMBSSerDiagCB( ( eMBSSerialDiagQueryType_t )usCurObjectID,
                                                         &pubMBPDU[ubCurPDUPos + 2], &usBytesWritten, usNBytesLeft );

#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                    if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                    {
                        if( ( MB_PDU_EX_NONE == eStatus2 ) && ( NULL != pubMBPDU ) )
                        {
                            vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                         "  Value returned for object " MBP_FORMAT_USHORT ": (len=" MBP_FORMAT_USHORT
                                         " val=%*s)\n",
                                         ( USHORT )usCurObjectID, ( USHORT )usBytesWritten, ( int )usBytesWritten,
                                         ( char * )&pubMBPDU[ubCurPDUPos + 2] );
                        }
                        else
                        {
                            vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                         "  Exception returned for object " MBP_FORMAT_USHORT
                                         ": " MBP_FORMAT_UINT_AS_HEXBYTE "\n",
                                         ( USHORT )usCurObjectID, ( unsigned int )eStatus2 );
                        }
                    }
#endif
                }
                else
                {
                    eStatus2 = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                }

                /* This object is not mapped. In case we are doing a specific request we are done.
                 * Otherwise look for the next available element.
                 */
                if( MB_PDU_EX_ILLEGAL_DATA_ADDRESS == eStatus2 )
                {
                    if( ubReadDeviceIDCode == MB_PDU_FUNC_READ_DEVID_DEVID_SPECIFIC_IDENTIFICATION_OBJECT )
                    {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                        if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                        {
                            vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                         "  Object " MBP_FORMAT_USHORT
                                         " does not exist. Aborting cause specific object requested.\n",
                                         ( USHORT )usCurObjectID );
                        }
#endif
                        break;
                    }
                    /* If the first accessed element then restart with element 0 */
                    else if( ( ubObjectID != 0 ) && ( usCurObjectID == ubObjectID ) )
                    {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                        if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                        {
                            vMBPPortLog(
                                MB_LOG_DEBUG, MB_LOG_CORE,
                                "  Object " MBP_FORMAT_USHORT
                                " was first object and did not exist. Continuing with object " MBP_FORMAT_USHORT ".\n",
                                ( USHORT )usCurObjectID, ( USHORT )DEVICE_IDENTIFICATION_OBJECT_ID_VENDORNAME );
                        }
#endif
                        usCurObjectID = ubObjectID = ( UBYTE )DEVICE_IDENTIFICATION_OBJECT_ID_VENDORNAME;
                    }
                    /* Otherwise just look for the next available element. We can also
                     * skip some.
                     */
                    else
                    {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                        if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                        {
                            vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                         "  Object " MBP_FORMAT_USHORT " does not exist. Continuing.\n",
                                         ( USHORT )usCurObjectID );
                        }
#endif
                        usCurObjectID++;
                    }
                }
                /* We need to segment the request because there is not enough
                 * space in the buffer.
                 */
                else if( MB_PDU_EX_ILLEGAL_DATA_VALUE == eStatus2 )
                {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                    if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                    {
                        vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                     "  Object " MBP_FORMAT_USHORT
                                     " does not fit into current request. Splitting request.\n",
                                     ( USHORT )usCurObjectID );
                    }
#endif
                    /* Assert on this. This would imply that the function peMBSSerDiagCB
                     * wants to return a value which can never fit in the buffer.
                     */
                    MBP_ASSERT( ubCurPDUPos != MB_PDU_FUNC_DEVID_RESP_OBJECTS_OFF );
                    pubMBPDU[MB_PDU_FUNC_DEVID_RESP_MORE_FOLLOWS_OFF] = 0xFF;
                    pubMBPDU[MB_PDU_FUNC_DEVID_RESP_NEXT_OBJECTID_OFF] = ( UBYTE )usCurObjectID;
                    break;
                }
                else
                {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                    if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                    {
                        vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                     "  Object " MBP_FORMAT_USHORT
                                     " serialized into response at position " MBP_FORMAT_USHORT ". Continuing.\n",
                                     ( USHORT )usCurObjectID, ubCurPDUPos );
                    }
#endif
                    pubMBPDU[ubCurPDUPos] = ( UBYTE )usCurObjectID;
                    /*lint -save -esym(644, usBytesWritten)  usBytesWritten is customer callback */
                    pubMBPDU[ubCurPDUPos + 1] = ( UBYTE )usBytesWritten;
                    ubObjectsWritten++;
                    ubCurPDUPos += ( UBYTE )( usBytesWritten + 2 );
                    /*lint -restore */
                    MBP_ASSERT( MB_PDU_EX_NONE == eStatus2 );
                    usCurObjectID++;
                }
            }

            if( ubObjectsWritten > 0 )
            {
                eStatus = MB_PDU_EX_NONE;
                pubMBPDU[MB_PDU_FUNC_DEVID_RESP_NOBJECTS_OFF] = ubObjectsWritten;
                *pusMBPDULen = ( USHORT )ubCurPDUPos;
            }
            else
            {
                eStatus = MB_PDU_EX_NONE;
            }
        }
    }
    return eStatus;
}

eMBException
eMBSFuncDiagnostics( UBYTE *pubMBPDU, USHORT *pusMBPDULen, const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    eMBException eStatus = MB_PDU_EX_ILLEGAL_DATA_VALUE;
    USHORT       usSubFunction;
    /* Length check */
    if( ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE ) <= *pusMBPDULen )
    {
        usSubFunction = ( USHORT )pubMBPDU[MB_PDU_FUNC_DIAG_REQ_SUBFUNCTION_OFF] << 8U;
        usSubFunction |= ( USHORT )pubMBPDU[MB_PDU_FUNC_DIAG_REQ_SUBFUNCTION_OFF + 1];

#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
        if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
        {
            vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                         "Diagnostic call with sub function code " MBP_FORMAT_USHORT " , length = " MBP_FORMAT_USHORT
                         "\n",
                         ( USHORT )usSubFunction, ( USHORT )*pusMBPDULen );
        }
#endif
        switch( usSubFunction )
        {
            case 0x00:
                eStatus = MB_PDU_EX_NONE;
                break;
            case 0x0A:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    ( void )eMBSResetStatistics( pxMBSRegisterCB->pxIntHdl );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x0B:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNPacketsReceived >> 8U );
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNPacketsReceived & 0xFF );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x0C:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNChecksumErrors >> 8U );
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNChecksumErrors & 0xFF );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x0D:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNExceptionCount >> 8U );
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNExceptionCount & 0xFF );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x0E:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNPacketsReceivedSelf >> 8U );
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNPacketsReceivedSelf & 0xFF );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x0F:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] = 0x00;
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] = 0x00;
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x10:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNNACKExceptionCount >> 8U );
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNNACKExceptionCount & 0xFF );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x11:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNBusyExceptionCount >> 8U );
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] =
                        ( UBYTE )( pxMBSRegisterCB->pxIntHdl->xFrameStat.ulNBusyExceptionCount & 0xFF );
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x12:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] = 0x00;
                    pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] = 0x00;
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            case 0x14:
                if( ( *pusMBPDULen == ( MB_PDU_SIZE_MIN + MB_PDU_FUNC_DIAG_MIN_REQ_SIZE + 2 ) ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF] ) &&
                    ( 0x00 == pubMBPDU[MB_PDU_FUNC_DIAG_REQ_DATA_OFF + 1] ) )
                {
                    /* Just echo - We can not count this cause overruns are within the
                     * porting layer.
                     */
                    eStatus = MB_PDU_EX_NONE;
                }
                break;
            default:
                eStatus = MB_PDU_EX_ILLEGAL_FUNCTION;
                break;
        }
    }
    return eStatus;
}

#endif

#if MBS_FUNC_READ_EXCEPTION_STATUS_ENABLED == 1
eMBException
eMBSFuncReadExceptionStatus( UBYTE *pubMBPDU, USHORT *pusMBPDULen,
                             const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    eMBException      eStatus = MB_PDU_EX_SLAVE_DEVICE_FAILURE, eStatus2;
    UBYTE             ubCnt;
    UBYTE             ubResult;
    STATIC const LONG arCoildForException[] = { MBS_EXCEPTION_STATUS_BIT0_COIL, MBS_EXCEPTION_STATUS_BIT1_COIL,
                                                MBS_EXCEPTION_STATUS_BIT2_COIL, MBS_EXCEPTION_STATUS_BIT3_COIL,
                                                MBS_EXCEPTION_STATUS_BIT4_COIL, MBS_EXCEPTION_STATUS_BIT5_COIL,
                                                MBS_EXCEPTION_STATUS_BIT6_COIL, MBS_EXCEPTION_STATUS_BIT7_COIL };

    /* Length check */
    if( ( MB_PDU_SIZE_MIN == *pusMBPDULen ) && ( NULL != pxMBSRegisterCB->peMBSCoilsCB ) )
    {
        pubMBPDU[MB_PDU_DATA_OFF] = 0x00;
        for( ubCnt = 0; ubCnt < 8; ubCnt++ )
        {
            if( ( arCoildForException[ubCnt] >= 0 ) && ( arCoildForException[ubCnt] <= 65535 ) )
            {
                ubResult = 0;
#if MBS_CALLBACK_ENABLE_CONTEXT == 1
                eStatus2 = pxMBSRegisterCB->peMBSCoilsCB( pxMBSRegisterCB->pvCtx, &ubResult,
                                                          ( USHORT )arCoildForException[ubCnt], 1, MBS_REGISTER_READ );
#else
                eStatus2 = pxMBSRegisterCB->peMBSCoilsCB( &ubResult, ( USHORT )arCoildForException[ubCnt], 1,
                                                          MBS_REGISTER_READ );
#endif
                if( MB_PDU_EX_NONE == eStatus2 )
                {
                    if( ubResult )
                    {
                        pubMBPDU[MB_PDU_DATA_OFF] |= ( UBYTE )( 1U << ubCnt );
                    }
                }
                else
                {
#if defined( MBS_ENABLE_DEBUG_FACILITY ) && ( MBS_ENABLE_DEBUG_FACILITY == 1 )
                    if( bMBPPortLogIsEnabled( MB_LOG_DEBUG, MB_LOG_CORE ) )
                    {
                        vMBPPortLog( MB_LOG_DEBUG, MB_LOG_CORE,
                                     "Read exception status failed for coil address " MBP_FORMAT_USHORT "\n",
                                     ( USHORT )arCoildForException[ubCnt] );
                    }
#endif
                    break;
                }
            }
        }
        if( 8 == ubCnt )
        {
            eStatus = MB_PDU_EX_NONE;
            *pusMBPDULen = 2;
        }
    }
    return eStatus;
}
#endif
