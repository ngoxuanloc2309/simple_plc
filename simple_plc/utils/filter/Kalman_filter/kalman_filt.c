/* SPDX-License-Identifier: MIT */

#include "kalman_filt.h"

void kalman_init(kalman_t *filt, float proc_noise, float meas_noise,
                 float est0, float unc0)
{
    filt->proc_noise = proc_noise;
    filt->meas_noise = meas_noise;
    filt->est        = est0;
    filt->err_cov    = unc0;
    filt->gain       = 0;
}
