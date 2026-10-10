#include "stm32h5_time.h"
#include "sx_os.h"

#if STM32H5_PLATFORM

/*
 * Delay policy (see components/os/sx_os.h):
 *   - bare-metal (SX_OS_USE_FREERTOS = 0): sx_os_is_running() is an inline
 *     "false", so this is exactly HAL_Delay().
 *   - FreeRTOS: once the scheduler runs, sleep through it so other tasks get
 *     the CPU; before that (early boot) HAL_Delay() is still the only option.
 * sx_get_tick_ms() is NOT affected: it always reads HAL_GetTick().
 */
void sx_delay_ms(uint32_t ms)
{
    if (sx_os_is_running()) {
        sx_os_sleep_ms(ms);
        return;
    }
    HAL_Delay(ms);
}

void sx_delay_s(uint32_t s)
{
    /* Sleep in 1 s slices: s * 1000 would overflow uint32_t above ~49 days. */
    while (s--) {
        sx_delay_ms(1000U);
    }
}

uint32_t sx_get_tick_ms(void)
{
    return HAL_GetTick();
}

uint32_t sx_get_tick_s(void)
{
    return HAL_GetTick() / 1000;
}

#endif // STM32H5_PLATFORM