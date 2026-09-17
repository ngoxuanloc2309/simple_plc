#include "stm32h5_pwd.h"

#if STM32H5_PLATFORM

/* Single callback slot -- see the "only ONE callback" note in
 * sx_pwd.h for why this isn't a list. */
static void (*s_low_voltage_cb)(void) = NULL;

void sx_power_register_low_voltage_callback(void (*callback)(void))
{
    s_low_voltage_cb = callback;
}

/* Overrides the __weak HAL_PWR_PVDCallback(void) declared in
 * Drivers/STM32H5xx_HAL_Driver/Inc/stm32h5xx_hal_pwr.h. Called by
 * HAL_PWR_PVD_IRQHandler() (Drivers/STM32H5xx_HAL_Driver/Src/
 * stm32h5xx_hal_pwr.c), which itself is called from
 * PVD_AVD_IRQHandler() (Core/Src/stm32h5xx_it.c, CubeMX-generated) --
 * this only runs once the PVD is actually enabled and its NVIC line is
 * enabled in the .ioc (both done -- see docs/handoff.md section 1.7 for
 * the full chain and the level/mode chosen).
 *
 * HAL_PWR_PVD_IRQHandler() already clears the EXTI pending bit for the
 * PVD line right after calling this callback (see its implementation),
 * so nothing needs to be cleared here.
 *
 * Runs in interrupt context -- see sx_power.h's callback contract
 * (must be short, non-blocking) for what s_low_voltage_cb is allowed to
 * do.
 */
void HAL_PWR_PVDCallback(void)
{
    if (s_low_voltage_cb != NULL) {
        s_low_voltage_cb();
    }
}

#endif // STM32H5_PLATFORM