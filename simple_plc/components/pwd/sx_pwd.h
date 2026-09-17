#ifndef SX_POWER_H
#define SX_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers a callback to run when the platform detects the supply
 * voltage has dropped below a safe threshold -- an early warning that
 * power loss is imminent, while there is still (briefly) enough energy
 * left to run code. On STM32H5 this is backed by the PVD (Programmable
 * Voltage Detector) peripheral; a different platform/chip could back it
 * with whatever brownout-detection mechanism it has, without this
 * caller-facing contract changing.
 *
 * Intended use: register a handler that does the minimum necessary work
 * to persist critical state before power is actually lost (e.g.
 * services/plc_retain.c's emergency Flash snapshot write) -- not general
 * app logic. The callback runs in interrupt context (on STM32H5, from
 * the PVD/AVD EXTI line ISR), so it must be short, must not block, and
 * must not call anything that itself waits on interrupts being enabled.
 *
 * Only ONE callback can be registered at a time -- calling this again
 * replaces whatever was registered before, it does not add a second
 * handler. This is a deliberate simplification: today only one thing
 * (retain's emergency snapshot) needs this, and a single fixed slot
 * avoids adding a list/array plus its own MAX_CALLBACKS constant for a
 * feature nothing yet needs. If a second caller ever needs this too,
 * revisit then rather than building it speculatively now.
 *
 * callback: function to call when low voltage is detected. Pass NULL to
 *           unregister (stop calling anything).
 */
void sx_power_register_low_voltage_callback(void (*callback)(void));

#ifdef __cplusplus
}
#endif

#endif /* SX_POWER_H */