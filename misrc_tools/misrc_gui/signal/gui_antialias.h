/*
 * MISRC GUI - Anti-Aliasing Filter
 *
 * 4th order Butterworth lowpass filter (cascade of two biquads).
 * Applied to raw ADC samples before recording to prevent aliasing
 * when the signal is later processed at lower sample rates.
 *
 * Build option: -DENABLE_ANTIALIASING=1
 */

#ifndef GUI_ANTIALIAS_H
#define GUI_ANTIALIAS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Single biquad state (2nd order section)
typedef struct {
    float x1, x2;  // Input delay line
    float y1, y2;  // Output delay line
} biquad_state_t;

// Filter state for one channel (4th order = 2 cascaded biquads)
typedef struct {
    biquad_state_t stage1;
    biquad_state_t stage2;
} antialias_state_t;

// Single biquad coefficients
typedef struct {
    float b0, b1, b2;  // Feedforward (numerator) coefficients
    float a1, a2;      // Feedback (denominator) coefficients, a0 normalized to 1
} biquad_coeffs_t;

// Filter coefficients (4th order = 2 cascaded biquads)
typedef struct {
    biquad_coeffs_t stage1;
    biquad_coeffs_t stage2;
} antialias_coeffs_t;

// Initialize filter coefficients for given sample rate
// cutoff_ratio: fraction of Nyquist frequency (0.75 = 75% of Nyquist)
void antialias_init_coeffs(antialias_coeffs_t *coeffs, float sample_rate, float cutoff_ratio);

// Reset filter state (call when starting a new capture)
void antialias_reset(antialias_state_t *state);

// Process a buffer of int16_t samples in-place
// Uses the provided coefficients and updates the filter state
void antialias_process_buffer(const antialias_coeffs_t *coeffs,
                              antialias_state_t *state,
                              int16_t *samples,
                              size_t num_samples);

// Process a buffer with SIMD optimization (if available)
// Falls back to scalar if SIMD not supported
void antialias_process_buffer_fast(const antialias_coeffs_t *coeffs,
                                   antialias_state_t *state,
                                   int16_t *samples,
                                   size_t num_samples);

#ifdef __cplusplus
}
#endif

#endif // GUI_ANTIALIAS_H
