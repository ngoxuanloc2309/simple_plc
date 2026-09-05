/* SPDX-License-Identifier: MIT */

#include "median_filt.h"
#include <string.h>

void median_init(median_t *filt, float *buf, unsigned size)
{
    filt->ring   = buf;
    filt->sorted = buf + size;
    filt->size   = size;
    filt->idx    = 0;

    memset(buf, 0, 2 * size * sizeof(float));
}

void median_reset(median_t *filt)
{
    filt->idx = 0;
    memset(filt->ring, 0, 2 * filt->size * sizeof(float));
}

float median(median_t *filt, float sample)
{
    float *ring   = filt->ring;
    float *sorted = filt->sorted;
    const unsigned size     = filt->size;
    unsigned idx = filt->idx;
    unsigned pos;

    float oldest = ring[idx];
    ring[idx] = sample;
    if (++idx >= size)
        idx = 0;
    filt->idx = idx;

    for (pos = 0; pos < size; pos++) {
        if (sorted[pos] == oldest)
            break;
    }
    unsigned tail = size - 1 - pos;
    if (tail)
        memmove(&sorted[pos], &sorted[pos + 1], tail * sizeof(float));

    for (pos = size - 1; pos > 0; pos--) {
        if (sorted[pos - 1] <= sample)
            break;
        sorted[pos] = sorted[pos - 1];
    }
    sorted[pos] = sample;

    return sorted[size >> 1];
}
