#ifndef __SX_GPIO_H__
#define __SX_GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    SX_GPIO_LOW = 0,
    SX_GPIO_HIGH = 1
} sx_gpio_value_t;

typedef enum {
    SX_GPIO_MODE_INPUT = 0,
    SX_GPIO_MODE_OUTPUT_PP = 1,
} sx_gpio_mode_t;

typedef struct {
    void            *port;
    uint16_t         pin;
    sx_gpio_mode_t   mode;
} sx_gpio_pin_t;

/*
 * Configures the pin according to pin->mode (input or push-pull output)
 * and calls HAL_GPIO_Init() -- does not rely on MX_GPIO_Init() having
 * already configured this specific pin/mode.
 *
 * initial_value is only applied when pin->mode == SX_GPIO_MODE_OUTPUT_PP;
 * it is ignored for SX_GPIO_MODE_INPUT (pass SX_GPIO_LOW as a no-op value).
 */
void sx_gpio_init(sx_gpio_pin_t *pin, sx_gpio_value_t initial_value);

/* Only valid for pins configured as SX_GPIO_MODE_INPUT. */
sx_gpio_value_t sx_gpio_read(sx_gpio_pin_t *pin);

/* Only valid for pins configured as SX_GPIO_MODE_OUTPUT_PP. */
void sx_gpio_write(sx_gpio_pin_t *pin, sx_gpio_value_t value);

#ifdef __cplusplus
}
#endif

#endif