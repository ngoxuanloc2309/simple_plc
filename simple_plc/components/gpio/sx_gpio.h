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

typedef struct {
    void    *port;
    uint16_t pin;
} sx_gpio_pin_t;

void sx_gpio_init(sx_gpio_pin_t *pin, sx_gpio_value_t initial_value);

sx_gpio_value_t sx_gpio_read(sx_gpio_pin_t *pin);

void sx_gpio_write(sx_gpio_pin_t *pin, sx_gpio_value_t value);

#ifdef __cplusplus
}
#endif

#endif