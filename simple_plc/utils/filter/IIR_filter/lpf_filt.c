/* SPDX-License-Identifier: MIT */

#include "lpf_filt.h"
#include <math.h>

void iir_lpf_init(iir_lpf_t *filt, float freq, float srate, float bandwidth)
{
    float omega    = 6.2831853f * freq / srate; // 2 * pi * f / fs
    float sin_w    = sinf(omega);
    float cos_w    = cosf(omega);
    float alpha    = sin_w * sinhf(0.34657359f * bandwidth * omega / sin_w); // 0.5 * ln(2) = 0.34657359
    float inv_norm = 1.0f / (1.0f + alpha);

    filt->b0 = (1.0f - cos_w) * 0.5f * inv_norm;
    filt->b1 = (1.0f - cos_w) * inv_norm;
    filt->b2 = filt->b0;
    filt->a1 = -2.0f * cos_w * inv_norm;
    filt->a2 = (1.0f - alpha) * inv_norm;

    filt->d1 = 0;
    filt->d2 = 0;
}
