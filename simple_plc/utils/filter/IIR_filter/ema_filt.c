/* SPDX-License-Identifier: MIT */

#include "ema_filt.h"
#include <math.h>

void iir_ema_init(iir_ema_t *filt, float freq, float srate)
{
    float rc = 1.0f / (6.2831853f * freq);
    float dt = 1.0f / srate;

    filt->alpha = dt / (rc + dt);
    filt->out   = 0;
}
