/*
 * mbport.h - platform header required by libs/mb_master_slave (product-supplied,
 * the library ships none). Part of the SimplePLC MB adapter (port/mb_port/).
 *
 * Defines the basic types and the porting macros the library expects. Nothing
 * here knows any hardware: the byte link behind the library is the
 * modbus_transport_t the product already supplies (see mb_port.h).
 *
 * Threading: the library is driven ONLY from the engine context
 * (modbus_config_service()), never from an ISR, so the critical-section macros
 * are empty. Do not call into the library from another context.
 */
#ifndef SPLC_MBPORT_H
#define SPLC_MBPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef uint8_t   BOOL;
typedef char      CHAR;
typedef uint8_t   UCHAR;
typedef uint8_t   UBYTE;
typedef int16_t   SHORT;
typedef uint16_t  USHORT;
typedef int32_t   LONG;
typedef uint32_t  ULONG;

/* Library convention: file-local functions/data are declared STATIC. */
#define STATIC static
#define INLINE inline

/* Handles are opaque to the library (see mb_port.c for what they point to). */
typedef void *xMBPTimerHandle;
typedef void *xMBPEventHandle;
typedef void *xMBPSerialHandle;
typedef void *xMBPTCPHandle;
typedef void *xMBPTCPClientHandle;

#define MBP_TIMERHDL_INVALID       ( NULL )
#define MBP_EVENTHDL_INVALID       ( NULL )
#define MBP_SERIALHDL_INVALID      ( NULL )
#define MBP_TCPHDL_INVALID         ( NULL )
#define MBP_TCPHDL_CLIENT_INVALID  ( NULL )
#define MBP_HDL_INVALID            ( NULL )

/* Serial API V1 (byte per callback). V2 cannot keep the T3.5 timing strict. */
#define MBP_FORCE_SERV1PROTOTYPES        ( 1 )
#define MBP_FORCE_SERV2PROTOTYPES        ( 0 )

/* The port does not detect frame ends itself: the library's own T3.5 timer
 * (eMBPTimer*) does, fed by mb_port_pump() in the order rx -> timers -> poll. */
#define MBP_SERIAL_PORT_DETECTS_TIMEOUT  ( 0 )
#define MBP_SUPPORT_FRAME_ANALYZE_STAT_API ( 0 )
#define MBP_ENABLE_DEBUG_FACILITY        ( 0 )
#define MBP_HAS_TIMESTAMP                ( 0 )
#define MBP_ADVA_STARTUP_SHUTDOWN_ENABLED ( 0 )
#define MBP_HITECH_LINKERWORKAROUND_ENABLED ( 0 )
#define MBP_CORRECT_FUNCPOINTER_CAST     ( 0 )

#define MBP_ENTER_CRITICAL_INIT()    ( ( void )0 )
#define MBP_EXIT_CRITICAL_INIT()     ( ( void )0 )
#define MBP_ENTER_CRITICAL_SECTION() ( ( void )0 )
#define MBP_EXIT_CRITICAL_SECTION()  ( ( void )0 )
/* Evaluates the expression (keeps "set but not used" quiet), no trap. */
#define MBP_ASSERT( expr )           ( ( void )( expr ) )

#endif /* SPLC_MBPORT_H */