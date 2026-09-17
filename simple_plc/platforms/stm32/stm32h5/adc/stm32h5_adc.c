#include "stm32h5_adc.h"

#if STM32H5_PLATFORM
#include "adc.h"
/* Blocking conversion timeout, in milliseconds. Generous for a 10ms scan
 * loop budget -- a real timeout here means a hardware/config problem, not
 * a value worth waiting longer for. */
#define SX_ADC_POLL_TIMEOUT_MS 5U

static uint32_t sx_adc_resolution_to_hal(sx_adc_resolution_t resolution)
{
    switch (resolution) {
        case SX_ADC_RESOLUTION_12BIT: return ADC_RESOLUTION_12B;
        case SX_ADC_RESOLUTION_10BIT: return ADC_RESOLUTION_10B;
        case SX_ADC_RESOLUTION_8BIT:  return ADC_RESOLUTION_8B;
        case SX_ADC_RESOLUTION_6BIT:  return ADC_RESOLUTION_6B;
        default:                     return ADC_RESOLUTION_12B;
    }
}

void sx_adc_init(sx_adc_config_t *config, sx_adc_resolution_t resolution)
{
    ADC_HandleTypeDef *hadc = (ADC_HandleTypeDef *)config->instance;

    /* Relies on MX_ADCx_Init() having already enabled the ADC clock and
     * configured the analog GPIO pin -- this only (re)applies resolution
     * and sets up the single channel this config uses for conversion. */
    hadc->Init.Resolution = sx_adc_resolution_to_hal(resolution);
    HAL_ADC_Init(hadc);

    ADC_ChannelConfTypeDef sConfig = {0};
    sConfig.Channel      = config->channel;
    sConfig.Rank         = ADC_REGULAR_RANK_1;
    /* BUGFIX: was ADC_SAMPLETIME_COMMON_1, which does not exist anywhere
     * in the real STM32H5 HAL (Drivers/STM32H5xx_HAL_Driver/Inc/
     * stm32h5xx_hal_adc.h) -- confirmed by grepping every ADC_SAMPLETIME_*
     * macro that header actually defines. This only surfaced once
     * Core/Inc/adc.h existed (CubeMX-generated after configuring IN1 in
     * the .ioc) and this .c file actually got compiled for the first
     * time; it was never reachable before that.
     *
     * ADC_SAMPLETIME_247CYCLES_5 chosen as a reasonable default for a
     * general-purpose 4AI analog input (not a high-speed/high-frequency
     * signal) -- long enough sampling time for a stable reading on a
     * source with moderate impedance, short enough to comfortably fit
     * inside the 10ms scan-loop budget this driver is designed for (see
     * SX_ADC_POLL_TIMEOUT_MS above). This is a real design choice, not
     * just a compile-fix -- revisit if the actual analog input source
     * impedance/bandwidth for this product's 4AI channels is specified
     * elsewhere and calls for a different value. */
    sConfig.SamplingTime = ADC_SAMPLETIME_247CYCLES_5;
    HAL_ADC_ConfigChannel(hadc, &sConfig);
}

uint32_t sx_adc_read(sx_adc_config_t *config)
{
    ADC_HandleTypeDef *hadc = (ADC_HandleTypeDef *)config->instance;
    uint32_t value = 0;

    if (HAL_ADC_Start(hadc) != HAL_OK) {
        return 0;
    }

    if (HAL_ADC_PollForConversion(hadc, SX_ADC_POLL_TIMEOUT_MS) == HAL_OK) {
        value = HAL_ADC_GetValue(hadc);
    }

    HAL_ADC_Stop(hadc);

    return value;
}

#endif // STM32H5_PLATFORM