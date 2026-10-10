/*
 * sx_os_freertos.c - Layer 0 (Platform), FreeRTOS implementation of the
 * Layer 1 contract components/os/sx_os.h.
 * Compiles to nothing unless SX_OS_USE_FREERTOS = 1 (app/sx_os_config.h).
 */
#include "sx_os.h"

#if SX_OS_USE_FREERTOS

#include "FreeRTOS.h"
#include "task.h"

bool sx_os_is_running(void)
{
    return xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
}

void sx_os_sleep_ms(uint32_t ms)
{
    /* Round up: 1 ms at a 100 Hz tick must still sleep one tick, not zero. */
    uint64_t ticks = ((uint64_t)ms * (uint64_t)configTICK_RATE_HZ + 999U) / 1000U;
    vTaskDelay((TickType_t)ticks);
}

void sx_os_yield_wait(void)
{
    if (sx_os_is_running()) {
        vTaskDelay(1);
    }
}

#endif /* SX_OS_USE_FREERTOS */