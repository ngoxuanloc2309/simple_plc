#ifndef ETHERNET_WIFI_GATEWAY_H
#define ETHERNET_WIFI_GATEWAY_H

/*
 * board_ethernet_wifi_gateway.h - Ethernet/WiFi Gateway SKU
 *
 * This board has NO local I/O pins. Its DI/DO are VIRTUAL: each one stands
 * for a DI/DO of an I/O board on the RS485 bus (see services/plc_io/
 * plc_io_remote.h). Only two peripherals are used: the log UART and the
 * RS485 UART.
 *
 * ---- Adding / growing I/O boards (the ONLY place to edit) ---------------
 *
 * One row per I/O board:  X(unit_id, di_count, do_count)
 *
 *   4DI/4DO board, address 1 :   X(1, 4, 4)
 *   later 8DI/8DO, address 1 :   X(1, 8, 8)              (just change the row)
 *   plus a second board      :   X(1, 8, 8)  X(2, 4, 4)  (add a row)
 *
 * Virtual tags follow the rows in order: node 0's DI bits become DI tags
 * 0.., node 1's continue after them; the same for DO. The tag layout
 * (GATEWAY_FAKE_IO_DI_NUM / _DO_NUM below) is computed from the rows, so
 * there is nothing else to keep in sync. Limits: <= 16 DI and <= 16 DO per
 * row, <= SPLC_REMOTE_MAX_NODES rows, total tags <= MAX_TAGS
 * (board_tag_define.h) -- all checked at compile time.
 */

#include "splc_opt.h"

#define GATEWAY_IO_NODES(X) \
    X(1, 4, 4)

/* Lease the I/O boards get (ms, 0 = none) and their DO state when it expires
 * (bit i = DO i; 0 = all OFF). Same for every board. */
#define GATEWAY_NODE_LEASE_MS     3000
#define GATEWAY_NODE_SAFE_VALUE   0x0000

/* --- derived totals (do not edit) -------------------------------------- */
#define GW_SUM_DI_(unit, di, do_)  + (di)
#define GW_SUM_DO_(unit, di, do_)  + (do_)
#define GW_SUM_N_(unit, di, do_)   + 1

#define GATEWAY_FAKE_IO_DI_NUM    (0 GATEWAY_IO_NODES(GW_SUM_DI_))
#define GATEWAY_FAKE_IO_DO_NUM    (0 GATEWAY_IO_NODES(GW_SUM_DO_))
#define GATEWAY_NODE_COUNT        (0 GATEWAY_IO_NODES(GW_SUM_N_))

/* Same fixed internal tag ranges as every SKU. */
#define GATEWAY_AI_NUM              0
#define GATEWAY_VFLAG_NUM           32
#define GATEWAY_VREG_NUM            32
#define GATEWAY_VREG_RETAIN_NUM     32
#define GATEWAY_COUNTER_NUM         8

/* --- peripherals (CubeMX handles in Core/Inc/usart.h) ------------------- */
#define UART_LOG      hlpuart1
#define UART_RS485    huart1

/* RS485 driver-enable: CubeMX labels PB13 "USART1_DE" (main.h); the RS485
 * master drives it high around each transmit. */
#define GATEWAY_RS485_DE_PORT     USART1_DE_GPIO_Port
#define GATEWAY_RS485_DE_PIN      USART1_DE_Pin

/* MUST equal huart1.Init.BaudRate in Core/Src/usart.c (CubeMX).
 * sx_uart_init() does not change the peripheral's baud rate; this value only
 * feeds the inter-frame gap. The I/O boards must run the same baud, 8N1. */
#define GATEWAY_RS485_BAUDRATE    115200

/* Modbus RTU unit ID the gateway itself would answer to on its App link. */
#define MODBUS_UNIT_ID            SPLC_MODBUS_UNIT_ID

#endif