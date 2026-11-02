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
#define MB_PDU_FUNC_SLAVEID_BYTE_CNT_OFF ( MB_PDU_FUNC_OFF + 1 )
#define MB_PDU_FUNC_SLAVEID_DATA_OFF     ( MB_PDU_FUNC_SLAVEID_BYTE_CNT_OFF + 1 )

/* ----------------------- Type definitions ---------------------------------*/

/* ----------------------- Static variables ---------------------------------*/

/* ----------------------- Static functions ---------------------------------*/

/* ----------------------- Start implementation -----------------------------*/

#if MBS_FUNC_REPORT_SLAVE_ID_ENABLED == 1
eMBException
eMBSFuncReportSlaveID( UBYTE *pubMBPDU, USHORT *pusMBPDULen, const xMBSRegisterCB *pxMBSRegisterCB ) MB_CDECL_SUFFIX
{
    UBYTE *      pubFrameCur;
    eMBException eStatus = MB_PDU_EX_SLAVE_DEVICE_FAILURE, eStatus2;
    USHORT       usBytesLeft;
    USHORT       usBytesWritten;

    /* Length check */
    if( ( MB_PDU_SIZE_MIN == *pusMBPDULen ) && ( NULL != pxMBSRegisterCB->peMBSSerDiagCB ) )
    {
        /* First byte contains the function code. */
        pubMBPDU[MB_PDU_FUNC_OFF] = MBS_FUNC_REPORT_SLAVE_ID;
        pubMBPDU[MB_PDU_FUNC_SLAVEID_BYTE_CNT_OFF] = 0; /* will be updated later */
        *pusMBPDULen = ( USHORT )2;

        pubFrameCur = &pubMBPDU[MB_PDU_FUNC_SLAVEID_DATA_OFF];
        usBytesLeft = ( USHORT )MB_PDU_SIZE_MAX - *pusMBPDULen;
#if MBS_CALLBACK_ENABLE_CONTEXT == 1
        eStatus2 = pxMBSRegisterCB->peMBSSerDiagCB( pxMBSRegisterCB->pvCtx, REPORT_SLAVE_ID_REQUEST_SLAVE_ID,
                                                    pubFrameCur, &usBytesWritten, usBytesLeft );
#else
        eStatus2 = pxMBSRegisterCB->peMBSSerDiagCB( REPORT_SLAVE_ID_REQUEST_SLAVE_ID, pubFrameCur, &usBytesWritten,
                                                    usBytesLeft );
#endif
        if( ( MB_PDU_EX_NONE == eStatus2 ) && ( usBytesLeft > 0 ) )
        {
            MBP_ASSERT( usBytesWritten <= usBytesLeft );
            /* Account for last data written */
            pubFrameCur += usBytesWritten;
            *pusMBPDULen += usBytesWritten;
            usBytesLeft -= usBytesWritten;
#if MBS_CALLBACK_ENABLE_CONTEXT == 1
            eStatus2 = pxMBSRegisterCB->peMBSSerDiagCB( pxMBSRegisterCB->pvCtx, REPORT_SLAVE_ID_REQUEST_STATUS,
                                                        pubFrameCur, &usBytesWritten, 1 );
#else
            eStatus2 =
                pxMBSRegisterCB->peMBSSerDiagCB( REPORT_SLAVE_ID_REQUEST_STATUS, pubFrameCur, &usBytesWritten, 1 );
#endif

            if( MB_PDU_EX_NONE == eStatus2 )
            {
                MBP_ASSERT( usBytesWritten <= usBytesLeft );
                pubFrameCur += usBytesWritten;
                *pusMBPDULen += usBytesWritten;
                usBytesLeft -= usBytesWritten;
#if MBS_CALLBACK_ENABLE_CONTEXT == 1
                eStatus2 =
                    pxMBSRegisterCB->peMBSSerDiagCB( pxMBSRegisterCB->pvCtx, REPORT_SLAVE_ID_REQUEST_ADDITIONAL_DATA,
                                                     pubFrameCur, &usBytesWritten, usBytesLeft );
#else
                eStatus2 = pxMBSRegisterCB->peMBSSerDiagCB( REPORT_SLAVE_ID_REQUEST_ADDITIONAL_DATA, pubFrameCur,
                                                            &usBytesWritten, usBytesLeft );
#endif
                if( MB_PDU_EX_NONE == eStatus2 )
                {
                    MBP_ASSERT( usBytesWritten <= usBytesLeft );
                    *pusMBPDULen += usBytesWritten;
                    pubMBPDU[MB_PDU_FUNC_SLAVEID_BYTE_CNT_OFF] = ( UBYTE )( *pusMBPDULen - 2 );
                    eStatus = MB_PDU_EX_NONE;
                }
            }
        }
    }
    return eStatus;
}

#endif
