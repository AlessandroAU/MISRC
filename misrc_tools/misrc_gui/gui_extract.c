/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Shared functions for extracting samples and updating display buffers.
 */

#include "gui_extract.h"
#include "gui_app.h"
#include "../extract.h"

#include <stdlib.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(align, size) _aligned_malloc(size, align)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

// Buffer size for extraction
#define BUFFER_READ_SIZE 65536

// Extraction buffers (page-aligned for SSE/AVX)
static int16_t *s_buf_a = NULL;
static int16_t *s_buf_b = NULL;
static uint8_t *s_buf_aux = NULL;
static conv_function_t s_extract_fn = NULL;
static bool s_initialized = false;

void gui_extract_init(void) {
    if (s_initialized) return;

    // Allocate 32-byte aligned buffers for SSE/AVX
    s_buf_a = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_b = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_aux = aligned_alloc(16, BUFFER_READ_SIZE);

    // Get extraction function (AB mode)
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);

    s_initialized = true;
}

void gui_extract_cleanup(void) {
    if (!s_initialized) return;

    if (s_buf_a) {
        aligned_free(s_buf_a);
        s_buf_a = NULL;
    }
    if (s_buf_b) {
        aligned_free(s_buf_b);
        s_buf_b = NULL;
    }
    if (s_buf_aux) {
        aligned_free(s_buf_aux);
        s_buf_aux = NULL;
    }

    s_initialized = false;
}

extract_fn_t gui_extract_get_function(void) {
    if (!s_initialized) gui_extract_init();
    return (extract_fn_t)s_extract_fn;
}

int16_t *gui_extract_get_buf_a(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_a;
}

int16_t *gui_extract_get_buf_b(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_b;
}

uint8_t *gui_extract_get_buf_aux(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_aux;
}

void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples) {
    size_t clip_a_pos = 0, clip_a_neg = 0;
    size_t clip_b_pos = 0, clip_b_neg = 0;
    uint16_t peak_a_pos = 0, peak_a_neg = 0;
    uint16_t peak_b_pos = 0, peak_b_neg = 0;

    // Peak detection uses first 1000 samples
    size_t peak_samples = (num_samples < 1000) ? num_samples : 1000;

    for (size_t i = 0; i < num_samples; i++) {
        int16_t sa = buf_a[i];
        int16_t sb = buf_b[i];

        // Clipping detection (12-bit ADC: +2047 is positive clip, -2048 is negative clip)
        if (sa >= 2047) clip_a_pos++;
        else if (sa <= -2048) clip_a_neg++;
        if (sb >= 2047) clip_b_pos++;
        else if (sb <= -2048) clip_b_neg++;

        // Peak detection (first N samples only)
        if (i < peak_samples) {
            if (sa > 0 && (uint16_t)sa > peak_a_pos) peak_a_pos = (uint16_t)sa;
            if (sb > 0 && (uint16_t)sb > peak_b_pos) peak_b_pos = (uint16_t)sb;
            if (sa < 0 && (uint16_t)(-sa) > peak_a_neg) peak_a_neg = (uint16_t)(-sa);
            if (sb < 0 && (uint16_t)(-sb) > peak_b_neg) peak_b_neg = (uint16_t)(-sb);
        }
    }

    // Update atomic counters
    atomic_fetch_add(&app->clip_count_a_pos, clip_a_pos);
    atomic_fetch_add(&app->clip_count_a_neg, clip_a_neg);
    atomic_fetch_add(&app->clip_count_b_pos, clip_b_pos);
    atomic_fetch_add(&app->clip_count_b_neg, clip_b_neg);
    atomic_store(&app->peak_a_pos, peak_a_pos);
    atomic_store(&app->peak_a_neg, peak_a_neg);
    atomic_store(&app->peak_b_pos, peak_b_pos);
    atomic_store(&app->peak_b_neg, peak_b_neg);
}

void gui_extract_update_display(gui_app_t *app, const int16_t *buf_a,
                                const int16_t *buf_b, size_t num_samples) {
    const float scale = 1.0f / 2048.0f;
    const size_t target_samples = DISPLAY_BUFFER_SIZE;
    const size_t decimation = (num_samples + target_samples - 1) / target_samples;
    size_t display_samples = num_samples / decimation;

    if (display_samples > target_samples) {
        display_samples = target_samples;
    }

    for (size_t i = 0; i < display_samples; i++) {
        size_t start = i * decimation;
        size_t end = start + decimation;
        if (end > num_samples) end = num_samples;

        // Find min/max within this decimation window
        int16_t min_a = buf_a[start], max_a = buf_a[start];
        int16_t min_b = buf_b[start], max_b = buf_b[start];

        for (size_t j = start + 1; j < end; j++) {
            if (buf_a[j] < min_a) min_a = buf_a[j];
            if (buf_a[j] > max_a) max_a = buf_a[j];
            if (buf_b[j] < min_b) min_b = buf_b[j];
            if (buf_b[j] > max_b) max_b = buf_b[j];
        }

        app->display_samples[i].min_a = (float)min_a * scale;
        app->display_samples[i].max_a = (float)max_a * scale;
        app->display_samples[i].min_b = (float)min_b * scale;
        app->display_samples[i].max_b = (float)max_b * scale;
    }

    app->display_samples_available = display_samples;
}
