#include "stm32h5_flash.h"

#if STM32H5_PLATFORM
void sx_flash_lock(void)
{
    HAL_FLASH_Lock();
}

void sx_flash_unlock(void)
{
    HAL_FLASH_Unlock();
}
#endif // STM32H5_PLATFORM