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
    sConfig.SamplingTime = ADC_SAMPLETIME_COMMON_1;
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