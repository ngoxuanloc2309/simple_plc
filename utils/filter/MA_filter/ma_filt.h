/* SPDX-License-Identifier: MIT */
#ifndef MA_FILT_H
#define MA_FILT_H

#include <stdint.h>

/*
 * Usage:
 *   float buf[32];
 *   ma_t filt;
 *   ma_init(&filt, buf, 32);
 *   out = ma(&filt, sample);
 */

typedef struct {
    float    *buf;          /* circular buffer (caller-allocated) */
    float     sum;          /* running sum of buffer contents     */
    unsigned  size;         /* window size                        */
    unsigned  idx;          /* write index into circular buffer   */
    float     inv_size;     /* 1.0 / size — precomputed           */
} ma_t;

/**
* Initialize filter state.
* @param filt   Pointer to filter state
* @param buf    Caller-allocated buffer of `size` floats
* @param size   Size of buffer (must be > 0)
*/
void ma_init(ma_t *filt, float *buf, unsigned size);

/** Clear buffer and running sum to zero. Keeps size and buffer pointer. */
void ma_reset(ma_t *filt);

/**
 * @param filt   Pointer to filter state
 * @param sample Input sample
 * @return       Filtered output
 */
static inline float ma(ma_t *filt, float sample)
{
    unsigned idx = filt->idx;
    float sum = filt->sum - filt->buf[idx];

    filt->buf[idx] = sample;
    sum += sample;

    if (++idx >= filt->size)
        idx = 0;

    filt->idx = idx;
    filt->sum = sum;

    return sum * filt->inv_size;
}

#endif /* MA_FILT_H */
