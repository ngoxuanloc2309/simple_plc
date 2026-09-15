#ifndef SX_ADC_H
#define SX_ADC_H

#include <stdint.h>

typedef struct{
    void *instance;
    uint8_t channel;
} sx_adc_config_t;

typedef enum{
    SX_ADC_RESOLUTION_12BIT = 0,
    SX_ADC_RESOLUTION_10BIT,
    SX_ADC_RESOLUTION_8BIT,
    SX_ADC_RESOLUTION_6BIT
} sx_adc_resolution_t;

void sx_adc_init(sx_adc_config_t *config, sx_adc_resolution_t resolution);
uint32_t sx_adc_read(sx_adc_config_t *config);

#endif /*SX_ADC_H*/