/* SPDX-License-Identifier: MIT */

#include "peq_filt.h"
#include <math.h>

void iir_peq_init(iir_peq_t *filt, float dbGain, float freq,
                   float srate, float bandwidth)
{
    float gain     = powf(10.0f, dbGain * 0.025f);
    float omega    = 6.2831853f * freq / srate;
    float sin_w    = sinf(omega);
    float cos_w    = cosf(omega);
    float alpha    = sin_w * sinhf(0.34657359f * bandwidth * omega / sin_w);
    float inv_norm = 1.0f / (1.0f + alpha / gain);

    filt->b0 = (1.0f + alpha * gain) * inv_norm;
    filt->b1 = -2.0f * cos_w * inv_norm;
    filt->b2 = (1.0f - alpha * gain) * inv_norm;
    filt->a1 =  filt->b1;
    filt->a2 = (1.0f - alpha / gain) * inv_norm;

    filt->d1 = 0;
    filt->d2 = 0;
}
