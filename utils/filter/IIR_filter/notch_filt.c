/* SPDX-License-Identifier: MIT */

#include "notch_filt.h"
#include <math.h>

void iir_notch_init(iir_notch_t *filt, float freq, float srate, float bandwidth)
{
    float omega    = 6.2831853f * freq / srate;
    float sin_w    = sinf(omega);
    float cos_w    = cosf(omega);
    float alpha    = sin_w * sinhf(0.34657359f * bandwidth * omega / sin_w);
    float inv_norm = 1.0f / (1.0f + alpha);

    filt->b0 =  inv_norm;
    filt->b1 = -2.0f * cos_w * inv_norm;
    filt->b2 =  filt->b0;
    filt->a1 =  filt->b1;
    filt->a2 = (1.0f - alpha) * inv_norm;

    filt->d1 = 0;
    filt->d2 = 0;
}
