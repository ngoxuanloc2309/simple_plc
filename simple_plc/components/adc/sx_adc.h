#ifndef SX_ADC_H
#define SX_ADC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct{
    void   *instance;
    uint8_t channel;
} sx_adc_config_t;

typedef enum{
    SX_ADC_RESOLUTION_12BIT = 0,
    SX_ADC_RESOLUTION_10BIT,
    SX_ADC_RESOLUTION_8BIT,
    SX_ADC_RESOLUTION_6BIT
} sx_adc_resolution_t;

/*
 * Configures the ADC instance/channel referenced by config for single,
 * polled conversions at the given resolution. Does not start a conversion.
 * Relies on MX_ADCx_Init() (CubeMX) having already enabled the ADC
 * peripheral clock and configured the analog GPIO pin -- this only
 * (re)configures resolution and the specific channel used by sx_adc_read().
 */
void sx_adc_init(sx_adc_config_t *config, sx_adc_resolution_t resolution);

/*
 * Blocking single-shot read: starts a conversion, polls until it
 * completes (or a fixed HAL timeout), reads the raw value, then stops
 * the ADC. Safe to call repeatedly from a fixed-cycle scan loop.
 * Returns 0 on timeout/error.
 */
uint32_t sx_adc_read(sx_adc_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /*SX_ADC_H*/