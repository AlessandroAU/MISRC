/*
 * MISRC GUI - Anti-Aliasing Filter Implementation
 *
 * 4th order Butterworth lowpass IIR filter (cascade of two biquads).
 * -24 dB/octave rolloff for effective anti-aliasing.
 *
 * For 20 MHz sample rate at 75% Nyquist:
 *   Nyquist = 10 MHz
 *   Cutoff = 7.5 MHz
 */

#include "gui_antialias.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Compute biquad coefficients for one section of a Butterworth filter
// Q is the quality factor for this section
static void compute_biquad_lowpass(biquad_coeffs_t *coeffs, float omega, float Q) {
    float omega2 = omega * omega;
    float alpha = omega / Q;

    float norm = 1.0f / (1.0f + alpha + omega2);

    coeffs->b0 = omega2 * norm;
    coeffs->b1 = 2.0f * omega2 * norm;
    coeffs->b2 = omega2 * norm;
    coeffs->a1 = 2.0f * (omega2 - 1.0f) * norm;
    coeffs->a2 = (1.0f - alpha + omega2) * norm;
}

// Initialize 4th order Butterworth lowpass filter coefficients
// Implemented as cascade of two 2nd order sections (biquads)
void antialias_init_coeffs(antialias_coeffs_t *coeffs, float sample_rate, float cutoff_ratio) {
    // Cutoff frequency as fraction of sample rate
    // cutoff_ratio is fraction of Nyquist, so actual normalized freq = cutoff_ratio * 0.5
    float fc_normalized = cutoff_ratio * 0.5f;  // 0 to 0.5 range

    // Pre-warp: convert digital frequency to analog frequency
    float omega = tanf((float)M_PI * fc_normalized);

    // 4th order Butterworth has poles at angles:
    // pi/8, 3*pi/8, 5*pi/8, 7*pi/8 (from positive real axis)
    // This gives two conjugate pairs with Q factors:
    // Q1 = 1 / (2 * cos(3*pi/8)) = 0.5412
    // Q2 = 1 / (2 * cos(pi/8))   = 1.3066

    float Q1 = 0.54119610f;  // 1 / (2 * cos(67.5 deg))
    float Q2 = 1.30656296f;  // 1 / (2 * cos(22.5 deg))

    compute_biquad_lowpass(&coeffs->stage1, omega, Q1);
    compute_biquad_lowpass(&coeffs->stage2, omega, Q2);

    // Debug output
    fprintf(stderr, "[ANTIALIAS] 4th order Butterworth lowpass filter\n");
    fprintf(stderr, "[ANTIALIAS] Cutoff: %.1f%% of Nyquist (%.3f MHz)\n",
            cutoff_ratio * 100.0f, sample_rate * fc_normalized / 1e6f);
    fprintf(stderr, "[ANTIALIAS] Stage1 (Q=%.3f): b=[%.6f, %.6f, %.6f] a=[1, %.6f, %.6f]\n",
            Q1, coeffs->stage1.b0, coeffs->stage1.b1, coeffs->stage1.b2,
            coeffs->stage1.a1, coeffs->stage1.a2);
    fprintf(stderr, "[ANTIALIAS] Stage2 (Q=%.3f): b=[%.6f, %.6f, %.6f] a=[1, %.6f, %.6f]\n",
            Q2, coeffs->stage2.b0, coeffs->stage2.b1, coeffs->stage2.b2,
            coeffs->stage2.a1, coeffs->stage2.a2);
}

static void biquad_reset(biquad_state_t *state) {
    state->x1 = 0.0f;
    state->x2 = 0.0f;
    state->y1 = 0.0f;
    state->y2 = 0.0f;
}

void antialias_reset(antialias_state_t *state) {
    biquad_reset(&state->stage1);
    biquad_reset(&state->stage2);
}

// Process a single biquad section
static inline float biquad_process_sample(const biquad_coeffs_t *c, biquad_state_t *s, float x0) {
    float y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1;
    s->x1 = x0;
    s->y2 = s->y1;
    s->y1 = y0;
    return y0;
}

// Scalar implementation of 4th order filter (two cascaded biquads)
void antialias_process_buffer(const antialias_coeffs_t *coeffs,
                              antialias_state_t *state,
                              int16_t *samples,
                              size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        float x = (float)samples[i];

        // First biquad
        float y1 = biquad_process_sample(&coeffs->stage1, &state->stage1, x);

        // Second biquad
        float y2 = biquad_process_sample(&coeffs->stage2, &state->stage2, y1);

        // Clamp to int16 range and store with rounding
        if (y2 > 32767.0f) y2 = 32767.0f;
        if (y2 < -32768.0f) y2 = -32768.0f;
        samples[i] = (int16_t)(y2 >= 0 ? y2 + 0.5f : y2 - 0.5f);
    }
}

// Fast version - same as scalar for now since IIR has feedback dependencies
void antialias_process_buffer_fast(const antialias_coeffs_t *coeffs,
                                   antialias_state_t *state,
                                   int16_t *samples,
                                   size_t num_samples) {
    antialias_process_buffer(coeffs, state, samples, num_samples);
}
