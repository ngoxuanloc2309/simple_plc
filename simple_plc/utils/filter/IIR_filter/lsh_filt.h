/* SPDX-License-Identifier: MIT */

#ifndef IIR_LSH_FILT_H
#define IIR_LSH_FILT_H

#include <stdint.h>

/*
 * Usage:
 *   iir_lsh_t filt;
 *   iir_lsh_init(&filt, dbGain, freq, srate, bandwidth);
 *   out = iir_lsh(&filt, sample);
 */

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float d1, d2;
} iir_lsh_t;

/**
 * @param filt     Pointer to filter state
 * @param dbGain   Gain at shelf in dB
 * @param freq     Cutoff frequency in Hz
 * @param srate    Sampling rate in Hz
 * @param bandwidth  Bandwidth in Hz
 */
void iir_lsh_init(iir_lsh_t *filt, float dbGain, float freq,
                   float srate, float bandwidth);

/** Clear delay state to zero. Coefficients are kept. */
static inline void iir_lsh_reset(iir_lsh_t *filt)
{
    filt->d1 = 0;
    filt->d2 = 0;
}

/**
 * @param filt   Pointer to filter state
 * @param sample Input sample
 * @return       Filtered output
 */
static inline float iir_lsh(iir_lsh_t *filt, float sample)
{
    float out  = filt->b0 * sample + filt->d1;
    filt->d1 = filt->b1 * sample - filt->a1 * out + filt->d2;
    filt->d2 = filt->b2 * sample - filt->a2 * out;
    return out;
}

#endif /* IIR_LSH_FILT_H */
