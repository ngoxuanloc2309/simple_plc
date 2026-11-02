#ifndef PLC_MB_STATUS_H
#define PLC_MB_STATUS_H

/*
 * plc_mb_status.h - Layer 3. Library-neutral result of one Modbus register
 * handler (plc_modbus_cfg.c).
 *
 * The register-map logic is shared by every Modbus library that can serve the
 * App link (SPLC_MODBUS_LIB, splc_opt.h). It must not depend on one library's
 * error enum, so it returns these codes. 0 = OK; 1..4 are the standard Modbus
 * exception codes put on the wire. Each library adapter (bottom of
 * plc_modbus_cfg.c) maps them to its own type; for nanoMODBUS the values are
 * identical (NMBS_EXCEPTION_* = 1..4, NMBS_ERROR_NONE = 0), so that map is a
 * plain cast.
 */

typedef int plc_mb_status_t;

#define PLC_MB_OK                          0
#define PLC_MB_EXC_ILLEGAL_FUNCTION        1
#define PLC_MB_EXC_ILLEGAL_DATA_ADDRESS    2
#define PLC_MB_EXC_ILLEGAL_DATA_VALUE      3
#define PLC_MB_EXC_SERVER_DEVICE_FAILURE   4

#endif /* PLC_MB_STATUS_H */