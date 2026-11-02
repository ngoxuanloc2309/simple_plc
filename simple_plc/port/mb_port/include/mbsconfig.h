/*
 * mbsconfig.h - slave configuration for libs/mb_master_slave (product-supplied;
 * included by mbs.h before the library's own defaults). SimplePLC builds ONLY
 * what its register map needs: holding registers (FC03/FC06/FC16), serial RTU
 * and ASCII. TCP/UDP are not built yet (see mb_port.h).
 */
#ifndef SPLC_MBSCONFIG_H
#define SPLC_MBSCONFIG_H

#include "mbport.h"
#include "splc_opt.h"   /* SPLC_MODBUS_MAX_TRANSPORTS, SPLC_MODBUS_RTU_GAP_MS */

#define MBS_RTU_ENABLED                 ( 1 )
#define MBS_ASCII_ENABLED               ( 1 )
#define MBS_TCP_ENABLED                 ( 0 )
#define MBS_UDP_ENABLED                 ( 0 )

/* One server instance per transport slot (RTU and ASCII share the slot index). */
#define MBS_SERIAL_RTU_MAX_INSTANCES    ( SPLC_MODBUS_MAX_TRANSPORTS )
#define MBS_SERIAL_ASCII_MAX_INSTANCES  ( SPLC_MODBUS_MAX_TRANSPORTS )

#define MBS_SERIAL_API_VERSION          ( 1 )
#define MBS_CALLBACK_ENABLE_CONTEXT     ( 0 )
#define MBS_ENABLE_FULL_API_CHECKS      ( 1 )
#define MBS_ENABLE_STATISTICS_INTERFACE ( 0 )
#define MBS_ENABLE_PROT_ANALYZER_INTERFACE ( 0 )
#define MBS_ENABLE_SER_DIAG             ( 0 )
#define MBS_ENABLE_GATEWAY_MODE         ( 0 )
#define MBS_ENABLE_DEBUG_FACILITY       ( 0 )
#define MBS_RTU_WAITAFTERSEND_ENABLED   ( 0 )
#define MBS_ASCII_WAITAFTERSEND_ENABLED ( 0 )

/* One eMBSPoll() call runs the state machine until it is idle again (handoff
 * 86: otherwise a reply is up to 32 ms late at a 10 ms poll). */
#define MBS_POLL_SINGLE_CYCLE           ( 1 )

#define MBS_FUNC_READ_HOLDING_REGISTERS_ENABLED       ( 1 )
#define MBS_FUNC_WRITE_SINGLE_REGISTER_ENABLED        ( 1 )
#define MBS_FUNC_WRITE_MULTIPLE_REGISTERS_ENABLED     ( 1 )
#define MBS_FUNC_READ_INPUT_REGISTERS_ENABLED         ( 0 )
#define MBS_FUNC_READ_FILE_RECORD_ENABLED             ( 0 )
#define MBS_FUNC_WRITE_FILE_RECORD_ENABLED            ( 0 )
#define MBS_FUNC_MASK_WRITE_REGISTER_ENABLED          ( 0 )
#define MBS_FUNC_READWRITE_MULTIPLE_REGISTERS_ENABLED ( 0 )
#define MBS_FUNC_READ_DISCRETE_ENABLED                ( 0 )
#define MBS_FUNC_READ_COILS_ENABLED                   ( 0 )
#define MBS_FUNC_WRITE_SINGLE_COIL_ENABLED            ( 0 )
#define MBS_FUNC_WRITE_MULTIPLE_COILS_ENABLED         ( 0 )
#define MBS_FUNC_REPORT_SLAVE_ID_ENABLED              ( 0 )
#define MBS_FUNC_READ_EXCEPTION_STATUS_ENABLED        ( 0 )

#endif /* SPLC_MBSCONFIG_H */