/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#include "gui_record.h"
#include "gui_app.h"
#include "gui_extract.h"

#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/flac_writer.h"
#include "../misrc_common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

// Buffer sizes
#define BUFFER_READ_SIZE 65536

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Writer threads
static thrd_t s_writer_thread_a;
static thrd_t s_writer_thread_b;
static bool s_writer_threads_running = false;
static FILE *s_file_a = NULL;
static FILE *s_file_b = NULL;

// Global app pointer for threads
static gui_app_t *s_recording_app = NULL;

// File writer context
typedef struct {
    ringbuffer_t *rb;
    FILE *file;
    int channel;  // 0 = A, 1 = B
#if LIBFLAC_ENABLED == 1
    flac_writer_t *writer;
    atomic_uint_fast64_t *compressed_bytes;
#endif
    gui_app_t *app;  // For error reporting
} writer_ctx_t;

static writer_ctx_t s_ctx_a;
static writer_ctx_t s_ctx_b;

#if LIBFLAC_ENABLED == 1
// FLAC writers (managed by shared library)
static flac_writer_t *s_flac_writer_a = NULL;
static flac_writer_t *s_flac_writer_b = NULL;

// Error callback for GUI FLAC writer
static void gui_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
    (void)error;
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->app) {
        gui_app_set_status(wctx->app, message);
    }
    fprintf(stderr, "FLAC ERROR: %s\n", message);
}

// Bytes written callback for compression ratio tracking
static void gui_flac_bytes_callback(void *user_data, size_t bytes_written) {
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->compressed_bytes) {
        atomic_fetch_add(wctx->compressed_bytes, bytes_written);
    }
}

// FLAC file writer thread
static int flac_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = BUFFER_READ_SIZE * sizeof(int32_t);
    size_t raw_bytes_per_block = BUFFER_READ_SIZE * sizeof(int16_t);
    void *buf;

    fprintf(stderr, "[FLAC] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (1) {
        buf = rb_read_ptr(wctx->rb, len);
        if (!buf) {
            // No data available - check if we should exit
            if (atomic_load(&do_exit) || !s_recording_app || !s_recording_app->is_recording) {
                // Drain any remaining partial data before exiting
                size_t remaining = wctx->rb->tail - wctx->rb->head;
                if (remaining > 0 && remaining < len) {
                    size_t remaining_samples = remaining / sizeof(int32_t);
                    buf = rb_read_ptr(wctx->rb, remaining);
                    if (buf && remaining_samples > 0) {
                        flac_writer_process(wctx->writer, (const int32_t *)buf, remaining_samples);
                        rb_read_finished(wctx->rb, remaining);
                    }
                }
                break;
            }
            thrd_sleep_ms(1);
            continue;
        }

        int result = flac_writer_process(wctx->writer, (const int32_t *)buf, BUFFER_READ_SIZE);
        if (result < 0) {
            fprintf(stderr, "FLAC encoder error on channel %c\n", wctx->channel == 0 ? 'A' : 'B');
        }

        rb_read_finished(wctx->rb, len);

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, raw_bytes_per_block);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, raw_bytes_per_block);
            }
        }
    }

    fprintf(stderr, "[FLAC] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}
#endif

// RAW file writer thread
static int raw_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = BUFFER_READ_SIZE * sizeof(int16_t);

    fprintf(stderr, "[RAW] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (1) {
        void *buf = rb_read_ptr(wctx->rb, len);
        if (!buf) {
            // No data available - check if we should exit
            if (atomic_load(&do_exit) || !s_recording_app || !s_recording_app->is_recording) {
                // Drain any remaining partial data before exiting
                size_t remaining = wctx->rb->tail - wctx->rb->head;
                if (remaining > 0 && remaining < len) {
                    buf = rb_read_ptr(wctx->rb, remaining);
                    if (buf) {
                        fwrite(buf, 1, remaining, wctx->file);
                        rb_read_finished(wctx->rb, remaining);
                    }
                }
                break;
            }
            thrd_sleep_ms(1);
            continue;
        }

        size_t written = fwrite(buf, 1, len, wctx->file);
        rb_read_finished(wctx->rb, len);

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, written);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, written);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, written);
            }
        }
    }

    fprintf(stderr, "[RAW] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}

// Initialize recording subsystem
void gui_record_init(void) {
    // Nothing to initialize here anymore - ringbuffers are in gui_extract
}

// Cleanup recording subsystem
void gui_record_cleanup(void) {
    // Nothing to cleanup here anymore - ringbuffers are in gui_extract
}

// Check if recording is active
bool gui_record_is_active(void) {
    return s_recording_app != NULL && s_recording_app->is_recording;
}

// Start recording
int gui_record_start(gui_app_t *app) {

    if (!app->is_capturing) {
        gui_app_set_status(app, "Start capture first");
        return -1;
    }

    if (app->is_recording) {
        return 0;
    }

    // Verify extraction thread is running
    if (!gui_extract_is_running()) {
        gui_app_set_status(app, "Extraction not running");
        return -1;
    }

    // Get record ringbuffers from gui_extract
    ringbuffer_t *rb_a = gui_extract_get_record_rb_a();
    ringbuffer_t *rb_b = gui_extract_get_record_rb_b();

    if (!rb_a || !rb_b) {
        gui_app_set_status(app, "Record buffers not initialized");
        return -1;
    }

    s_recording_app = app;
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);

    // Reset record ringbuffers before starting
    gui_extract_reset_record_rbs();

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac) {
        // Open FLAC files
        s_file_a = fopen(app->settings.output_filename_a, "wb");
        s_file_b = fopen(app->settings.output_filename_b, "wb");

        if (!s_file_a || !s_file_b) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }

        // Setup writer contexts
        s_ctx_a.rb = rb_a;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.compressed_bytes = &app->recording_compressed_a;
        s_ctx_a.app = app;

        s_ctx_b.rb = rb_b;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;
        s_ctx_b.compressed_bytes = &app->recording_compressed_b;
        s_ctx_b.app = app;

        // Configure FLAC writers using shared library
        flac_writer_config_t config = flac_writer_default_config();
        config.sample_rate = 40000;
        config.bits_per_sample = 16;  // TODO: Make configurable for 12-bit support
        config.compression_level = app->settings.flac_level;
        config.verify = false;
        config.num_threads = 0;  // Auto-detect
        config.enable_seektable = true;

        // Create writer for channel A
        config.error_cb = gui_flac_error_callback;
        config.bytes_cb = gui_flac_bytes_callback;
        config.callback_user_data = &s_ctx_a;

        s_flac_writer_a = flac_writer_create_stream(s_file_a, &config);
        if (!s_flac_writer_a) {
            gui_app_set_status(app, "Failed to create FLAC encoder A");
            fclose(s_file_a); fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }
        s_ctx_a.writer = s_flac_writer_a;

        // Create writer for channel B
        config.callback_user_data = &s_ctx_b;

        s_flac_writer_b = flac_writer_create_stream(s_file_b, &config);
        if (!s_flac_writer_b) {
            gui_app_set_status(app, "Failed to create FLAC encoder B");
            flac_writer_abort(s_flac_writer_a);
            s_flac_writer_a = NULL;
            fclose(s_file_a); fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }
        s_ctx_b.writer = s_flac_writer_b;

        // Mark as recording and start writer threads
        app->is_recording = true;
        app->recording_start_time = GetTime();

        // Enable recording in extraction thread
        gui_extract_set_recording(true, true);

        thrd_create(&s_writer_thread_a, flac_writer_thread, &s_ctx_a);
        thrd_create(&s_writer_thread_b, flac_writer_thread, &s_ctx_b);
        s_writer_threads_running = true;

        gui_app_set_status(app, "Recording (FLAC)...");
    } else
#endif
    {
        // RAW recording
        s_file_a = fopen(app->settings.output_filename_a, "wb");
        s_file_b = fopen(app->settings.output_filename_b, "wb");

        if (!s_file_a || !s_file_b) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }

        s_ctx_a.rb = rb_a;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;

        s_ctx_b.rb = rb_b;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;

        // Mark as recording and start writer threads
        app->is_recording = true;
        app->recording_start_time = GetTime();

        // Enable recording in extraction thread
        gui_extract_set_recording(true, false);

        thrd_create(&s_writer_thread_a, raw_writer_thread, &s_ctx_a);
        thrd_create(&s_writer_thread_b, raw_writer_thread, &s_ctx_b);
        s_writer_threads_running = true;

        gui_app_set_status(app, "Recording (RAW)...");
    }

    return 0;
}

// Stop recording
void gui_record_stop(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }

    // Disable recording in extraction thread first
    // This stops new data from being written to record ringbuffers
    gui_extract_set_recording(false, false);

    // Signal threads to stop
    app->is_recording = false;

    // Wait for writer threads to drain and exit
    if (s_writer_threads_running) {
        thrd_join(s_writer_thread_a, NULL);
        thrd_join(s_writer_thread_b, NULL);
        s_writer_threads_running = false;
    }

#if LIBFLAC_ENABLED == 1
    // Finalize FLAC writers (this also cleans them up)
    if (s_flac_writer_a) {
        flac_writer_finish(s_flac_writer_a);
        s_flac_writer_a = NULL;
    }
    if (s_flac_writer_b) {
        flac_writer_finish(s_flac_writer_b);
        s_flac_writer_b = NULL;
    }
#endif

    // Close files
    if (s_file_a) {
        fclose(s_file_a);
        s_file_a = NULL;
    }
    if (s_file_b) {
        fclose(s_file_b);
        s_file_b = NULL;
    }

    s_recording_app = NULL;
    gui_app_set_status(app, "Recording stopped");
}
