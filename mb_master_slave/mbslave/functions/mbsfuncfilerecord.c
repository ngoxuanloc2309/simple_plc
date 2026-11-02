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
#include "internal/mbsiframe.h"
#include "internal/mbsi.h"
#include "mbsfunctions.h"

/* ----------------------- Defines ------------------------------------------*/
#define MB_PDU_FUNC_READ_MIN_SIZE                 ( 8 )
#define MB_PDU_FUNC_WRITE_MIN_SIZE                ( 9 )
#define MB_PDU_FUNC_READWRITE_FILE_REF_TYPE       ( 6 )
#define MB_PDU_FUNC_READWRITE_FILE_BYTE_OFF       ( MB_PDU_DATA_OFF )
#define MB_PDU_FUNC_READWRITE_FILE_SUBREQ_OFF     ( MB_PDU_DATA_OFF + 1 )
#define MB_PDU_FUNC_READ_BYTE_MIN                 ( 7 )
#define MB_PDU_FUNC_READ_BYTE_MAX                 ( 0xF5 )
#define MB_PDU_FUNC_WRITE_BYTE_MIN                ( 9 )
#define MB_PDU_FUNC_WRITE_BYTE_MAX                ( 0xFB )
#define MB_PDU_FUNC_READ_FILE_SUBREQ_REF_TYPE_OFF ( 0 )
#define MB_PDU_FUNC_READ_FILE_SUBREQ_FILENO_OFF   ( 1 )
#define MB_PDU_FUNC_READ_FILE_SUBREQ_RECORDNR_OFF ( 3 )
#define MB_PDU_FUNC_READ_FILE_SUBREQ_RECORDLN_OFF ( 5 )

#define MB_PDU_FUNC_READ_FILE_FILENO_MIN   ( 1 )
#define MB_PDU_FUNC_READ_FILE_RECORDNR_MAX ( 0x270F )

// maximum number of sub-records in a frame is 35, however let user override this
#ifndef MBS_FILE_RECORD_SUBREQ_MAX
#define MB_PDU_FUNC_READWRITE_FILE_RECORD_SUBREQ_MAX ( 35 )
#else
#define MB_PDU_FUNC_READWRITE_FILE_RECORD_SUBREQ_MAX MBS_FILE_RECORD_SUBREQ_MAX
#endif

#if MBS_CALLBACK_ENABLE_CONTEXT == 1
#define CALLBACK_ARG pxMBSRegisterCB->pvCtx,
#else
#define CALLBACK_ARG
#endif

/* ----------------------- Type definitions ---------------------------------*/

/* ----------------------- Static variables ---------------------------------*/
#if MBS_FUNC_READ_FILE_RECORD_ENABLED == 1 || MBS_FUNC_WRITE_FILE_RECORD_ENABLED == 1
static struct
{
    USHORT usFileNo;
    USHORT usRecordNo;
    USHORT usRecordLen;
} xSubreqBuffer[MB_PDU_FUNC_READWRITE_FILE_RECORD_SUBREQ_MAX];
#endif
/* ----------------------- Static functions ---------------------------------*/

/* ----------------------- Start implementation -----------------------------*/
#if MBS_FUNC_READ_FILE_RECORD_ENABLED == 1
eMBException
eMBSFuncReadFileRecord( UBYTE *pubMBPDU, USHORT *pusMBPDULen, const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    UBYTE        ubByteCount;
    UBYTE *      pubFrameCur;
    UBYTE        ubReqIsGood = 1;
    USHORT       usResLen = 0xffff;
    eMBException eStatus = MB_PDU_EX_ILLEGAL_DATA_VALUE;

    /* Length check: first at least one full frame?, second: bytecount + overheads sums up to PDU length? */
    if( ( ( MB_PDU_FUNC_READ_MIN_SIZE + MB_PDU_SIZE_MIN ) < *pusMBPDULen ) ||
        ( MB_PDU_SIZE_MIN + 2 + pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_BYTE_OFF] != *pusMBPDULen ) )
    {
        ubByteCount = ( UBYTE )( pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_BYTE_OFF] );
        UBYTE ubNrSubReq = ubByteCount / 7;

        usResLen = 0;

        /* Check if the number of registers to read is valid. If not
         * return Modbus illegal data value exception.
         */
        if( ( ubByteCount >= MB_PDU_FUNC_READ_BYTE_MIN ) && ( ubByteCount <= MB_PDU_FUNC_READ_BYTE_MAX ) )
        {
            /* Check for complete sub-requests */
            if( ubByteCount % 7 == 0 )
            {
                /* point to first sub-request */
                pubFrameCur = &pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_SUBREQ_OFF];

                for( UBYTE i = 0; i < ubNrSubReq; i++ )
                {
                    USHORT usFileNo;
                    USHORT usRecordNo;
                    USHORT usRecordLen;

                    UBYTE ubRefType = *pubFrameCur++;
                    if( ubRefType != MB_PDU_FUNC_READWRITE_FILE_REF_TYPE )
                    {
                        eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                        ubReqIsGood = 0;
                        break;
                    }

                    usFileNo = ( USHORT )( ( USHORT )*pubFrameCur++ << 8 );
                    usFileNo |= ( USHORT )( *pubFrameCur++ );

                    usRecordNo = ( USHORT )( ( USHORT )*pubFrameCur++ << 8 );
                    usRecordNo |= ( USHORT )( *pubFrameCur++ );

                    usRecordLen = ( USHORT )( ( USHORT )*pubFrameCur++ << 8 );
                    usRecordLen |= ( USHORT )( *pubFrameCur++ );

                    usResLen += 1 + 1 + usRecordLen * 2; /* byte-length subreq, reference, data bytes */

                    /* check each sub-request for sanity */
                    if( ubRefType != MB_PDU_FUNC_READWRITE_FILE_REF_TYPE ||
                        usRecordNo > MB_PDU_FUNC_READ_FILE_RECORDNR_MAX ||
                        usRecordLen + usRecordNo > MB_PDU_FUNC_READ_FILE_RECORDNR_MAX + 1 )
                    {
                        eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                        ubReqIsGood = 0;
                        break;
                    }

                    /* can the answer frame fit into PDU? */
                    if( usResLen > MB_PDU_FUNC_READ_BYTE_MAX )
                    {
                        eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                        ubReqIsGood = 0;
                        break;
                    }

                    /* store request, as we overwrite it when generating the answer */
                    /* the returned error code is not according to the specification */
                    if( i < MB_PDU_FUNC_READWRITE_FILE_RECORD_SUBREQ_MAX )
                    {
                        xSubreqBuffer[i].usFileNo = usFileNo;
                        xSubreqBuffer[i].usRecordNo = usRecordNo;
                        xSubreqBuffer[i].usRecordLen = usRecordLen;
                    }
                    else
                    {
                        eStatus = MB_PDU_EX_ILLEGAL_DATA_VALUE;
                        ubReqIsGood = 0;
                        break;
                    }
                }
            }
            else
            {
                ubReqIsGood = 0;
                eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
            }

            /* sub-requests are good, actually process frame */
            if( ubReqIsGood )
            {
                /* point to start of PDU */
                pubFrameCur = &pubMBPDU[MB_PDU_FUNC_OFF];
                *pusMBPDULen = MB_PDU_FUNC_OFF;

                /* First byte contains the function code. */
                *pubFrameCur++ = MBS_FUNCCODE_READ_FILE_RECORD;
                *pusMBPDULen += ( USHORT )1;

                /* Second byte in the response contain the number of bytes. */
                *pubFrameCur++ = ( UBYTE )usResLen;
                *pusMBPDULen += ( USHORT )1;

                for( UBYTE i = 0; i < ubNrSubReq; i++ )
                {
                    UBYTE ubSubreqLen = xSubreqBuffer[i].usRecordLen * 2;

                    /* First byte contains the number of bytes for this subreq */
                    *pubFrameCur++ = ( UBYTE )( 1 + xSubreqBuffer[i].usRecordLen * 2 );
                    *pusMBPDULen += ( USHORT )1;

                    /* Second byte in the response contains the reference type */
                    *pubFrameCur++ = ( UBYTE )MB_PDU_FUNC_READWRITE_FILE_REF_TYPE;
                    *pusMBPDULen += ( USHORT )1;

                    /* Get the acutal recard values from the callback. */
                    if( NULL != pxMBSRegisterCB->peMBSFileRecordCB )
                    {
                        eStatus = pxMBSRegisterCB->peMBSFileRecordCB(
                            CALLBACK_ARG pubFrameCur, xSubreqBuffer[i].usFileNo, xSubreqBuffer[i].usRecordNo,
                            xSubreqBuffer[i].usRecordLen, MBS_REGISTER_READ );

                        if( eStatus != MB_PDU_EX_NONE )
                        {
                            eStatus = MB_PDU_EX_SLAVE_DEVICE_FAILURE;
                            break;
                        }

                        pubFrameCur += ubSubreqLen;
                        *pusMBPDULen += ubSubreqLen;
                    }
                    else
                    {
                        eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                        break;
                    }
                } /* for */
            }     /* ubReqIsGood */
        }
        else
        {
            eStatus = MB_PDU_EX_ILLEGAL_DATA_VALUE;
        }
    }
    return eStatus;
}
#endif

#if MBS_FUNC_WRITE_FILE_RECORD_ENABLED == 1
eMBException
eMBSFuncWriteFileRecord( UBYTE *pubMBPDU, USHORT *pusMBPDULen, const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    eMBException eStatus = MB_PDU_EX_ILLEGAL_DATA_VALUE;

    /* Length check: first at least one full frame?, second: bytecount + overheads sums up to PDU length? */
    if( ( ( MB_PDU_FUNC_WRITE_MIN_SIZE + MB_PDU_SIZE_MIN ) < *pusMBPDULen ) ||
        ( MB_PDU_SIZE_MIN + 1 + pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_BYTE_OFF] != *pusMBPDULen ) )
    {
        UBYTE ubByteCount = ( UBYTE )( pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_BYTE_OFF] );

        /* check for size violation */
        if( ( ubByteCount >= MB_PDU_FUNC_WRITE_BYTE_MIN ) && ( ubByteCount <= MB_PDU_FUNC_WRITE_BYTE_MAX ) )
        {
            /* Handle sub-requests */
            /* point to first sub-request */
            UBYTE *pubFrameStart = &pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_SUBREQ_OFF];
            UBYTE *pubFrameCur = pubFrameStart;
            UBYTE *pubFrameEnd = &pubMBPDU[MB_PDU_FUNC_READWRITE_FILE_BYTE_OFF] + 1 + ubByteCount;

            UBYTE ubChecking = TRUE;
            /* this loop is itarted twice, once for checking ALL subReq w/o invoking callbacks */
            /* second time invoking callbacks as well, checks are repeated for simplicity */
            while( pubFrameCur < pubFrameEnd )
            {
                USHORT usFileNo;
                USHORT usRecordNo;
                USHORT usRecordLen;

                if( pubFrameCur + 7 > pubFrameEnd )
                {
                    eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                    break;
                }

                UBYTE ubRefType = *pubFrameCur++;

                if( ubRefType != MB_PDU_FUNC_READWRITE_FILE_REF_TYPE )
                {
                    eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                    break;
                }

                usFileNo = ( USHORT )( ( USHORT )*pubFrameCur++ << 8 );
                usFileNo |= ( USHORT )( *pubFrameCur++ );

                usRecordNo = ( USHORT )( ( USHORT )*pubFrameCur++ << 8 );
                usRecordNo |= ( USHORT )( *pubFrameCur++ );

                usRecordLen = ( USHORT )( ( USHORT )*pubFrameCur++ << 8 );
                usRecordLen |= ( USHORT )( *pubFrameCur++ );

                /* check that data access stays within frame */
                if( pubFrameCur + usRecordLen * 2 > pubFrameEnd )
                {
                    eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
                    break;
                }

                /* Get the acutal recard values from the callback, if not in checking iteration */
                if( ubChecking != TRUE && NULL != pxMBSRegisterCB->peMBSFileRecordCB )
                {
                    eStatus = pxMBSRegisterCB->peMBSFileRecordCB( CALLBACK_ARG pubFrameCur, usFileNo, usRecordNo,
                                                                  usRecordLen, MBS_REGISTER_WRITE );

                    if( eStatus != MB_PDU_EX_NONE )
                    {
                        eStatus = MB_PDU_EX_SLAVE_DEVICE_FAILURE;
                        break;
                    }
                }

                /* move to end this subReq */
                pubFrameCur += usRecordLen * 2;

                /* check for last subReq */
                if( pubFrameCur == pubFrameEnd )
                {
                    if( ubChecking == TRUE )
                    {
                        /* frame fully consumed by SubReq, thus good, now iterate again and actually invoke callbacks */
                        ubChecking = FALSE;
                        pubFrameCur = pubFrameStart;
                    }
                }
            }

            /* if subReq checking fails we do have an illegal data address error */
            if( ubChecking == TRUE )
            {
                eStatus = MB_PDU_EX_ILLEGAL_DATA_ADDRESS;
            }
        }
        else
        {
            eStatus = MB_PDU_EX_ILLEGAL_DATA_VALUE;
        }
    }
    return eStatus;
}
#endif
