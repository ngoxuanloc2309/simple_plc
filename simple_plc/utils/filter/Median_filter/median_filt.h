/* SPDX-License-Identifier: MIT */
#ifndef MEDIAN_FILT_H
#define MEDIAN_FILT_H

#include <stdint.h>

/*
 * Usage:
 *   #define WINDOW 7
 *   float buf[WINDOW * 2];   // needs 2x window
 *   median_t filt;
 *   median_init(&filt, buf, WINDOW);
 *   out = median(&filt, sample);
 *
 * Window size must be odd for a unique median.
 */

typedef struct {
    float    *ring;     /* circular buffer — insertion order  */
    float    *sorted;   /* sorted copy of the window          */
    unsigned  size;     /* window size                        */
    unsigned  idx;      /* write index into ring              */
} median_t;

/**
 * Initialize filter state.
 * @param filt   Pointer to filter state
 * @param buf    Pointer to caller-allocated buffer of `size * 2` floats
 * @param size   Size of buffer
 */
void  median_init(median_t *filt, float *buf, unsigned size);

/** Clear buffers to zero. Keeps size and buffer pointer. */
void  median_reset(median_t *filt);

/**
 * @param filt   Pointer to filter state
 * @param sample Input sample
 * @return       Filtered output
 */
float median(median_t *filt, float sample);

#endif /* MEDIAN_FILT_H */
