#include "stm32h5_gpio.h"

void sx_gpio_init(sx_gpio_pin_t *pin, sx_gpio_value_t initial_value)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // Configure GPIO pin
    GPIO_InitStruct.Pin = pin->pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init((GPIO_TypeDef *)pin->port, &GPIO_InitStruct);

    // Set initial value
    HAL_GPIO_WritePin((GPIO_TypeDef *)pin->port, pin->pin, (initial_value == SX_GPIO_HIGH) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}