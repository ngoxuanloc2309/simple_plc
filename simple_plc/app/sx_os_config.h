#ifndef SX_OS_CONFIG_H
#define SX_OS_CONFIG_H

/*
 * OS integration switch for the whole library.
 *
 * The switch itself, SX_OS_USE_FREERTOS, is a product option: set it in the
 * product's splcopts.h (default and meaning in config/splc_opt.h). This
 * header only derives what the library code tests, so the two can never
 * disagree.
 */
#include "splc_opt.h"

#if SX_OS_USE_FREERTOS
#define SX_NO_OS            0
#else
#define SX_NO_OS            1
#endif

#endif /* SX_OS_CONFIG_H */