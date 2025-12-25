/*
 * MISRC Common - Capture Handler Implementation
 *
 * Shared capture context and callback infrastructure for CLI and GUI.
 */

#include "capture_handler.h"

#include <stdio.h>
#include <string.h>

/*-----------------------------------------------------------------------------
 * Initialization
 *-----------------------------------------------------------------------------*/

void capture_handler_init(capture_handler_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    frame_parser_init(&ctx->frame_state);
}

void capture_handler_reset_audio_sync(capture_handler_ctx_t *ctx)
{
    ctx->audio_sync_stage1 = false;
    ctx->audio_sync_stage2 = false;
}

/*-----------------------------------------------------------------------------
 * Default Callbacks
 *-----------------------------------------------------------------------------*/

void capture_handler_default_progress(void *user_ctx, unsigned int non_sync_cnt)
{
    (void)user_ctx;

    if (non_sync_cnt % 5 == 0) {
        fprintf(stderr, "\033[A\33[2K\r Received %u frames without sync...\n",
                non_sync_cnt + 1);
    }

    if (non_sync_cnt == 500) {
        fprintf(stderr, " Received more than 500 corrupted frames! Check connection!\n");
    }
}

/*-----------------------------------------------------------------------------
 * Sync Event Handling
 *-----------------------------------------------------------------------------*/

bool capture_handler_process_sync_event(capture_handler_ctx_t *ctx,
                                         frame_sync_result_t sync_result,
                                         const metadata_t *meta,
                                         bool was_synced)
{
    /* Call user callback if set */
    if (ctx->sync_event_cb) {
        ctx->sync_event_cb(ctx->user_ctx, sync_result, meta, was_synced);
    }

    switch (sync_result) {
        case FRAME_SYNC_LOST:
            /* Reset audio sync state when sync is lost */
            capture_handler_reset_audio_sync(ctx);
            return false;

        case FRAME_SYNC_DUPLICATE:
            return false;

        case FRAME_SYNC_MISSED:
            /* Caller may want to log this but can continue processing */
            break;

        case FRAME_SYNC_ACQUIRED:
            /* Reset audio sync state for clean start */
            capture_handler_reset_audio_sync(ctx);

            /* Check if audio is available when requested */
            if (ctx->capture_audio) {
                if (!(meta->flags & FLAG_STREAM_ID_PRESENT)) {
                    /* Audio requested but not available in stream */
                    if (ctx->msg_cb) {
                        ctx->msg_cb(ctx->user_ctx, 3,
                                   "MISRC does not transmit audio, cannot capture audio!\n");
                    }
                    /* Caller should handle this - we continue but audio won't work */
                }
            }
            break;

        case FRAME_SYNC_OK:
            break;
    }

    return true;
}

/*-----------------------------------------------------------------------------
 * Audio Sync Filter
 *-----------------------------------------------------------------------------*/

bool capture_handler_audio_filter(void *ctx, int stream_id,
                                   const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;

    capture_handler_ctx_t *cap = (capture_handler_ctx_t *)ctx;

    if (stream_id == 0) {
        /* RF stream */
        /* Only copy RF if:
         * - We want to capture RF, AND
         * - Either we're not waiting for audio, OR audio sync has started */
        return cap->capture_rf &&
               (!cap->capture_audio || cap->audio_sync_stage1);
    } else if (stream_id == 1) {
        /* Audio stream */
        if (!cap->capture_audio)
            return false;

        if (cap->audio_sync_stage2) {
            /* Fully synced - copy audio normally */
            return true;
        }

        if (cap->audio_sync_stage1) {
            /* Stage 1 -> Stage 2: second audio line seen */
            cap->audio_sync_stage2 = true;
            if (cap->audio_sync_cb) {
                cap->audio_sync_cb(cap->user_ctx, true);
            }
            /* Skip this transition frame */
            return false;
        } else {
            /* Stage 0 -> Stage 1: first audio line seen */
            cap->audio_sync_stage1 = true;
            /* Skip this frame */
            return false;
        }
    }

    /* Unknown stream ID */
    return false;
}
