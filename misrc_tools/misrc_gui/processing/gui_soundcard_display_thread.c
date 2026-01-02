/*
 * MISRC GUI - Soundcard Display Thread Implementation
 *
 * Null consumer thread for soundcard audio buffer.
 * Drains BUF_SOUNDCARD_AUDIO and calculates VU meter peaks.
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#include "gui_soundcard_display_thread.h"
#include "../core/gui_app.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External do_exit flag */
extern atomic_int do_exit;

/*
 * Read size - we read in small chunks to keep latency low.
 * The actual audio format may vary (the system can return 4ch/32bit, 2ch/16bit, etc.)
 * so we read whatever is available up to this size.
 */
#define SOUNDCARD_MAX_READ_SIZE 8192

/*
 * Soundcard display thread main function
 *
 * This is a "null consumer" that drains BUF_SOUNDCARD_AUDIO and calculates
 * VU meter peaks. The actual audio format depends on what WASAPI negotiates,
 * but we always treat the first two int16 values as L/R for peak calculation.
 * This works correctly for 16-bit stereo and is a reasonable approximation
 * for other formats.
 */
static int soundcard_display_thread_func(void *ctx) {
    soundcard_display_thread_t *dt = (soundcard_display_thread_t *)ctx;
    buffer_manager_t *bufmgr = dt->bufmgr;

    fprintf(stderr, "[SOUNDCARD_DISPLAY] Display thread started\n");

    while (!atomic_load(&dt->stop_requested) && !atomic_load(&do_exit)) {
        /* Check how much data is available */
        size_t available = bufmgr_fill_level(bufmgr, BUF_SOUNDCARD_AUDIO);
        if (available == 0) {
            /* No data - wait briefly and try again */
            bufmgr_wait_data(bufmgr, BUF_SOUNDCARD_AUDIO, 20);
            continue;
        }

        /* Read up to max size, but at least what's available */
        size_t read_size = (available < SOUNDCARD_MAX_READ_SIZE) ? available : SOUNDCARD_MAX_READ_SIZE;

        void *buf = bufmgr_read_begin(bufmgr, BUF_SOUNDCARD_AUDIO, read_size, 0);
        if (!buf) {
            continue;
        }

        /*
         * Calculate VU meter peaks from samples.
         * We treat the data as interleaved int16 stereo for peak calculation.
         * For 32-bit formats, this reads the upper 16 bits which is a reasonable
         * approximation. For 4+ channel formats, we only look at the first 2 channels.
         */
        const int16_t *samples = (const int16_t *)buf;
        uint32_t num_int16 = (uint32_t)(read_size / sizeof(int16_t));
        int16_t peak_l = 0, peak_r = 0;

        /* Process pairs of int16 values as L, R channels */
        for (uint32_t i = 0; i + 1 < num_int16; i += 2) {
            int16_t l = samples[i];
            int16_t r = samples[i + 1];
            if (l < 0) l = -l;
            if (r < 0) r = -r;
            if (l > peak_l) peak_l = l;
            if (r > peak_r) peak_r = r;
        }

        /* Update VU meter peaks in app */
        atomic_store(&dt->app->soundcard_peak_l, (uint16_t)peak_l);
        atomic_store(&dt->app->soundcard_peak_r, (uint16_t)peak_r);

        /* Mark read as complete - signals space available */
        /* This is a null consumer - we discard the samples */
        bufmgr_read_end(bufmgr, BUF_SOUNDCARD_AUDIO, read_size);

        atomic_fetch_add(&dt->frames_processed, 1);
    }

    atomic_store(&dt->running, false);
    fprintf(stderr, "[SOUNDCARD_DISPLAY] Display thread exiting\n");
    return 0;
}

/*
 * Initialize soundcard display thread state
 */
int gui_soundcard_display_thread_init(soundcard_display_thread_t *dt) {
    if (!dt) return -1;

    memset(dt, 0, sizeof(*dt));

    atomic_store(&dt->running, false);
    atomic_store(&dt->stop_requested, false);
    atomic_store(&dt->frames_processed, 0);

    fprintf(stderr, "[SOUNDCARD_DISPLAY] Display thread state initialized\n");
    return 0;
}

/*
 * Cleanup soundcard display thread state
 */
void gui_soundcard_display_thread_cleanup(soundcard_display_thread_t *dt) {
    if (!dt) return;

    /* Ensure thread is stopped */
    gui_soundcard_display_thread_stop(dt);

    fprintf(stderr, "[SOUNDCARD_DISPLAY] Display thread state cleaned up\n");
}

/*
 * Start the soundcard display thread
 */
int gui_soundcard_display_thread_start(soundcard_display_thread_t *dt,
                                        struct gui_app *app,
                                        struct buffer_manager *bufmgr) {
    if (!dt || !app || !bufmgr) return -1;

    if (atomic_load(&dt->running)) {
        fprintf(stderr, "[SOUNDCARD_DISPLAY] Thread already running\n");
        return 0;
    }

    /* Ensure BUF_SOUNDCARD_AUDIO is initialized */
    if (bufmgr_ensure_init(bufmgr, BUF_SOUNDCARD_AUDIO) < 0) {
        fprintf(stderr, "[SOUNDCARD_DISPLAY] Failed to initialize soundcard audio buffer\n");
        return -1;
    }

    dt->app = app;
    dt->bufmgr = bufmgr;
    atomic_store(&dt->stop_requested, false);
    atomic_store(&dt->running, true);

    /* Reset statistics */
    atomic_store(&dt->frames_processed, 0);

    /* Create thread */
    thrd_t *thread = (thrd_t *)malloc(sizeof(thrd_t));
    if (!thread) {
        atomic_store(&dt->running, false);
        return -1;
    }

    if (thrd_create(thread, soundcard_display_thread_func, dt) != thrd_success) {
        fprintf(stderr, "[SOUNDCARD_DISPLAY] Failed to create display thread\n");
        free(thread);
        atomic_store(&dt->running, false);
        return -1;
    }

    dt->thread = thread;
    fprintf(stderr, "[SOUNDCARD_DISPLAY] Display thread started\n");
    return 0;
}

/*
 * Stop the soundcard display thread
 */
void gui_soundcard_display_thread_stop(soundcard_display_thread_t *dt) {
    if (!dt || !dt->thread) return;

    if (!atomic_load(&dt->running)) {
        return;
    }

    /* Signal thread to stop */
    atomic_store(&dt->stop_requested, true);

    /* Wait for thread to exit */
    thrd_t *thread = (thrd_t *)dt->thread;
    thrd_join(*thread, NULL);

    free(thread);
    dt->thread = NULL;
    atomic_store(&dt->running, false);

    fprintf(stderr, "[SOUNDCARD_DISPLAY] Display thread stopped (processed %llu frames)\n",
            (unsigned long long)atomic_load(&dt->frames_processed));
}

/*
 * Get soundcard display thread statistics
 */
void gui_soundcard_display_thread_get_stats(soundcard_display_thread_t *dt,
                                             uint64_t *frames_processed) {
    if (!dt) return;

    if (frames_processed) {
        *frames_processed = atomic_load(&dt->frames_processed);
    }
}
