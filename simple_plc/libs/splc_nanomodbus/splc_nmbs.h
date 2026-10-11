#ifndef SPLC_NMBS_H
#define SPLC_NMBS_H

/*
 * splc_nmbs.h - SimplePLC's private copy of nanoMODBUS.
 *
 * Every SimplePLC source file includes THIS header, never "nanomodbus.h".
 *
 * Why a private, renamed copy. nanoMODBUS is pure logic (bytes in, bytes out,
 * no peripheral, no global state -- verified with `nm`: no global data
 * symbol), so two copies can coexist safely. A firmware that embeds SimplePLC
 * usually has its own nanoMODBUS already (same function names, possibly a
 * different version or compile-time config). Sharing that copy would tie
 * SimplePLC's reliability to a version and flags it does not control; linking
 * both under the same names gives "multiple definition". Instead SimplePLC
 * always builds its own pinned copy and renames every global symbol with a
 * "splc_" prefix, so the two never meet. Cost: ~7.5 KB of Flash.
 * (TinyUSB is NOT handled this way: it owns a peripheral and an ISR, so it
 * has exactly one owner. See docs/architecture.md section 3.1.)
 *
 * What this file does:
 *   1. #undef every nanoMODBUS compile-time option. A host that builds its
 *      own nanoMODBUS with e.g. -DNMBS_SERVER_DISABLED passes that flag to
 *      the whole directory; without this, the flag would also strip the
 *      server out of SimplePLC's copy. SimplePLC's copy is pinned to
 *      nanoMODBUS's defaults (server + client, NMBS_BITFIELD_MAX 2000).
 *   2. Rename every global function of nanomodbus.c. NOTE this includes the
 *      recv_*_res functions, which do not start with "nmbs_".
 *   3. Include the vendored header by RELATIVE path, so a host's own
 *      "nanomodbus.h" can never be picked up through include-path order.
 *
 * Types (nmbs_t, nmbs_error, ...) and NMBS_* constants keep their names: they
 * have no link-time symbol, and a TU never sees both copies' headers.
 *
 * The vendored libs/nanomodbus/ is NOT modified. If a new nanoMODBUS version
 * adds a global function, the symbol check in libs/CMakeLists.txt
 * (cmake_modules/splc_check_symbols.cmake) fails the build: add its #define
 * below.
 */

#undef NMBS_BITFIELD_MAX
#undef NMBS_CLIENT_DISABLED
#undef NMBS_SERVER_DISABLED
#undef NMBS_DEBUG
#undef NMBS_STRERROR_DISABLED
#undef NMBS_SERVER_READ_COILS_DISABLED
#undef NMBS_SERVER_READ_DISCRETE_INPUTS_DISABLED
#undef NMBS_SERVER_READ_HOLDING_REGISTERS_DISABLED
#undef NMBS_SERVER_READ_INPUT_REGISTERS_DISABLED
#undef NMBS_SERVER_WRITE_SINGLE_COIL_DISABLED
#undef NMBS_SERVER_WRITE_SINGLE_REGISTER_DISABLED
#undef NMBS_SERVER_WRITE_MULTIPLE_COILS_DISABLED
#undef NMBS_SERVER_WRITE_MULTIPLE_REGISTERS_DISABLED
#undef NMBS_SERVER_READ_FILE_RECORD_DISABLED
#undef NMBS_SERVER_WRITE_FILE_RECORD_DISABLED
#undef NMBS_SERVER_READ_WRITE_REGISTERS_DISABLED
#undef NMBS_SERVER_READ_DEVICE_IDENTIFICATION_DISABLED

/* Every global function defined by nanomodbus.c (generated from `nm`). */
#define nmbs_callbacks_create                    splc_nmbs_callbacks_create
#define nmbs_client_create                       splc_nmbs_client_create
#define nmbs_crc_calc                            splc_nmbs_crc_calc
#define nmbs_create                              splc_nmbs_create
#define nmbs_platform_conf_create                splc_nmbs_platform_conf_create
#define nmbs_read_coils                          splc_nmbs_read_coils
#define nmbs_read_device_identification          splc_nmbs_read_device_identification
#define nmbs_read_device_identification_basic    splc_nmbs_read_device_identification_basic
#define nmbs_read_device_identification_extended splc_nmbs_read_device_identification_extended
#define nmbs_read_device_identification_regular  splc_nmbs_read_device_identification_regular
#define nmbs_read_discrete_inputs                splc_nmbs_read_discrete_inputs
#define nmbs_read_file_record                    splc_nmbs_read_file_record
#define nmbs_read_holding_registers              splc_nmbs_read_holding_registers
#define nmbs_read_input_registers                splc_nmbs_read_input_registers
#define nmbs_read_write_registers                splc_nmbs_read_write_registers
#define nmbs_receive_raw_pdu_response            splc_nmbs_receive_raw_pdu_response
#define nmbs_send_raw_pdu                        splc_nmbs_send_raw_pdu
#define nmbs_server_create                       splc_nmbs_server_create
#define nmbs_server_poll                         splc_nmbs_server_poll
#define nmbs_set_byte_timeout                    splc_nmbs_set_byte_timeout
#define nmbs_set_callbacks_arg                   splc_nmbs_set_callbacks_arg
#define nmbs_set_destination_rtu_address         splc_nmbs_set_destination_rtu_address
#define nmbs_set_platform_arg                    splc_nmbs_set_platform_arg
#define nmbs_set_read_timeout                    splc_nmbs_set_read_timeout
#define nmbs_strerror                            splc_nmbs_strerror
#define nmbs_write_file_record                   splc_nmbs_write_file_record
#define nmbs_write_multiple_coils                splc_nmbs_write_multiple_coils
#define nmbs_write_multiple_registers            splc_nmbs_write_multiple_registers
#define nmbs_write_single_coil                   splc_nmbs_write_single_coil
#define nmbs_write_single_register               splc_nmbs_write_single_register
#define recv_read_device_identification_res      splc_recv_read_device_identification_res
#define recv_read_file_record_res                splc_recv_read_file_record_res
#define recv_write_file_record_res               splc_recv_write_file_record_res
#define recv_write_multiple_coils_res            splc_recv_write_multiple_coils_res
#define recv_write_multiple_registers_res        splc_recv_write_multiple_registers_res
#define recv_write_single_coil_res               splc_recv_write_single_coil_res
#define recv_write_single_register_res           splc_recv_write_single_register_res

#include "../nanomodbus/nanomodbus.h"

#endif /* SPLC_NMBS_H */