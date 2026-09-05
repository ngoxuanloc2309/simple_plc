/* SPDX-License-Identifier: MIT */

#include "bpf_filt.h"
#include <math.h>

void iir_bpf_init(iir_bpf_t *filt, float freq, float srate, float bandwidth)
{
    float omega    = 6.2831853f * freq / srate;
    float sin_w    = sinf(omega);
    float cos_w    = cosf(omega);
    float alpha    = sin_w * sinhf(0.34657359f * bandwidth * omega / sin_w);
    float inv_norm = 1.0f / (1.0f + alpha);

    filt->b0 =  alpha * inv_norm;
    filt->b1 =  0;
    filt->b2 = -filt->b0;
    filt->a1 = -2.0f * cos_w * inv_norm;
    filt->a2 = (1.0f - alpha) * inv_norm;

    filt->d1 = 0;
    filt->d2 = 0;
}
