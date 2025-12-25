/*
 * MISRC Common - Capture Handler
 *
 * Shared capture context and callback infrastructure for CLI and GUI.
 * Provides unified handling of frame synchronization, audio sync state machine,
 * and customizable event callbacks.
 */

#ifndef MISRC_CAPTURE_HANDLER_H
#define MISRC_CAPTURE_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "frame_parser.h"
#include "ringbuffer.h"

/*-----------------------------------------------------------------------------
 * Callback Types
 *-----------------------------------------------------------------------------*/

/* Message callback - for logging messages
 *
 * @param user_ctx      User context
 * @param level         Message level (0=info, 1=warning, 2=error, 3=critical)
 * @param fmt           Format string
 * @param ...           Format arguments
 */
typedef void (*capture_msg_cb_t)(void *user_ctx, int level, const char *fmt, ...);

/* Sync progress callback - called while waiting for sync
 *
 * @param user_ctx      User context
 * @param non_sync_cnt  Number of frames received without sync
 */
typedef void (*capture_sync_progress_cb_t)(void *user_ctx, unsigned int non_sync_cnt);

/* Sync event callback - called on sync state changes
 *
 * @param user_ctx      User context
 * @param result        Sync result (ACQUIRED, LOST, MISSED, etc.)
 * @param meta          Frame metadata (for info display)
 * @param was_synced    True if was synced before this frame
 */
typedef void (*capture_sync_event_cb_t)(void *user_ctx, frame_sync_result_t result,
                                         const metadata_t *meta, bool was_synced);

/* Audio sync callback - called when audio sync state changes
 *
 * @param user_ctx      User context
 * @param synced        True when RF and audio are now synced
 */
typedef void (*capture_audio_sync_cb_t)(void *user_ctx, bool synced);

/*-----------------------------------------------------------------------------
 * Capture Handler Context
 *-----------------------------------------------------------------------------*/

typedef struct {
    /* Ringbuffer pointers (caller owns allocation) */
    ringbuffer_t *rb_rf;              /* RF data ringbuffer (required for capture) */
    ringbuffer_t *rb_audio;           /* Audio data ringbuffer (optional, NULL if no audio) */

    /* Frame parser state */
    frame_parser_state_t frame_state;

    /* Capture configuration */
    bool capture_rf;                  /* Whether to capture RF stream */
    bool capture_audio;               /* Whether to capture audio stream */

    /* Audio sync state machine (two-stage sync)
     * Stage 1: First audio line seen, RF can start
     * Stage 2: Second audio line seen, audio fully synced */
    bool audio_sync_stage1;           /* First audio line seen */
    bool audio_sync_stage2;           /* Fully synced */

    /* Callbacks (all optional - can be NULL) */
    capture_msg_cb_t msg_cb;          /* For logging messages */
    capture_sync_progress_cb_t progress_cb;  /* For "waiting for sync" updates */
    capture_sync_event_cb_t sync_event_cb;   /* For sync state changes */
    capture_audio_sync_cb_t audio_sync_cb;   /* For audio sync notification */

    /* User context passed to all callbacks */
    void *user_ctx;
} capture_handler_ctx_t;

/*-----------------------------------------------------------------------------
 * Initialization
 *-----------------------------------------------------------------------------*/

/* Initialize capture handler context
 *
 * @param ctx           Context to initialize
 *
 * Sets all fields to safe defaults. Caller should then set:
 * - rb_rf (required for capture)
 * - rb_audio (optional for audio capture)
 * - capture_rf, capture_audio flags
 * - callbacks as needed
 * - user_ctx for callbacks
 */
void capture_handler_init(capture_handler_ctx_t *ctx);

/* Reset audio sync state machine
 *
 * @param ctx           Context to reset
 *
 * Call this when sync is lost or when starting a new capture session.
 */
void capture_handler_reset_audio_sync(capture_handler_ctx_t *ctx);

/*-----------------------------------------------------------------------------
 * Default Callbacks (for CLI-style output)
 *-----------------------------------------------------------------------------*/

/* Default sync progress callback - prints to stderr
 *
 * @param user_ctx      Unused
 * @param non_sync_cnt  Number of frames received without sync
 *
 * Prints progress every 5 frames and warns at 500 frames.
 * Can be used directly as progress_cb or called from custom callback.
 */
void capture_handler_default_progress(void *user_ctx, unsigned int non_sync_cnt);

/*-----------------------------------------------------------------------------
 * Sync Event Handling
 *-----------------------------------------------------------------------------*/

/* Process sync event and determine whether to continue
 *
 * @param ctx           Capture handler context
 * @param sync_result   Sync result from frame_process()
 * @param meta          Frame metadata (for info display)
 * @param was_synced    True if was synced before this frame
 * @return True if callback should continue processing, false to return early
 *
 * Handles LOST, DUPLICATE, MISSED, ACQUIRED, OK cases.
 * Calls sync_event_cb if set.
 * Returns false for LOST and DUPLICATE (caller should return).
 * Also resets audio sync state on LOST.
 */
bool capture_handler_process_sync_event(capture_handler_ctx_t *ctx,
                                         frame_sync_result_t sync_result,
                                         const metadata_t *meta,
                                         bool was_synced);

/*-----------------------------------------------------------------------------
 * Audio Sync Filter
 *-----------------------------------------------------------------------------*/

/* Audio sync filter callback for use with frame_copy_payloads_cb()
 *
 * @param ctx           Capture handler context (cast from void*)
 * @param stream_id     Stream ID (0=RF, 1=audio)
 * @param data          Pointer to payload data (unused)
 * @param len           Length of payload in bytes (unused)
 * @return True to copy this payload, false to skip
 *
 * Implements two-stage audio sync state machine:
 * - Stage 0: Wait for first audio line, skip all data
 * - Stage 1: First audio seen, copy RF, skip audio
 * - Stage 2: Second audio seen, copy both RF and audio
 *
 * Usage:
 *   frame_copy_payloads_cb(buf, width, height, meta,
 *                          out_rf, out_audio,
 *                          capture_handler_audio_filter, ctx);
 */
bool capture_handler_audio_filter(void *ctx, int stream_id,
                                   const uint8_t *data, size_t len);

#endif /* MISRC_CAPTURE_HANDLER_H */
