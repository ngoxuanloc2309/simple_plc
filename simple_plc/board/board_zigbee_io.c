#include "board.h"
#include "board_zigbee_io.h"
#include "sx_gpio.h"
#include "plc_io.h"
#include "plc_tag_def.h"

/*
 * board_zigbee_io.c - Layer 4
 *
 * board_hw_init() for the Zigbee-IO SKU (4 DI / 4 DO / 0 AI, see
 * board_zigbee_io.h). Owns the sx_gpio_pin_t storage for every channel
 * (plc_io_register_di/do() keep the pointer, not a copy) and does the
 * one-time sx_gpio_init() + plc_io_register_*() call per channel.
 *
 * Only this .c is linked into a Zigbee-IO firmware image -- a different
 * SKU (e.g. future board_remoteio.c) provides its own board_hw_init()
 * instead; the two are never compiled into the same target.
 */

static sx_gpio_pin_t s_di_pins[4];
static sx_gpio_pin_t s_do_pins[4];

void board_hw_init(void)
{
    s_di_pins[0] = (sx_gpio_pin_t){ .port = DI0_PORT, .pin = DI0_PIN, .mode = SX_GPIO_MODE_INPUT };
    s_di_pins[1] = (sx_gpio_pin_t){ .port = DI1_PORT, .pin = DI1_PIN, .mode = SX_GPIO_MODE_INPUT };
    s_di_pins[2] = (sx_gpio_pin_t){ .port = DI2_PORT, .pin = DI2_PIN, .mode = SX_GPIO_MODE_INPUT };
    s_di_pins[3] = (sx_gpio_pin_t){ .port = DI3_PORT, .pin = DI3_PIN, .mode = SX_GPIO_MODE_INPUT };

    s_do_pins[0] = (sx_gpio_pin_t){ .port = DO0_PORT, .pin = DO0_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };
    s_do_pins[1] = (sx_gpio_pin_t){ .port = DO1_PORT, .pin = DO1_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };
    s_do_pins[2] = (sx_gpio_pin_t){ .port = DO2_PORT, .pin = DO2_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };
    s_do_pins[3] = (sx_gpio_pin_t){ .port = DO3_PORT, .pin = DO3_PIN, .mode = SX_GPIO_MODE_OUTPUT_PP };

    for (int i = 0; i < 4; i++) {
        sx_gpio_init(&s_di_pins[i], SX_GPIO_LOW);
        plc_io_register_di((uint16_t)(TAG_DI0 + i), &s_di_pins[i]);
    }

    for (int i = 0; i < 4; i++) {
        sx_gpio_init(&s_do_pins[i], SX_GPIO_LOW);
        plc_io_register_do((uint16_t)(TAG_DO0 + i), &s_do_pins[i]);
    }
}