/* SPDX-License-Identifier: MIT */
#include "ma_filt.h"

void ma_init(ma_t *filt, float *buf, unsigned size)
{
    filt->buf      = buf;
    filt->size     = size;
    filt->inv_size = 1.0f / (float)size;
    filt->sum      = 0;
    filt->idx      = 0;

    for (unsigned idx = 0; idx < size; idx++)
        buf[idx] = 0;
}

void ma_reset(ma_t *filt)
{
    filt->sum = 0;
    filt->idx = 0;

    for (unsigned idx = 0; idx < filt->size; idx++)
        filt->buf[idx] = 0;
}
