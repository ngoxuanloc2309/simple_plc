#ifndef SX_TIME_H
#define SX_TIME_H

#include "stdint.h"

void sx_delay_ms(uint32_t ms);
void sx_delay_s(uint32_t s);
uint32_t sx_get_tick_ms(void);
uint32_t sx_get_tick_s(void);

#endif