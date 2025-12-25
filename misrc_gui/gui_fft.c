/*
 * MISRC GUI - FFT Line Spectrum Display with GPU Phosphor Persistence
 *
 * Computes FFT of resampled display waveform and displays as a line-based spectrum
 * using GPU-accelerated phosphor persistence (shared module).
 *
 * Copyright (C) 2024-2025 vrunk11, stefan_o
 * Licensed under GNU GPL v3 or later
 */

#include "gui_fft.h"
#include "gui_app.h"
#include "gui_phosphor_rt.h"
#include "gui_ui.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if LIBFFTW_ENABLED
#include <fftw3.h>
#endif

//-----------------------------------------------------------------------------
// Availability Check
//-----------------------------------------------------------------------------

bool gui_fft_available(void) {
#if LIBFFTW_ENABLED
    return true;
#else
    return false;
#endif
}

//-----------------------------------------------------------------------------
// FFT Lifecycle Management
//-----------------------------------------------------------------------------

bool gui_fft_init(fft_state_t *state) {
    if (!state) return false;

    // Clear state first
    memset(state, 0, sizeof(fft_state_t));

#if LIBFFTW_ENABLED
    // Allocate FFTW input buffer (real samples)
    state->fftw_input = (float *)fftwf_malloc(sizeof(float) * FFT_SIZE);
    if (!state->fftw_input) {
        fprintf(stderr, "[FFT] Failed to allocate FFTW input buffer\n");
        return false;
    }

    // Allocate FFTW output buffer (complex)
    state->fftw_output = fftwf_malloc(sizeof(fftwf_complex) * FFT_BINS);
    if (!state->fftw_output) {
        fprintf(stderr, "[FFT] Failed to allocate FFTW output buffer\n");
        fftwf_free(state->fftw_input);
        state->fftw_input = NULL;
        return false;
    }

    // Create FFTW plan (real-to-complex, measure for best performance)
    state->fftw_plan = fftwf_plan_dft_r2c_1d(FFT_SIZE, state->fftw_input,
                                              (fftwf_complex *)state->fftw_output,
                                              FFTW_MEASURE);
    if (!state->fftw_plan) {
        fprintf(stderr, "[FFT] Failed to create FFTW plan\n");
        fftwf_free(state->fftw_input);
        fftwf_free(state->fftw_output);
        state->fftw_input = NULL;
        state->fftw_output = NULL;
        return false;
    }

    // Allocate Hanning window
    state->window = (float *)malloc(sizeof(float) * FFT_SIZE);
    if (!state->window) {
        fprintf(stderr, "[FFT] Failed to allocate window buffer\n");
        gui_fft_cleanup(state);
        return false;
    }

    // Precompute Hanning window coefficients
    for (int i = 0; i < FFT_SIZE; i++) {
        state->window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
    }

    // Allocate magnitude output buffer (normalized 0-1)
    state->magnitude = (float *)malloc(sizeof(float) * FFT_BINS);
    if (!state->magnitude) {
        fprintf(stderr, "[FFT] Failed to allocate magnitude buffer\n");
        gui_fft_cleanup(state);
        return false;
    }
    memset(state->magnitude, 0, sizeof(float) * FFT_BINS);

    // Phosphor render textures will be created on first render (need OpenGL context)
    memset(&state->phosphor, 0, sizeof(phosphor_rt_t));

    // Set FFT-specific phosphor config
    state->phosphor.config.decay_rate = FFT_DECAY_RATE;
    state->phosphor.config.hit_increment = FFT_HIT_INCREMENT;
    state->phosphor.config.bloom_intensity = FFT_BLOOM;

    state->data_ready = false;
    state->initialized = true;

    fprintf(stderr, "[FFT] Initialized: FFT_SIZE=%d, FFT_BINS=%d (display sample based)\n",
            FFT_SIZE, FFT_BINS);

    return true;
#else
    fprintf(stderr, "[FFT] FFTW not available, FFT support disabled\n");
    return false;
#endif
}

void gui_fft_clear(fft_state_t *state) {
    if (!state || !state->initialized) return;

#if LIBFFTW_ENABLED
    // Clear magnitude buffer
    if (state->magnitude) {
        memset(state->magnitude, 0, sizeof(float) * FFT_BINS);
    }

    // Clear phosphor render textures
    phosphor_rt_clear(&state->phosphor);

    state->data_ready = false;
#endif
}

void gui_fft_cleanup(fft_state_t *state) {
    if (!state) return;

#if LIBFFTW_ENABLED
    if (state->fftw_plan) {
        fftwf_destroy_plan((fftwf_plan)state->fftw_plan);
        state->fftw_plan = NULL;
    }

    if (state->fftw_input) {
        fftwf_free(state->fftw_input);
        state->fftw_input = NULL;
    }

    if (state->fftw_output) {
        fftwf_free(state->fftw_output);
        state->fftw_output = NULL;
    }

    if (state->window) {
        free(state->window);
        state->window = NULL;
    }

    if (state->magnitude) {
        free(state->magnitude);
        state->magnitude = NULL;
    }

    phosphor_rt_cleanup(&state->phosphor);
#endif

    state->initialized = false;
}

//-----------------------------------------------------------------------------
// FFT Processing - Compute FFT from display samples
//-----------------------------------------------------------------------------

void gui_fft_process_display(fft_state_t *state, const waveform_sample_t *samples,
                             size_t count, float display_sample_rate) {
#if LIBFFTW_ENABLED
    if (!state || !state->initialized || !samples) return;

    (void)display_sample_rate; // Used only for frequency axis in render

    // Need at least FFT_SIZE samples
    if (count < FFT_SIZE) return;

    // Use the most recent FFT_SIZE samples
    size_t start = (count > FFT_SIZE) ? (count - FFT_SIZE) : 0;

    // Calculate DC offset (mean of samples) to remove from signal
    float dc_offset = 0.0f;
    for (int i = 0; i < FFT_SIZE; i++) {
        dc_offset += samples[start + i].value;
    }
    dc_offset /= FFT_SIZE;

    // Copy samples to FFT input with DC removal and Hanning window
    for (int i = 0; i < FFT_SIZE; i++) {
        state->fftw_input[i] = (samples[start + i].value - dc_offset) * state->window[i];
    }

    // Execute FFT
    fftwf_execute((fftwf_plan)state->fftw_plan);

    // Compute magnitude in dB, then normalize to 0-1, then apply EMA smoothing
    fftwf_complex *output = (fftwf_complex *)state->fftw_output;
    for (int j = 0; j < FFT_BINS; j++) {
        float real = output[j][0];
        float imag = output[j][1];
        float mag = sqrtf(real * real + imag * imag);

        // Normalize by FFT size
        mag /= FFT_SIZE;

        // Convert to dB (with small epsilon to avoid log(0))
        float db = 20.0f * log10f(mag + 1e-10f);

        // Clamp to dB range
        if (db < FFT_DB_MIN) db = FFT_DB_MIN;
        if (db > FFT_DB_MAX) db = FFT_DB_MAX;

        // Convert dB to normalized intensity (0-1)
        float current = (db - FFT_DB_MIN) / (FFT_DB_MAX - FFT_DB_MIN);

        // Apply EMA smoothing: smoothed = alpha * previous + (1 - alpha) * current
        float previous = state->magnitude[j];
        state->magnitude[j] = FFT_EMA_ALPHA * previous + (1.0f - FFT_EMA_ALPHA) * current;
    }

    state->data_ready = true;
#else
    (void)state;
    (void)samples;
    (void)count;
    (void)display_sample_rate;
#endif
}

//-----------------------------------------------------------------------------
// FFT Rendering - GPU phosphor persistence using shared module
//-----------------------------------------------------------------------------

// Grid settings for FFT (matching oscilloscope style)
#define FFT_GRID_MIN_SPACING_PX 80   // Minimum pixels between frequency grid lines
#define FFT_GRID_MAX_DIVISIONS 12    // Maximum number of frequency divisions

// Snap to 1-2-5 log scale sequence (same as oscilloscope)
static double fft_snap_to_125(double value) {
    if (value <= 0) return 1.0;

    double log_val = log10(value);
    double magnitude = pow(10.0, floor(log_val));
    double normalized = value / magnitude;

    double snapped;
    if (normalized < 1.5) {
        snapped = 1.0;
    } else if (normalized < 3.5) {
        snapped = 2.0;
    } else if (normalized < 7.5) {
        snapped = 5.0;
    } else {
        snapped = 10.0;
    }

    return snapped * magnitude;
}

// Format frequency value with appropriate unit (Hz, kHz, MHz)
static void format_freq_label(char *buf, size_t buf_size, double hz) {
    if (hz >= 1000000.0) {
        snprintf(buf, buf_size, "%.3gMHz", hz / 1000000.0);
    } else if (hz >= 1000.0) {
        snprintf(buf, buf_size, "%.3gkHz", hz / 1000.0);
    } else {
        snprintf(buf, buf_size, "%.3gHz", hz);
    }
}

// Helper to draw text with font
static void fft_draw_text(Font *fonts, const char *text, float px, float py, int fontSize, Color color) {
    if (fonts) {
        DrawTextEx(fonts[0], text, (Vector2){px, py}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)px, (int)py, fontSize, color);
    }
}

// Helper to draw text with monospace font (for numbers)
static void fft_draw_text_mono(Font *fonts, const char *text, float px, float py, int fontSize, Color color) {
    if (fonts) {
        DrawTextEx(fonts[1], text, (Vector2){px, py}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)px, (int)py, fontSize, color);
    }
}

// Helper to measure text with font
static int fft_measure_text(Font *fonts, const char *text, int fontSize) {
    if (fonts) {
        Vector2 size = MeasureTextEx(fonts[0], text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return MeasureText(text, fontSize);
}

void gui_fft_render(fft_state_t *state, float x, float y,
                    float width, float height, float display_sample_rate,
                    Color color, Font *fonts) {
#if LIBFFTW_ENABLED
    if (!state || !state->initialized) return;

    (void)color; // Not used currently

    int rt_width = (int)width;
    int rt_height = (int)height;

    // Ensure we have phosphor render textures of the right size
    if (!phosphor_rt_init(&state->phosphor, rt_width, rt_height)) {
        // Fallback: just draw background
        DrawRectangle((int)x, (int)y, rt_width, rt_height, COLOR_METER_BG);
        fft_draw_text(fonts, "FFT: GPU init failed", x + 10, y + 10, FONT_SIZE_OSC_SCALE, (Color){255, 80, 80, 255});
        return;
    }

    // Begin phosphor frame (applies decay and prepares for drawing)
    phosphor_rt_begin_frame(&state->phosphor);

    // Draw FFT bins as connected line segments
    if (state->magnitude && state->data_ready) {
        Color lineColor = phosphor_rt_get_draw_color(&state->phosphor);

        // Map FFT bins to render texture width
        float bin_width = (float)rt_width / (float)(FFT_BINS - 1);

        for (int bin = 0; bin < FFT_BINS - 1; bin++) {
            float intensity1 = state->magnitude[bin];
            float intensity2 = state->magnitude[bin + 1];

            // X positions for this segment
            float x1 = bin * bin_width;
            float x2 = (bin + 1) * bin_width;

            // Y positions based on intensity (0 = bottom, 1 = top)
            float y1 = rt_height - (intensity1 * rt_height);
            float y2 = rt_height - (intensity2 * rt_height);

            // Draw line segment
            DrawLineEx((Vector2){x1, y1}, (Vector2){x2, y2}, 1.5f, lineColor);
        }
    }

    // End phosphor frame (finalize accumulation buffer)
    phosphor_rt_end_frame(&state->phosphor);

    // Draw background (same as oscilloscope)
    DrawRectangle((int)x, (int)y, rt_width, rt_height, COLOR_METER_BG);

    // Draw horizontal grid lines (dB levels) with 1-2-5 snapping
    float db_range = FFT_DB_MAX - FFT_DB_MIN;  // 80 dB range
    float rough_db_division = db_range * (float)FFT_GRID_MIN_SPACING_PX / height;
    float db_division = (float)fft_snap_to_125((double)rough_db_division);

    // Find first grid line position
    float first_db = ceilf(FFT_DB_MIN / db_division) * db_division;
    int div_count = 0;

    for (float db = first_db; db <= FFT_DB_MAX && div_count < FFT_GRID_MAX_DIVISIONS; db += db_division) {
        float normalized = (db - FFT_DB_MIN) / db_range;
        float line_y = y + height - (normalized * height);

        // Draw grid line (use major color for 0 dB)
        bool is_zero = (fabsf(db) < 0.001f);
        DrawLineV((Vector2){x, line_y}, (Vector2){x + width, line_y},
                  is_zero ? COLOR_GRID_MAJOR : COLOR_GRID);

        // Draw dB label on left side
        char label[16];
        snprintf(label, sizeof(label), "%+.0fdB", db);
        fft_draw_text_mono(fonts, label, x + 4, line_y - 6, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);

        div_count++;
    }

    // Draw vertical frequency grid lines with 1-2-5 snapping (matching oscilloscope time grid logic)
    if (display_sample_rate > 0) {
        float nyquist = display_sample_rate / 2.0f;

        // Calculate frequency per pixel
        float freq_per_pixel = nyquist / width;

        // Calculate rough frequency division to get reasonable spacing
        float rough_division = freq_per_pixel * (float)FFT_GRID_MIN_SPACING_PX;

        // Snap to 1-2-5 sequence
        float freq_division = (float)fft_snap_to_125((double)rough_division);

        // Calculate pixels per division
        float pixels_per_div = freq_division / freq_per_pixel;

        // Draw vertical grid lines at frequency intervals
        char freq_buf[32];
        div_count = 0;
        for (float freq = freq_division; freq < nyquist && div_count < FFT_GRID_MAX_DIVISIONS; freq += freq_division) {
            float normalized = freq / nyquist;
            float line_x = x + (normalized * width);

            DrawLineV((Vector2){line_x, y}, (Vector2){line_x, y + height}, COLOR_GRID);

            // Draw frequency label (skip if too close to edges)
            if (line_x > x + 30 && line_x < x + width - 30) {
                format_freq_label(freq_buf, sizeof(freq_buf), freq);
                int label_w = fft_measure_text(fonts, freq_buf, FONT_SIZE_OSC_SCALE);
                fft_draw_text_mono(fonts, freq_buf, line_x - label_w / 2, y + height - 14, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
            }
            div_count++;
        }

        // Show frequency per division in bottom-left corner (matching oscilloscope style)
        format_freq_label(freq_buf, sizeof(freq_buf), freq_division);
        char div_label[48];
        snprintf(div_label, sizeof(div_label), "%s/div", freq_buf);
        fft_draw_text(fonts, div_label, x + 6, y + height - 18, 18, COLOR_TEXT);

        (void)pixels_per_div; // Suppress unused warning
    }

    // Draw center line (0V equivalent position not applicable for FFT, skip)

    // Border (same as oscilloscope)
    DrawRectangleLinesEx((Rectangle){x, y, width, height}, 1, COLOR_GRID_MAJOR);

    // Render phosphor texture with heatmap colormap and bloom
    phosphor_rt_render(&state->phosphor, x, y, true);

    // Draw "FFT" label in top-right corner (matching oscilloscope channel label style)
    const char *fft_label = "FFT";
    int label_width = fft_measure_text(fonts, fft_label, FONT_SIZE_OSC_LABEL);
    fft_draw_text(fonts, fft_label, x + width - label_width - 8, y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);

#else
    (void)state;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)display_sample_rate;
    (void)color;
    (void)fonts;
#endif
}
