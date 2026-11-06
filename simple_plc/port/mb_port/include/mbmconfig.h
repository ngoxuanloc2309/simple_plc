/*
 * mbmconfig.h - master configuration for libs/mb_master_slave (product-supplied;
 * included by mbm.h before the library's own defaults). SimplePLC builds ONLY what
 * its Modbus master blocks (MB_READ / MB_WRITE / Remote I/O) need: serial RTU on
 * RS-485, FC01..FC06, FC15, FC16, plus Modbus TCP when SPLC_MODBUS_TCP_MASTER_ENABLE=1
 * (port/modbus_tcp_master/). ASCII, UDP, file records, raw PDU, slave id and the
 * statistics / analyzer interfaces are not built.
 */
#ifndef SPLC_MBMCONFIG_H
#define SPLC_MBMCONFIG_H

#include "mbport.h"
#include "splc_opt.h"   /* SPLC_MBM_ENABLE, SPLC_MBM_PORT_COUNT */

#define MBM_RTU_ENABLED                 ( 1 )
#define MBM_ASCII_ENABLED               ( SPLC_MBM_ASCII_PORTS > 0 )
#define MBM_TCP_ENABLED                 ( SPLC_MODBUS_TCP_MASTER_ENABLE )
#define MBM_UDP_ENABLED                 ( 0 )

/* One master instance per RS-485 channel of mb_port.c (Modbus master ports plus the
 * Gateway's Remote I/O master). */
#if SPLC_MBM_ENABLE && SPLC_ZIGBEE_ENABLE
/* + one for the Zigbee byte pipe as a Master (MB_READ / MB_WRITE port value 4) */
#define MBM_SERIAL_RTU_MAX_INSTANCES    ( SPLC_MBM_PORT_COUNT + 2 )
#elif SPLC_MBM_ENABLE
#define MBM_SERIAL_RTU_MAX_INSTANCES    ( SPLC_MBM_PORT_COUNT + 1 )
#else
#define MBM_SERIAL_RTU_MAX_INSTANCES    ( 1 )
#endif
/* ASCII master instances: one per RS-485 port built for ASCII (the Remote I/O master is RTU).
 * Instances are taken from a free-slot pool, so the count (not the channel) is what matters. */
#if SPLC_MBM_ASCII_PORTS > 0
#define MBM_SERIAL_ASCII_MAX_INSTANCES  ( SPLC_MBM_ASCII_PORTS )
#else
#define MBM_SERIAL_ASCII_MAX_INSTANCES  ( 1 )
#endif
#define MBM_TCP_MAX_INSTANCES           ( SPLC_MODBUS_TCP_MASTER_COUNT )
#define MBM_UDP_MAX_INSTANCES           ( 1 )

#define MBM_SERIAL_API_VERSION          ( 1 )
#define MBM_ENABLE_FULL_API_CHECKS      ( 1 )
#define MBM_ENABLE_STATISTICS_INTERFACE ( 0 )
#define MBM_ENABLE_PROT_ANALYZER_INTERFACE ( 0 )
#define MBM_ENABLE_DEBUG_FACILITY       ( 0 )
#define MBM_RTU_WAITAFTERSEND_ENABLED   ( 0 )
#define MBM_ASCII_WAITAFTERSEND_ENABLED ( 0 )

/* The answer timeout runs on the library's soft timer from the start of the request;
 * the facade (modbus_serial_mb.c) adds the transmit time of the frame to it. */
#define MBM_TIMEOUT_MODE_AFTER_TRANSMIT ( 0 )

#define MBM_FUNC_RD_HOLDING_ENABLED         ( 1 )
#define MBM_FUNC_RD_INPUT_REGS_ENABLED      ( 1 )
#define MBM_FUNC_WR_SINGLE_REG_ENABLED      ( 1 )
#define MBM_FUNC_WR_MUL_REGS_ENABLED        ( 1 )
#define MBM_FUNC_READ_COILS_ENABLED         ( 1 )
#define MBM_FUNC_READ_DISC_INPUTS_ENABLED   ( 1 )
#define MBM_FUNC_WRITE_SINGLE_COIL_ENABLED  ( 1 )
#define MBM_FUNC_WRITE_COILS_ENABLED        ( 1 )
#define MBM_FUNC_MASK_WR_REG_ENABLED        ( 0 )
#define MBM_FUNC_RDWR_MUL_REGS_ENABLED      ( 0 )
#define MBM_FUNC_READWRITE_RAWPDU_ENABLED   ( 0 )
#define MBM_FUNC_REPORT_SLAVEID_ENABLED     ( 0 )
#define MBM_FUNC_RD_FILES_ENABLED           ( 0 )
#define MBM_FUNC_WR_FILES_ENABLED           ( 0 )

#endif /* SPLC_MBMCONFIG_H */