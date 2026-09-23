#include "plc_system_cmd_service.h"

#include <stdbool.h>
#include <stdint.h>

#include "plc_modbus_cfg.h"
#include "plc_system_cmd.h"
#include "plc_error.h"
#include "sx_system.h"
#include "sx_time.h"

/*
 * Grace period between accepting SPLC_SYSTEM_CMD_REBOOT and actually
 * calling sx_system_reset(). Needed because plc_modbus_cfg.c's
 * write_system_command() only QUEUES the FC06 response byte -- the USB
 * CDC transport (sx_usb_tiny_write(), tud_cdc_write_flush()) does not
 * synchronously guarantee the App has received it by the time this
 * function is first called with the pending command. Reboot
 * unconditionally right away and, on real hardware, the App's Modbus
 * client is left waiting on a response that a reset MCU can now never
 * send -- indistinguishable, from the App's side, from the exact
 * "operation has timed out" symptom already diagnosed once in this
 * project for a different reason (see docs/handoff.md). A few scan
 * cycles' delay costs nothing (REBOOT is not latency-sensitive) and is
 * comfortably longer than one USB Full-Speed frame's transmission time.
 */
#define PLC_REBOOT_DELAY_MS  300U

/* SPLC_SYSTEM_CMD_NONE (0) is never a valid "reboot pending" tick value
 * on its own, but 0 IS a legitimate sx_get_tick_ms() reading right after
 * boot -- so this uses a separate bool, not "tick == 0", to mean
 * "no reboot pending". */
static bool     s_reboot_pending    = false;
static uint32_t s_reboot_due_tick   = 0;

void plc_system_cmd_service(void)
{
    /* A reboot is already pending: don't look for a NEW command this
     * cycle (there is nothing more to accept while one is in flight),
     * just check whether the grace period has elapsed yet. */
    if (s_reboot_pending) {
        uint32_t now = sx_get_tick_ms();
        /* Unsigned-subtraction wraparound-safe "has now reached
         * s_reboot_due_tick yet" check (same pattern as plc_engine.c's
         * own scan-cycle pacing) -- correct across the ~49.7-day
         * sx_get_tick_ms() wraparound, unlike a plain `now >= due` would
         * be right at the wrap boundary. */
        if ((uint32_t)(now - s_reboot_due_tick) < 0x80000000U) {
            sx_system_reset(); /* Does not return. */
        }
        return;
    }

    SPLC_SystemCommand cmd = plc_modbus_cfg_get_pending_system_command();

    switch (cmd) {
        case SPLC_SYSTEM_CMD_NONE:
            /* Nothing to do -- the overwhelmingly common case. */
            break;

        case SPLC_SYSTEM_CMD_REBOOT:
            s_reboot_pending  = true;
            s_reboot_due_tick = sx_get_tick_ms() + PLC_REBOOT_DELAY_MS;
            /* SYSTEM_COMMAND_RESULT.status already reads ACCEPTED (set by
             * write_system_command() the moment the command was decoded);
             * intentionally not flipped to DONE here, or anywhere -- see
             * plc_system_cmd_service.h's doc-comment on why a successful
             * reboot has no "after" to report DONE in. */
            break;

        case SPLC_SYSTEM_CMD_FACTORY_RESET:
        case SPLC_SYSTEM_CMD_CLEAR_RULES:
        case SPLC_SYSTEM_CMD_CLEAR_RETAIN:
            /*
             * Accepted over Modbus (write_system_command() already set
             * status=ACCEPTED) but deliberately NOT acted on here yet.
             * Unlike REBOOT, these three destroy data (Flash-erase the
             * rule table and/or the retain store) and "factory default"
             * for this SKU has not been pinned down yet -- e.g. does
             * FACTORY_RESET clear tag/network config too, or only rules +
             * retain, and does it reboot afterwards? See docs/handoff.md
             * for the open question. Implementing this with a guessed
             * scope risks destroying more (or less) than the product
             * actually wants "factory reset" to mean, which is worse
             * than leaving it visibly unimplemented (ACCEPTED forever,
             * never DONE) until answered.
             */
            break;

        default:
            /* write_system_command() already rejects any value outside
             * SPLC_SystemCommand with SPLC_CMD_STATUS_ERROR before it
             * would ever become s_pending_system_command, so this is
             * unreachable in practice -- kept only so the switch has no
             * silently-unhandled case if the enum grows later. */
            break;
    }
}