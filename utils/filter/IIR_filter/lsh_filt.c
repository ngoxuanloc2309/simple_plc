/* SPDX-License-Identifier: MIT */

#include "lsh_filt.h"
#include <math.h>

void iir_lsh_init(iir_lsh_t *filt, float dbGain, float freq,
                   float srate, float bandwidth)
{
    float gain      = powf(10.0f, dbGain * 0.025f);
    float omega     = 6.2831853f * freq / srate;
    float sin_w     = sinf(omega);
    float cos_w     = cosf(omega);
    float alpha     = sin_w * sinhf(0.34657359f * bandwidth * omega / sin_w);
    float sqrt_gain = sqrtf(gain);
    float gp1       = gain + 1.0f;
    float gm1       = gain - 1.0f;
    float two_sa    = 2.0f * sqrt_gain * alpha;
    float inv_norm  = 1.0f / (gp1 + gm1 * cos_w + two_sa);

    filt->b0 = (gain * (gp1 - gm1 * cos_w + two_sa)) * inv_norm;
    filt->b1 = (2.0f * gain * (gm1 - gp1 * cos_w))   * inv_norm;
    filt->b2 = (gain * (gp1 - gm1 * cos_w - two_sa)) * inv_norm;
    filt->a1 = (-2.0f * (gm1 + gp1 * cos_w))         * inv_norm;
    filt->a2 = (gp1 + gm1 * cos_w - two_sa)           * inv_norm;

    filt->d1 = 0;
    filt->d2 = 0;
}
