/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#include "gui_extract.h"
#include "gui_app.h"
#include "gui_oscilloscope.h"
#include "../misrc_tools/extract.h"
#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/threading.h"
#include "../misrc_common/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Buffer sizes
#define BUFFER_READ_SIZE 65536
#define BUFFER_RECORD_SIZE (65536 * 1024)  // 64MB per channel

// Extraction buffers (page-aligned for SSE/AVX)
static int16_t *s_buf_a = NULL;
static int16_t *s_buf_b = NULL;
static uint8_t *s_buf_aux = NULL;
static conv_function_t s_extract_fn = NULL;
static bool s_initialized = false;

// Recording ringbuffers (extracted samples -> file writers)
static ringbuffer_t s_record_rb_a;
static ringbuffer_t s_record_rb_b;
static bool s_record_rb_initialized = false;

// Extraction thread state
static thrd_t s_extract_thread;
static bool s_extract_thread_running = false;
static ringbuffer_t *s_capture_rb = NULL;
static gui_app_t *s_extract_app = NULL;

// Recording state (atomic for thread-safe access)
static atomic_bool s_recording_enabled = false;
static atomic_bool s_use_flac = false;

// Extraction thread - runs continuously from capture start to stop
// Always updates display/stats, conditionally writes to record ringbuffers
static int extraction_thread(void *ctx) {
    (void)ctx;
    size_t read_size = BUFFER_READ_SIZE * 4;  // 4 bytes per sample pair
    size_t clip[2] = {0, 0};
    uint16_t peak[2] = {0, 0};

    fprintf(stderr, "[EXTRACT] Continuous extraction thread started\n");

    while (1) {
        // Check for exit
        if (atomic_load(&do_exit)) {
            break;
        }

        // Try to read from capture ringbuffer
        void *buf = rb_read_ptr(s_capture_rb, read_size);
        if (!buf) {
            // No data available yet - check if we should exit
            if (!s_extract_app || !s_extract_app->is_capturing) {
                break;
            }
            thrd_sleep_ms(1);
            continue;
        }

        // Extract samples
        s_extract_fn((uint32_t*)buf, BUFFER_READ_SIZE, clip, s_buf_aux, s_buf_a, s_buf_b, peak);

        // Mark capture buffer as consumed
        rb_read_finished(s_capture_rb, read_size);

        // Always update stats and display
        gui_extract_update_stats(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);
        gui_oscilloscope_update_display(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->total_samples, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_a, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_b, BUFFER_READ_SIZE);

        // Conditionally write to record ringbuffers
        if (atomic_load(&s_recording_enabled)) {
            if (atomic_load(&s_use_flac)) {
                // FLAC needs 32-bit samples with 12-bit to 16-bit extension
                size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int32_t);

                // Wait for space in record ringbuffers - never drop recording data
                int32_t *write_a;
                int32_t *write_b;
                while ((write_a = (int32_t *)rb_write_ptr(&s_record_rb_a, sample_bytes)) == NULL ||
                       (write_b = (int32_t *)rb_write_ptr(&s_record_rb_b, sample_bytes)) == NULL) {
                    if (atomic_load(&do_exit)) {
                        goto exit_thread;
                    }
                    thrd_sleep_ms(1);
                }

                // Convert 12-bit samples to 16-bit by left-shifting 4 bits
                for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                    write_a[i] = (int32_t)s_buf_a[i] << 4;
                    write_b[i] = (int32_t)s_buf_b[i] << 4;
                }
                rb_write_finished(&s_record_rb_a, sample_bytes);
                rb_write_finished(&s_record_rb_b, sample_bytes);
            } else {
                // RAW uses 16-bit samples directly
                size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int16_t);

                // Wait for space in record ringbuffers - never drop recording data
                void *write_a;
                void *write_b;
                while ((write_a = rb_write_ptr(&s_record_rb_a, sample_bytes)) == NULL ||
                       (write_b = rb_write_ptr(&s_record_rb_b, sample_bytes)) == NULL) {
                    if (atomic_load(&do_exit)) {
                        goto exit_thread;
                    }
                    thrd_sleep_ms(1);
                }

                memcpy(write_a, s_buf_a, sample_bytes);
                memcpy(write_b, s_buf_b, sample_bytes);
                rb_write_finished(&s_record_rb_a, sample_bytes);
                rb_write_finished(&s_record_rb_b, sample_bytes);
            }
        }
    }

exit_thread:
    fprintf(stderr, "[EXTRACT] Continuous extraction thread exiting\n");
    return 0;
}

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
    // Stop extraction thread if running
    gui_extract_stop();

    // Close record ringbuffers
    if (s_record_rb_initialized) {
        rb_close(&s_record_rb_a);
        rb_close(&s_record_rb_b);
        s_record_rb_initialized = false;
    }

    // Free extraction buffers
    if (s_initialized) {
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
}

int gui_extract_start(gui_app_t *app, ringbuffer_t *capture_rb) {
    if (s_extract_thread_running) {
        return 0;  // Already running
    }

    // Initialize extraction if needed
    gui_extract_init();

    // Initialize record ringbuffers if needed
    if (!s_record_rb_initialized) {
        rb_init(&s_record_rb_a, "record_a_rb", BUFFER_RECORD_SIZE);
        rb_init(&s_record_rb_b, "record_b_rb", BUFFER_RECORD_SIZE);
        s_record_rb_initialized = true;
    }

    // Store context
    s_capture_rb = capture_rb;
    s_extract_app = app;
    atomic_store(&s_recording_enabled, false);
    atomic_store(&s_use_flac, false);

    // Start extraction thread
    if (thrd_create(&s_extract_thread, extraction_thread, NULL) != thrd_success) {
        fprintf(stderr, "[EXTRACT] Failed to create extraction thread\n");
        return -1;
    }

    s_extract_thread_running = true;
    fprintf(stderr, "[EXTRACT] Started continuous extraction thread\n");
    return 0;
}

void gui_extract_stop(void) {
    if (!s_extract_thread_running) {
        return;
    }

    // Disable recording first
    atomic_store(&s_recording_enabled, false);

    // Wait for thread to exit
    thrd_join(s_extract_thread, NULL);
    s_extract_thread_running = false;
    s_extract_app = NULL;
    s_capture_rb = NULL;

    fprintf(stderr, "[EXTRACT] Stopped continuous extraction thread\n");
}

bool gui_extract_is_running(void) {
    return s_extract_thread_running;
}

ringbuffer_t *gui_extract_get_record_rb_a(void) {
    return &s_record_rb_a;
}

ringbuffer_t *gui_extract_get_record_rb_b(void) {
    return &s_record_rb_b;
}

void gui_extract_set_recording(bool enabled, bool use_flac) {
    atomic_store(&s_use_flac, use_flac);
    atomic_store(&s_recording_enabled, enabled);
    fprintf(stderr, "[EXTRACT] Recording %s (FLAC: %s)\n",
            enabled ? "enabled" : "disabled",
            use_flac ? "yes" : "no");
}

void gui_extract_reset_record_rbs(void) {
    if (s_record_rb_initialized) {
        atomic_store(&s_record_rb_a.head, 0);
        atomic_store(&s_record_rb_a.tail, 0);
        atomic_store(&s_record_rb_b.head, 0);
        atomic_store(&s_record_rb_b.tail, 0);
    }
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
