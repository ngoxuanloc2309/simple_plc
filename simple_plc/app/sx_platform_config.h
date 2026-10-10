#ifndef SX_PLATFORM_CONFIG_H
#define SX_PLATFORM_CONFIG_H

/*
 * Chip-family flags tested by the Layer 0/1 sources (#if STM32H5_PLATFORM).
 * Derived from the single product option SPLC_PLATFORM (splcopts.h, defaults
 * in config/splc_opt.h); do not define them by hand.
 */
#include "splc_opt.h"

#define STM32H5_PLATFORM    (SPLC_PLATFORM == SPLC_PLATFORM_STM32H5)
#define STM32F1_PLATFORM    (SPLC_PLATFORM == SPLC_PLATFORM_STM32F1)
#define STM32F4_PLATFORM    (SPLC_PLATFORM == SPLC_PLATFORM_STM32F4)
#define STM32H7_PLATFORM    (SPLC_PLATFORM == SPLC_PLATFORM_STM32H7)
#define ESP32_PLATFORM      (SPLC_PLATFORM == SPLC_PLATFORM_ESP32)

#endif    // SX_PLATFORM_CONFIG_H