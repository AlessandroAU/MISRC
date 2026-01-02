/*
 * MISRC GUI - Soundcard FLAC Writer
 *
 * Writes captured soundcard audio to FLAC file.
 * Reads from BUF_SOUNDCARD_AUDIO ringbuffer and encodes to FLAC.
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#include "gui_soundcard_writer.h"
#include "../core/gui_app.h"
#include "../input/gui_soundcard.h"
#include "../../common/buffer_manager.h"
#include "../../common/flac_writer.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// Soundcard audio parameters (must match gui_soundcard.c)
#define SOUNDCARD_SAMPLE_RATE     48000
#define SOUNDCARD_CHANNELS        2
#define SOUNDCARD_BITS_PER_SAMPLE 16

// Buffer read size (in bytes)
#define BUFFER_READ_SIZE (4096 * 4)  // 4K frames * 4 bytes/frame (stereo 16-bit)

// External do_exit flag
extern atomic_int do_exit;

// Writer thread state
static thrd_t s_writer_thread;
static bool s_writer_running = false;
static FILE *s_output_file = NULL;
static gui_app_t *s_writer_app = NULL;

#if LIBFLAC_ENABLED == 1
static flac_writer_t *s_flac_writer = NULL;

// Writer context for callbacks
typedef struct {
    gui_app_t *app;
    buffer_manager_t *bufmgr;
} soundcard_writer_ctx_t;

static soundcard_writer_ctx_t s_ctx;

// Error callback
static void soundcard_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
    (void)error;
    soundcard_writer_ctx_t *ctx = (soundcard_writer_ctx_t *)user_data;
    if (ctx && ctx->app) {
        gui_app_set_status(ctx->app, message);
    }
    fprintf(stderr, "[SOUNDCARD FLAC] ERROR: %s\n", message);
}

// Writer thread
static int soundcard_writer_thread(void *ctx) {
    soundcard_writer_ctx_t *wctx = (soundcard_writer_ctx_t *)ctx;
    size_t read_bytes = BUFFER_READ_SIZE;
    uint64_t total_bytes_written = 0;
    uint32_t debug_counter = 0;

    fprintf(stderr, "[SOUNDCARD FLAC] Writer thread started\n");

    // Boost thread priority
    thrd_set_priority(THRD_PRIORITY_ABOVE);

    while (1) {
        // Read from soundcard RECORD buffer (not the VU meter buffer)
        void *buf = bufmgr_read_begin(wctx->bufmgr, BUF_SOUNDCARD_RECORD, read_bytes, 10);
        if (!buf) {
            // Check if we should exit
            if (atomic_load(&do_exit) || !s_writer_app || !s_writer_app->is_recording) {
                fprintf(stderr, "[SOUNDCARD FLAC] Exiting: do_exit=%d, app=%p, recording=%d\n",
                        atomic_load(&do_exit), (void*)s_writer_app,
                        s_writer_app ? s_writer_app->is_recording : -1);
                // Drain remaining data
                size_t remaining = bufmgr_fill_level(wctx->bufmgr, BUF_SOUNDCARD_RECORD);
                fprintf(stderr, "[SOUNDCARD FLAC] Draining %zu remaining bytes\n", remaining);
                while (remaining > 0) {
                    size_t drain_len = (remaining < read_bytes) ? remaining : read_bytes;
                    buf = bufmgr_read_begin(wctx->bufmgr, BUF_SOUNDCARD_RECORD, drain_len, 0);
                    if (!buf) break;

                    // Process samples (interleaved stereo int16)
                    uint32_t num_samples = (uint32_t)(drain_len / sizeof(int16_t));
                    if (num_samples > 0 && s_flac_writer) {
                        flac_writer_process_int16(s_flac_writer, (const int16_t *)buf, num_samples);
                        total_bytes_written += drain_len;
                    }

                    bufmgr_read_end(wctx->bufmgr, BUF_SOUNDCARD_RECORD, drain_len);
                    remaining = bufmgr_fill_level(wctx->bufmgr, BUF_SOUNDCARD_RECORD);
                }
                break;
            }
            continue;
        }

        // Process samples (interleaved stereo int16)
        uint32_t num_samples = (uint32_t)(read_bytes / sizeof(int16_t));
        if (num_samples > 0 && s_flac_writer) {
            flac_writer_process_int16(s_flac_writer, (const int16_t *)buf, num_samples);
            total_bytes_written += read_bytes;
            debug_counter++;
            if (debug_counter <= 5 || (debug_counter % 500) == 0) {
                fprintf(stderr, "[SOUNDCARD FLAC] Wrote %zu bytes, total: %llu bytes\n",
                        read_bytes, (unsigned long long)total_bytes_written);
            }
        }

        bufmgr_read_end(wctx->bufmgr, BUF_SOUNDCARD_RECORD, read_bytes);
    }

    fprintf(stderr, "[SOUNDCARD FLAC] Writer thread exiting, total written: %llu bytes\n",
            (unsigned long long)total_bytes_written);
    return 0;
}

#endif // LIBFLAC_ENABLED

int gui_soundcard_writer_start(gui_app_t *app, buffer_manager_t *bufmgr) {
    if (!app || !bufmgr) return -1;
    if (s_writer_running) return 0;  // Already running

#if LIBFLAC_ENABLED == 1
    fprintf(stderr, "[SOUNDCARD FLAC] Starting writer\n");

    // Note: BUF_SOUNDCARD_RECORD is already initialized when soundcard capture starts
    // (in gui_soundcard_start). We just reset it here to clear any stale data.
    bufmgr_reset(bufmgr, BUF_SOUNDCARD_RECORD);

    // Build output path
    char filepath[512];
    const char *filename = app->settings.soundcard_filename;
    if (!filename || filename[0] == '\0') {
        filename = "linear_audio.flac";
    }
    snprintf(filepath, sizeof(filepath), "%s/%s", app->settings.output_path, filename);

    // Open output file
    s_output_file = fopen(filepath, "wb");
    if (!s_output_file) {
        fprintf(stderr, "[SOUNDCARD FLAC] Failed to open output file: %s\n", filepath);
        gui_app_set_status(app, "Failed to open linear audio file");
        return -1;
    }

    // Configure FLAC encoder for stereo 48kHz 16-bit
    flac_writer_config_t config = flac_writer_default_config();
    config.sample_rate = SOUNDCARD_SAMPLE_RATE;
    config.bits_per_sample = SOUNDCARD_BITS_PER_SAMPLE;
    config.num_channels = SOUNDCARD_CHANNELS;
    config.compression_level = app->settings.flac_level;
    config.verify = app->settings.flac_verification;
    config.enable_seektable = true;
    config.seektable_spacing = SOUNDCARD_SAMPLE_RATE * 10;  // Seek point every 10 seconds
    config.error_cb = soundcard_flac_error_callback;
    config.bytes_cb = NULL;  // We don't track compressed bytes for soundcard
    config.callback_user_data = &s_ctx;

    // Create FLAC writer
    s_flac_writer = flac_writer_create_stream(s_output_file, &config);
    if (!s_flac_writer) {
        fprintf(stderr, "[SOUNDCARD FLAC] Failed to create FLAC writer\n");
        gui_app_set_status(app, "Failed to create linear audio encoder");
        fclose(s_output_file);
        s_output_file = NULL;
        return -1;
    }

    // Setup context
    s_ctx.app = app;
    s_ctx.bufmgr = bufmgr;
    s_writer_app = app;

    // Enable soundcard recording BEFORE starting writer thread
    // This ensures data starts flowing into the buffer immediately
    gui_soundcard_set_recording(true);

    // Start writer thread
    if (thrd_create(&s_writer_thread, soundcard_writer_thread, &s_ctx) != thrd_success) {
        fprintf(stderr, "[SOUNDCARD FLAC] Failed to create writer thread\n");
        gui_app_set_status(app, "Failed to start linear audio writer");
        gui_soundcard_set_recording(false);  // Disable on failure
        flac_writer_abort(s_flac_writer);
        s_flac_writer = NULL;
        fclose(s_output_file);
        s_output_file = NULL;
        return -1;
    }

    s_writer_running = true;
    fprintf(stderr, "[SOUNDCARD FLAC] Writer started: %s\n", filepath);
    return 0;

#else
    (void)app;
    (void)bufmgr;
    fprintf(stderr, "[SOUNDCARD FLAC] FLAC support not compiled in\n");
    return -1;
#endif
}

void gui_soundcard_writer_stop(void) {
    if (!s_writer_running) return;

#if LIBFLAC_ENABLED == 1
    fprintf(stderr, "[SOUNDCARD FLAC] Stopping writer\n");

    // Disable soundcard recording first - stop capture thread from writing more data
    gui_soundcard_set_recording(false);

    // Wait for thread to finish (it will drain the buffer)
    thrd_join(s_writer_thread, NULL);

    // Finish FLAC encoding
    if (s_flac_writer) {
        flac_writer_error_t err = flac_writer_finish(s_flac_writer);
        if (err != FLAC_WRITER_OK) {
            fprintf(stderr, "[SOUNDCARD FLAC] Warning: FLAC finish returned error\n");
        }
        s_flac_writer = NULL;
    }

    // Close output file
    if (s_output_file) {
        fclose(s_output_file);
        s_output_file = NULL;
    }

    s_writer_running = false;
    s_writer_app = NULL;
    fprintf(stderr, "[SOUNDCARD FLAC] Writer stopped\n");
#endif
}

bool gui_soundcard_writer_is_running(void) {
    return s_writer_running;
}
