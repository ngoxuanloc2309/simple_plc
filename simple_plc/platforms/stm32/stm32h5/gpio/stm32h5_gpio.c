#include "stm32h5_gpio.h"

#if STM32H5_PLATFORM

void sx_gpio_init(sx_gpio_pin_t *pin, sx_gpio_value_t initial_value)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin   = pin->pin;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    if (pin->mode == SX_GPIO_MODE_OUTPUT_PP) {
        GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    } else {
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    }

    HAL_GPIO_Init((GPIO_TypeDef *)pin->port, &GPIO_InitStruct);

    /* initial_value only applies to outputs; irrelevant for inputs. */
    if (pin->mode == SX_GPIO_MODE_OUTPUT_PP) {
        HAL_GPIO_WritePin((GPIO_TypeDef *)pin->port, pin->pin,
                           (initial_value == SX_GPIO_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

sx_gpio_value_t sx_gpio_read(sx_gpio_pin_t *pin)
{
    GPIO_PinState state = HAL_GPIO_ReadPin((GPIO_TypeDef *)pin->port, pin->pin);
    return (state == GPIO_PIN_SET) ? SX_GPIO_HIGH : SX_GPIO_LOW;
}

void sx_gpio_write(sx_gpio_pin_t *pin, sx_gpio_value_t value)
{
    HAL_GPIO_WritePin((GPIO_TypeDef *)pin->port, pin->pin,
                       (value == SX_GPIO_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

#endif // STM32H5_PLATFORM