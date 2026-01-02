/*
 * MISRC GUI - Soundcard Display Thread
 *
 * Dedicated thread for consuming soundcard audio data from BUF_SOUNDCARD_AUDIO.
 * This is a "null consumer" that drains the buffer and calculates VU meter peaks,
 * ensuring the capture thread doesn't stall due to backpressure.
 *
 * Architecture:
 *   Soundcard Capture Thread --[BUF_SOUNDCARD_AUDIO]--> Display Thread --> VU Meter
 *                           \--[BUF_SOUNDCARD_RECORD]--> Writer Thread --> FLAC File
 *
 * The display thread reads from BUF_SOUNDCARD_AUDIO (lossy buffer) and:
 *   - Calculates VU meter peaks for display
 *   - Discards the samples (null consumer)
 *
 * If the display thread is slow, frames are dropped from BUF_SOUNDCARD_AUDIO
 * without affecting the recording path (which uses BUF_SOUNDCARD_RECORD).
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#ifndef GUI_SOUNDCARD_DISPLAY_THREAD_H
#define GUI_SOUNDCARD_DISPLAY_THREAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/* Forward declarations */
struct gui_app;
struct buffer_manager;

/*
 * Soundcard display thread state
 */
typedef struct soundcard_display_thread {
    /* Thread handle */
    void *thread;                /* thrd_t */
    atomic_bool running;         /* Thread running flag */
    atomic_bool stop_requested;  /* Request thread to stop */

    /* Buffer manager reference */
    struct buffer_manager *bufmgr;

    /* App reference (for setting VU meter peaks) */
    struct gui_app *app;

    /* Statistics */
    atomic_uint_fast64_t frames_processed;
} soundcard_display_thread_t;

/*
 * Initialize soundcard display thread state
 *
 * @param dt    Display thread state
 * @return 0 on success, -1 on error
 */
int gui_soundcard_display_thread_init(soundcard_display_thread_t *dt);

/*
 * Cleanup soundcard display thread state
 *
 * @param dt    Display thread state
 */
void gui_soundcard_display_thread_cleanup(soundcard_display_thread_t *dt);

/*
 * Start the soundcard display thread
 *
 * @param dt        Display thread state
 * @param app       Application state (for setting VU meter peaks)
 * @param bufmgr    Buffer manager (for reading BUF_SOUNDCARD_AUDIO)
 * @return 0 on success, -1 on error
 */
int gui_soundcard_display_thread_start(soundcard_display_thread_t *dt,
                                        struct gui_app *app,
                                        struct buffer_manager *bufmgr);

/*
 * Stop the soundcard display thread (waits for thread to exit)
 *
 * @param dt    Display thread state
 */
void gui_soundcard_display_thread_stop(soundcard_display_thread_t *dt);

/*
 * Get soundcard display thread statistics
 */
void gui_soundcard_display_thread_get_stats(soundcard_display_thread_t *dt,
                                             uint64_t *frames_processed);

#endif /* GUI_SOUNDCARD_DISPLAY_THREAD_H */
