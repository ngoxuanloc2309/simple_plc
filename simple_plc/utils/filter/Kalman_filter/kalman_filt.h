/* SPDX-License-Identifier: MIT */

#ifndef KALMAN_FILT_H
#define KALMAN_FILT_H

#include <stdint.h>

/*
 * Usage:
 *   kalman_t filt;
 *   kalman_init(&filt, proc_noise, meas_noise, est0, unc0);
 *   float filtered = kalman(&filt, measurement);
 */

typedef struct {
    float proc_noise;   /* process noise covariance (q)     */
    float meas_noise;   /* measurement noise covariance (r)  */
    float est;          /* current state estimate            */
    float err_cov;      /* estimate error covariance         */
    float gain;         /* Kalman gain (last computed)        */
} kalman_t;

/**
 * Set tuning and initial state. Call once before processing.
 * @param filt        Pointer to filter state
 * @param proc_noise  Drift rate — larger = faster tracking, noisier output
 * @param meas_noise  Sensor noise — larger = smoother output, slower response
 * @param est0        Initial estimate (e.g. first reading)
 * @param unc0        Initial uncertainty (large = low confidence)
 */
void kalman_init(kalman_t *filt, float proc_noise, float meas_noise,
                 float est0, float unc0);

/** Reset estimate and uncertainty. Keeps tuning (proc_noise, meas_noise). */
static inline void kalman_reset(kalman_t *filt, float est0, float unc0)
{
    filt->est     = est0;
    filt->err_cov = unc0;
    filt->gain    = 0;
}

/**
 * @param filt   Pointer to filter state
 * @param meas   Measurement input
 * @return       Updated state estimate
 */
static inline float kalman(kalman_t *filt, float meas)
{
    float err_cov = filt->err_cov + filt->proc_noise;
    float gain = err_cov / (err_cov + filt->meas_noise);
    float est  = filt->est + gain * (meas - filt->est);

    filt->err_cov = err_cov * (1.0f - gain);
    filt->gain    = gain;
    filt->est     = est;

    return est;
}

#endif /* KALMAN_FILT_H */
