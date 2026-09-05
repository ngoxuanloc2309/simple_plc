/* SPDX-License-Identifier: MIT */

#ifndef IIR_EMA_FILT_H
#define IIR_EMA_FILT_H
#include <stdint.h>

/*
 * Usage:
 *   iir_ema_t filt;
 *   iir_ema_init(&filt, cutoff_freq, srate);
 *   out = iir_ema(&filt, sample);
 */

typedef struct {
    float alpha;    /* smoothing factor (0..1) */
    float out;      /* previous output         */
} iir_ema_t;

/** 
 * @param freq   Cutoff frequency in Hz
 * @param srate  Sampling rate in Hz
 */
void iir_ema_init(iir_ema_t *filt, float freq, float srate);

/** Clear output to zero. Keeps alpha. */
static inline void iir_ema_reset(iir_ema_t *filt)
{
    filt->out = 0;
}

/**
 * @param filt   Pointer to filter state
 * @param sample Input sample
 * @return       Filtered output
 */
static inline float iir_ema(iir_ema_t *filt, float sample)
{
    filt->out += filt->alpha * (sample - filt->out);
    return filt->out;
}

#endif /* IIR_EMA_FILT_H */
