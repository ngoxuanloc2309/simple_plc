#include "stm32h5_system.h"

#if STM32H5_PLATFORM

void sx_system_reset(void)
{
    NVIC_SystemReset();
    /* NVIC_SystemReset() does not return -- the line below is
     * unreachable, but keeps static analyzers that don't know that
     * quiet, and documents the contract explicitly. */
    for (;;) { }
}

#endif // STM32H5_PLATFORM  