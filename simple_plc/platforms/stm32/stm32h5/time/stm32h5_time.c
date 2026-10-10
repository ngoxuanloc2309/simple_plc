#include "stm32h5_time.h"
#include "sx_os_config.h"

#if STM32H5_PLATFORM

void sx_delay_ms(uint32_t ms)
{
#if SX_NO_OS
    HAL_Delay(ms);
#else

#endif
}

void sx_delay_s(uint32_t s)
{
    HAL_Delay(s * 1000);
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