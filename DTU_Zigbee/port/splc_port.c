#include "splc_port.h"
#include "splc_opt.h"

#include <stddef.h>

#include "splc_log.h"
#include "plc_engine.h"
#include "splc_tcp_port.h"
#include "splc_rtc_port.h"
#include "splc_mqtt_port.h"
#include "splc_host_rs485_port.h"
#include "splc_host_zigbee_port.h"

#include "FreeRTOS.h"
#include "task.h"

#if !SX_OS_USE_FREERTOS
#error "DTU Zigbee port: SX_OS_USE_FREERTOS must be 1 (the engine runs in a FreeRTOS task)"
#endif

/* Engine task parameters. Stack is in WORDS (4 bytes). These are first guesses, NOT measured on
 * the board: check uxTaskGetStackHighWaterMark() after a Flash deploy / COMMIT before trusting. */
#ifndef SPLC_ENGINE_TASK_STACK_WORDS
#define SPLC_ENGINE_TASK_STACK_WORDS 2048U
#endif
#ifndef SPLC_ENGINE_TASK_PRIORITY
#define SPLC_ENGINE_TASK_PRIORITY    (tskIDLE_PRIORITY + 2U)
#endif

void splc_port_init(void)
{
    /* The host must already have initialised its logger and installed the RS485 UARTs with
     * sx_uart (SimplePLC owns no UART). The USB-CDC App link is registered by the library itself
     * in board_hw_init() (SPLC_PORT_USB_CDC = 1). */
    splc_host_rs485_port_init();
    splc_host_zigbee_port_init();
    splc_rtc_port_init();
    splc_mqtt_port_init();
}

static void engine_task(void *arg)
{
    (void)arg;
    splc_port_init();
    plc_engine_init();
    (void)splc_tcp_port_attach();
    for (;;) {
        plc_engine_poll();
        vTaskDelay(1);
    }
}

int splc_port_start(void)
{
    static TaskHandle_t s_task;
    if (s_task != NULL) {
        return 0;               /* already started */
    }
    if (xTaskCreate(engine_task, "splc_engine", SPLC_ENGINE_TASK_STACK_WORDS, NULL,
                    SPLC_ENGINE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return -1;
    }
    return 0;
}