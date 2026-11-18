#include "stm32h5_pwd.h"

#if STM32H5_PLATFORM

/*
 * PVD (Programmable Voltage Detector) low-voltage early warning.
 *
 * What CubeMX already provides (Core/Src/stm32h5xx_hal_msp.c,
 * HAL_MspInit()): the PVD threshold (PWR_PVDLEVEL_4), interrupt on BOTH
 * edges (PWR_PVD_MODE_IT_RISING_FALLING), and HAL_PWR_EnablePVD(). That
 * configures the detector and its EXTI line ONLY.
 *
 * Edge polarity: the PVD output (PWR_VMSR.PVDO) is 1 while VDD is BELOW the
 * threshold, so the EXTI RISING edge is "VDD dropped" and the FALLING edge is
 * "VDD recovered" -- the opposite of what the names suggest. A falling-edge-only
 * configuration (CubeMX "falling") fires when the supply comes BACK, never at
 * power loss, and the emergency retain write then never runs. Both edges are
 * enabled and HAL_PWR_PVDCallback() below acts only while PVDO = 1, so the
 * result does not depend on the polarity convention. It does NOT enable PVD_AVD_IRQn in the NVIC and
 * does NOT generate PVD_AVD_IRQHandler() (the .ioc has no NVIC entry for
 * it, and startup_stm32h523xx.s maps the vector to Default_Handler). This
 * file supplies both, so no CubeMX-generated file has to be edited:
 *
 *   - PVD_AVD_IRQHandler(): strong definition overriding the startup
 *     file's weak alias; forwards to HAL_PWR_PVD_IRQHandler(), which calls
 *     HAL_PWR_PVDCallback() below and then clears the EXTI pending bits.
 *   - sx_power_register_low_voltage_callback(): registering a callback
 *     arms the NVIC line, passing NULL disarms it. Nothing runs from the
 *     PVD interrupt until a callback is registered.
 *
 * If the NVIC entry is ever enabled in CubeMX instead (which then
 * generates its own PVD_AVD_IRQHandler()), define
 * SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX to drop the definition here, otherwise
 * the link fails with a duplicate symbol.
 */

#ifndef SX_POWER_PVD_IRQ_PRIORITY
/* Highest preemption priority: the handler must be able to preempt every
 * other interrupt (UART, TIM, DMA all sit at 0..1 in the .ioc; SysTick is
 * 15). Equal priority does not preempt a running handler, it waits. */
#define SX_POWER_PVD_IRQ_PRIORITY 0U
#endif

/* Single callback slot -- see the "only ONE callback" note in
 * sx_pwd.h for why this isn't a list. */
static void (*volatile s_low_voltage_cb)(void) = NULL;

static void sx_power_clear_pvd_pending(void)
{
    WRITE_REG(EXTI->RPR1, PWR_EXTI_LINE_PVD);
    WRITE_REG(EXTI->FPR1, PWR_EXTI_LINE_PVD);
}

void sx_power_register_low_voltage_callback(void (*callback)(void))
{
    if (callback == NULL) {
        HAL_NVIC_DisableIRQ(PVD_AVD_IRQn);
        s_low_voltage_cb = NULL;
        return;
    }

    s_low_voltage_cb = callback;

    /* Drop any edge latched before a callback existed, so arming never
     * fires on a stale event. */
    sx_power_clear_pvd_pending();
    HAL_NVIC_ClearPendingIRQ(PVD_AVD_IRQn);
    HAL_NVIC_SetPriority(PVD_AVD_IRQn, SX_POWER_PVD_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(PVD_AVD_IRQn);
}

#ifndef SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX
void PVD_AVD_IRQHandler(void)
{
    HAL_PWR_PVD_IRQHandler();
}
#endif

/* Overrides the __weak HAL_PWR_PVDCallback(void) declared in
 * Drivers/STM32H5xx_HAL_Driver/Inc/stm32h5xx_hal_pwr.h. Called by
 * HAL_PWR_PVD_IRQHandler(), which clears the EXTI pending bits right
 * after this returns, so nothing needs to be cleared here.
 *
 * Runs in interrupt context -- see sx_pwd.h's callback contract (short,
 * non-blocking, no logging) for what s_low_voltage_cb may do.
 */
void HAL_PWR_PVDCallback(void)
{
    /* Both edges interrupt: only the "VDD below the threshold" one is a power loss. */
    if (!__HAL_PWR_GET_FLAG(PWR_FLAG_PVDO)) {
        return;
    }
    void (*cb)(void) = s_low_voltage_cb;
    if (cb != NULL) {
        cb();
    }
}

#endif // STM32H5_PLATFORM