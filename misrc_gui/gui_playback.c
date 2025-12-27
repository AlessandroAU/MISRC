/*
 * MISRC GUI - Playback Mode Implementation
 *
 * Reads FLAC files and plays them back as if being captured live.
 * Uses libFLAC stream decoder for reading.
 */

#include "gui_playback.h"
#include "gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_cvbs.h"
#include "gui_extract.h"
#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#if LIBFLAC_ENABLED == 1
#include "FLAC/stream_decoder.h"
#include "FLAC/metadata.h"
#endif

//-----------------------------------------------------------------------------
// Playback State
//-----------------------------------------------------------------------------

typedef struct {
    // File handles and decoders
#if LIBFLAC_ENABLED == 1
    FLAC__StreamDecoder *decoder_a;
    FLAC__StreamDecoder *decoder_b;
#endif
    FILE *file_a;
    FILE *file_b;

    // File info
    playback_file_info_t info_a;
    playback_file_info_t info_b;

    // Decode buffers (filled by FLAC decoder callbacks)
    int16_t *decode_buf_a;
    int16_t *decode_buf_b;
    size_t decode_buf_size;        // Allocated size
    size_t decode_available_a;     // Samples available in buffer
    size_t decode_available_b;
    size_t decode_pos_a;           // Current read position in buffer
    size_t decode_pos_b;

    // Playback state
    atomic_int state;              // playback_state_t
    atomic_int speed;              // playback_speed_t
    atomic_bool loop_enabled;
    atomic_bool seek_requested;
    atomic_uint_fast64_t seek_target;

    // Position tracking
    atomic_uint_fast64_t current_sample;
    uint64_t total_samples;        // Max of file_a and file_b totals

    // Thread
    void *playback_thread;
    atomic_bool running;

    // App reference
    gui_app_t *app;
} playback_ctx_t;

static playback_ctx_t s_playback = {0};

//-----------------------------------------------------------------------------
// Speed Multipliers
//-----------------------------------------------------------------------------

static const float speed_multipliers[] = {
    0.25f,   // PLAYBACK_SPEED_0_25X
    0.5f,    // PLAYBACK_SPEED_0_5X
    1.0f,    // PLAYBACK_SPEED_1X
    2.0f,    // PLAYBACK_SPEED_2X
    4.0f,    // PLAYBACK_SPEED_4X
    0.0f     // PLAYBACK_SPEED_MAX (no delay)
};

const char* gui_playback_speed_name(playback_speed_t speed) {
    switch (speed) {
        case PLAYBACK_SPEED_0_25X: return "0.25x";
        case PLAYBACK_SPEED_0_5X:  return "0.5x";
        case PLAYBACK_SPEED_1X:    return "1x";
        case PLAYBACK_SPEED_2X:    return "2x";
        case PLAYBACK_SPEED_4X:    return "4x";
        case PLAYBACK_SPEED_MAX:   return "Max";
        default: return "?";
    }
}

//-----------------------------------------------------------------------------
// FLAC Decoder Callbacks
//-----------------------------------------------------------------------------

#if LIBFLAC_ENABLED == 1

// Channel A write callback
static FLAC__StreamDecoderWriteStatus decoder_write_cb_a(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    uint32_t samples = frame->header.blocksize;
    uint8_t bps = frame->header.bits_per_sample;

    // Ensure buffer space
    if (ctx->decode_available_a + samples > ctx->decode_buf_size) {
        // Buffer overflow - shouldn't happen with proper flow control
        fprintf(stderr, "[PLAYBACK] Channel A decode buffer overflow\n");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    // Convert to int16 and store
    int16_t *out = ctx->decode_buf_a + ctx->decode_available_a;
    const FLAC__int32 *in = buffer[0];  // Mono FLAC

    // Debug: print first frame's sample info
    static int debug_count_a = 0;
    if (debug_count_a < 5) {
        int32_t min_val = in[0], max_val = in[0];
        for (uint32_t i = 1; i < samples && i < 1000; i++) {
            if (in[i] < min_val) min_val = in[i];
            if (in[i] > max_val) max_val = in[i];
        }
        fprintf(stderr, "[PLAYBACK] Ch A frame %d: bps=%u, samples=%u, range=[%d, %d]\n",
                debug_count_a, bps, samples, min_val, max_val);
        debug_count_a++;
    }

    // FLAC returns samples as int32 with the full bit depth
    // MISRC recording format:
    //   16-bit FLAC: 12-bit ADC samples shifted left 4 bits (range -32768 to 32752)
    //   12-bit FLAC: 12-bit ADC samples as-is (range -2048 to 2047)
    //   8-bit FLAC: samples clamped to 8-bit (range -128 to 127)
    // Display expects 12-bit range values (like simulated mode uses ~1024 scale)
    if (bps == 16) {
        // 16-bit FLAC: shift right 4 to get back to 12-bit range
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] >> 4);
        }
    } else if (bps == 12) {
        // 12-bit FLAC: already in 12-bit range, just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else if (bps == 8) {
        // 8-bit FLAC: keep in 8-bit range (-128 to 127), don't scale up
        // This will show reduced amplitude on display, reflecting the lower bit depth
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else {
        // Unknown bit depth - just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    }

    ctx->decode_available_a += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// Channel B write callback
static FLAC__StreamDecoderWriteStatus decoder_write_cb_b(
    const FLAC__StreamDecoder *decoder,
    const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[],
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    uint32_t samples = frame->header.blocksize;
    uint8_t bps = frame->header.bits_per_sample;

    if (ctx->decode_available_b + samples > ctx->decode_buf_size) {
        fprintf(stderr, "[PLAYBACK] Channel B decode buffer overflow\n");
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    int16_t *out = ctx->decode_buf_b + ctx->decode_available_b;
    const FLAC__int32 *in = buffer[0];

    // Debug: print first frame's sample info
    static int debug_count_b = 0;
    if (debug_count_b < 5) {
        int32_t min_val = in[0], max_val = in[0];
        for (uint32_t i = 1; i < samples && i < 1000; i++) {
            if (in[i] < min_val) min_val = in[i];
            if (in[i] > max_val) max_val = in[i];
        }
        fprintf(stderr, "[PLAYBACK] Ch B frame %d: bps=%u, samples=%u, range=[%d, %d]\n",
                debug_count_b, bps, samples, min_val, max_val);
        debug_count_b++;
    }

    // Same conversion as Channel A - normalize to 12-bit range for display
    if (bps == 16) {
        // 16-bit FLAC: shift right 4 to get back to 12-bit range
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)(in[i] >> 4);
        }
    } else if (bps == 12) {
        // 12-bit FLAC: already in 12-bit range, just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else if (bps == 8) {
        // 8-bit FLAC: keep in 8-bit range, don't scale up
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    } else {
        // Unknown bit depth - just cast
        for (uint32_t i = 0; i < samples; i++) {
            out[i] = (int16_t)in[i];
        }
    }

    ctx->decode_available_b += samples;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

// Metadata callback (extracts stream info)
static void decoder_metadata_cb_a(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        ctx->info_a.sample_rate = metadata->data.stream_info.sample_rate;
        ctx->info_a.bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ctx->info_a.total_samples = metadata->data.stream_info.total_samples;
        if (ctx->info_a.sample_rate > 0) {
            ctx->info_a.duration_seconds = (double)ctx->info_a.total_samples / ctx->info_a.sample_rate;
        }
        ctx->info_a.valid = true;
        fprintf(stderr, "[PLAYBACK] File A: %u Hz, %u-bit, %llu samples (%.1f sec)\n",
                ctx->info_a.sample_rate, ctx->info_a.bits_per_sample,
                (unsigned long long)ctx->info_a.total_samples, ctx->info_a.duration_seconds);
    }
}

static void decoder_metadata_cb_b(
    const FLAC__StreamDecoder *decoder,
    const FLAC__StreamMetadata *metadata,
    void *client_data)
{
    (void)decoder;
    playback_ctx_t *ctx = (playback_ctx_t *)client_data;

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        ctx->info_b.sample_rate = metadata->data.stream_info.sample_rate;
        ctx->info_b.bits_per_sample = metadata->data.stream_info.bits_per_sample;
        ctx->info_b.total_samples = metadata->data.stream_info.total_samples;
        if (ctx->info_b.sample_rate > 0) {
            ctx->info_b.duration_seconds = (double)ctx->info_b.total_samples / ctx->info_b.sample_rate;
        }
        ctx->info_b.valid = true;
        fprintf(stderr, "[PLAYBACK] File B: %u Hz, %u-bit, %llu samples (%.1f sec)\n",
                ctx->info_b.sample_rate, ctx->info_b.bits_per_sample,
                (unsigned long long)ctx->info_b.total_samples, ctx->info_b.duration_seconds);
    }
}

// Error callback
static void decoder_error_cb(
    const FLAC__StreamDecoder *decoder,
    FLAC__StreamDecoderErrorStatus status,
    void *client_data)
{
    (void)decoder;
    (void)client_data;
    fprintf(stderr, "[PLAYBACK] FLAC decoder error: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

#endif // LIBFLAC_ENABLED

//-----------------------------------------------------------------------------
// File Validation
//-----------------------------------------------------------------------------

bool gui_playback_validate_file(const char *filepath, playback_file_info_t *info) {
    if (!filepath || !info) return false;

    memset(info, 0, sizeof(*info));
    strncpy(info->filepath, filepath, sizeof(info->filepath) - 1);

#if LIBFLAC_ENABLED == 1
    // Use FLAC metadata API to read stream info without full decode
    // FLAC__metadata_get_streaminfo fills in an existing struct
    FLAC__StreamMetadata metadata;

    if (!FLAC__metadata_get_streaminfo(filepath, &metadata)) {
        fprintf(stderr, "[PLAYBACK] Failed to read FLAC metadata from: %s\n", filepath);
        return false;
    }

    FLAC__StreamMetadata_StreamInfo *si = &metadata.data.stream_info;

    info->sample_rate = si->sample_rate;
    info->bits_per_sample = si->bits_per_sample;
    info->total_samples = si->total_samples;

    if (info->sample_rate > 0) {
        info->duration_seconds = (double)info->total_samples / info->sample_rate;
    }

    // Validate compatibility with MISRC format
    // Expected: 40kHz, 8/12/16-bit, mono
    bool compatible = true;

    if (info->sample_rate != 40000) {
        fprintf(stderr, "[PLAYBACK] Warning: Sample rate %u Hz (expected 40000 Hz)\n", info->sample_rate);
        // Allow non-40kHz files, but warn
    }

    if (si->channels != 1) {
        fprintf(stderr, "[PLAYBACK] Error: FLAC has %u channels (expected mono)\n", si->channels);
        compatible = false;
    }

    if (info->bits_per_sample != 8 && info->bits_per_sample != 12 && info->bits_per_sample != 16) {
        fprintf(stderr, "[PLAYBACK] Warning: Bit depth %u (expected 8, 12, or 16)\n", info->bits_per_sample);
        // Still allow, will attempt conversion
    }

    info->valid = compatible;
    return compatible;

#else
    fprintf(stderr, "[PLAYBACK] FLAC support not compiled in\n");
    return false;
#endif
}

//-----------------------------------------------------------------------------
// Playback Thread
//-----------------------------------------------------------------------------

static int playback_thread_func(void *ctx_ptr) {
    playback_ctx_t *ctx = (playback_ctx_t *)ctx_ptr;
    gui_app_t *app = ctx->app;

    fprintf(stderr, "[PLAYBACK] Playback thread started\n");

    // Allocate output buffers
    int16_t *buf_a = (int16_t *)malloc(PLAYBACK_BUFFER_SIZE * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(PLAYBACK_BUFFER_SIZE * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[PLAYBACK] Failed to allocate output buffers\n");
        free(buf_a);
        free(buf_b);
        atomic_store(&ctx->state, PLAYBACK_STATE_STOPPED);
        return -1;
    }

    gui_extract_init_record_rbs();

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, PLAYBACK_SAMPLE_RATE);

    uint64_t batch_count = 0;

    while (atomic_load(&ctx->running)) {
        // Check for pause state
        if (atomic_load(&ctx->state) == PLAYBACK_STATE_PAUSED) {
            thrd_sleep_ms(10);
            continue;
        }

        // Check for seek request
        if (atomic_load(&ctx->seek_requested)) {
            uint64_t target = atomic_load(&ctx->seek_target);
            atomic_store(&ctx->seek_requested, false);

#if LIBFLAC_ENABLED == 1
            // Seek both decoders
            if (ctx->decoder_a) {
                FLAC__stream_decoder_seek_absolute(ctx->decoder_a, target);
                ctx->decode_available_a = 0;
                ctx->decode_pos_a = 0;
            }
            if (ctx->decoder_b) {
                FLAC__stream_decoder_seek_absolute(ctx->decoder_b, target);
                ctx->decode_available_b = 0;
                ctx->decode_pos_b = 0;
            }
#endif
            atomic_store(&ctx->current_sample, target);
            continue;
        }

        // Decode more samples if needed
#if LIBFLAC_ENABLED == 1
        // Decode channel A
        while (ctx->decoder_a && ctx->decode_available_a - ctx->decode_pos_a < PLAYBACK_BUFFER_SIZE) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_a)) {
                break;
            }
        }

        // Decode channel B
        while (ctx->decoder_b && ctx->decode_available_b - ctx->decode_pos_b < PLAYBACK_BUFFER_SIZE) {
            if (FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM) {
                break;
            }
            if (!FLAC__stream_decoder_process_single(ctx->decoder_b)) {
                break;
            }
        }
#endif

        // Calculate how many samples we can output
        size_t avail_a = ctx->decode_available_a - ctx->decode_pos_a;
        size_t avail_b = ctx->decode_available_b - ctx->decode_pos_b;
        size_t samples_to_output = PLAYBACK_BUFFER_SIZE;

        // Use minimum of what's available, or check for EOF
        bool eof_a = (ctx->decoder_a == NULL) || (avail_a == 0 &&
                      FLAC__stream_decoder_get_state(ctx->decoder_a) == FLAC__STREAM_DECODER_END_OF_STREAM);
        bool eof_b = (ctx->decoder_b == NULL) || (avail_b == 0 &&
                      FLAC__stream_decoder_get_state(ctx->decoder_b) == FLAC__STREAM_DECODER_END_OF_STREAM);

        // Both channels at EOF?
        if ((ctx->decoder_a && ctx->decoder_b && eof_a && eof_b) ||
            (ctx->decoder_a && !ctx->decoder_b && eof_a) ||
            (!ctx->decoder_a && ctx->decoder_b && eof_b)) {

            if (atomic_load(&ctx->loop_enabled)) {
                // Loop back to beginning
                fprintf(stderr, "[PLAYBACK] Looping back to start\n");
#if LIBFLAC_ENABLED == 1
                if (ctx->decoder_a) {
                    FLAC__stream_decoder_seek_absolute(ctx->decoder_a, 0);
                    ctx->decode_available_a = 0;
                    ctx->decode_pos_a = 0;
                }
                if (ctx->decoder_b) {
                    FLAC__stream_decoder_seek_absolute(ctx->decoder_b, 0);
                    ctx->decode_available_b = 0;
                    ctx->decode_pos_b = 0;
                }
#endif
                atomic_store(&ctx->current_sample, 0);
                continue;
            } else {
                fprintf(stderr, "[PLAYBACK] End of file reached\n");
                atomic_store(&ctx->state, PLAYBACK_STATE_EOF);
                break;
            }
        }

        // Limit output to available samples
        if (ctx->decoder_a && avail_a < samples_to_output && !eof_a) {
            samples_to_output = avail_a;
        }
        if (ctx->decoder_b && avail_b < samples_to_output && !eof_b) {
            samples_to_output = avail_b;
        }

        if (samples_to_output == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        // Fill output buffers
        if (ctx->decoder_a && avail_a > 0) {
            size_t to_copy = (avail_a < samples_to_output) ? avail_a : samples_to_output;
            memcpy(buf_a, ctx->decode_buf_a + ctx->decode_pos_a, to_copy * sizeof(int16_t));
            ctx->decode_pos_a += to_copy;
            // Zero-pad if needed
            if (to_copy < samples_to_output) {
                memset(buf_a + to_copy, 0, (samples_to_output - to_copy) * sizeof(int16_t));
            }
        } else {
            memset(buf_a, 0, samples_to_output * sizeof(int16_t));
        }

        if (ctx->decoder_b && avail_b > 0) {
            size_t to_copy = (avail_b < samples_to_output) ? avail_b : samples_to_output;
            memcpy(buf_b, ctx->decode_buf_b + ctx->decode_pos_b, to_copy * sizeof(int16_t));
            ctx->decode_pos_b += to_copy;
            if (to_copy < samples_to_output) {
                memset(buf_b + to_copy, 0, (samples_to_output - to_copy) * sizeof(int16_t));
            }
        } else {
            memset(buf_b, 0, samples_to_output * sizeof(int16_t));
        }

        // Compact decode buffers if needed (shift remaining data to start)
        if (ctx->decode_pos_a > ctx->decode_buf_size / 2) {
            size_t remaining = ctx->decode_available_a - ctx->decode_pos_a;
            if (remaining > 0) {
                memmove(ctx->decode_buf_a, ctx->decode_buf_a + ctx->decode_pos_a,
                        remaining * sizeof(int16_t));
            }
            ctx->decode_available_a = remaining;
            ctx->decode_pos_a = 0;
        }
        if (ctx->decode_pos_b > ctx->decode_buf_size / 2) {
            size_t remaining = ctx->decode_available_b - ctx->decode_pos_b;
            if (remaining > 0) {
                memmove(ctx->decode_buf_b, ctx->decode_buf_b + ctx->decode_pos_b,
                        remaining * sizeof(int16_t));
            }
            ctx->decode_available_b = remaining;
            ctx->decode_pos_b = 0;
        }

        // Update display
        gui_oscilloscope_update_display(app, buf_a, buf_b, samples_to_output);

        // CVBS decode (if enabled)
        cvbs_decoder_t *cvbs_a = atomic_load(&app->cvbs_a);
        if (cvbs_a) {
            atomic_fetch_add(&app->cvbs_busy_a, 1);
            int sys = atomic_load(&app->cvbs_system_a);
            gui_cvbs_set_format(cvbs_a, sys);
            gui_cvbs_process_buffer(cvbs_a, buf_a, samples_to_output);
            atomic_fetch_sub(&app->cvbs_busy_a, 1);
        }
        cvbs_decoder_t *cvbs_b = atomic_load(&app->cvbs_b);
        if (cvbs_b) {
            atomic_fetch_add(&app->cvbs_busy_b, 1);
            int sys = atomic_load(&app->cvbs_system_b);
            gui_cvbs_set_format(cvbs_b, sys);
            gui_cvbs_process_buffer(cvbs_b, buf_b, samples_to_output);
            atomic_fetch_sub(&app->cvbs_busy_b, 1);
        }

        // Update statistics
        atomic_fetch_add(&app->total_samples, samples_to_output);
        atomic_fetch_add(&app->samples_a, samples_to_output);
        atomic_fetch_add(&app->samples_b, samples_to_output);
        atomic_fetch_add(&app->frame_count, 1);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
        atomic_fetch_add(&ctx->current_sample, samples_to_output);

        // Update peak values for VU meters
        int16_t peak_a_pos = 0, peak_a_neg = 0;
        int16_t peak_b_pos = 0, peak_b_neg = 0;
        for (size_t i = 0; i < samples_to_output; i += 16) {
            if (buf_a[i] > peak_a_pos) peak_a_pos = buf_a[i];
            if (buf_a[i] < peak_a_neg) peak_a_neg = buf_a[i];
            if (buf_b[i] > peak_b_pos) peak_b_pos = buf_b[i];
            if (buf_b[i] < peak_b_neg) peak_b_neg = buf_b[i];
        }
        atomic_store(&app->peak_a_pos, (uint16_t)peak_a_pos);
        atomic_store(&app->peak_a_neg, (uint16_t)(-peak_a_neg));
        atomic_store(&app->peak_b_pos, (uint16_t)peak_b_pos);
        atomic_store(&app->peak_b_neg, (uint16_t)(-peak_b_neg));

        // Write to record ringbuffers if recording is enabled
        bool use_flac = false;
        if (gui_extract_is_recording(&use_flac)) {
            ringbuffer_t *rb_a = gui_extract_get_record_rb_a();
            ringbuffer_t *rb_b = gui_extract_get_record_rb_b();

            if (rb_a && rb_b) {
                if (use_flac) {
                    size_t sample_bytes = samples_to_output * sizeof(int32_t);

                    int32_t *write_a = (int32_t *)rb_write_ptr(rb_a, sample_bytes);
                    int32_t *write_b = (int32_t *)rb_write_ptr(rb_b, sample_bytes);

                    if (write_a && write_b) {
                        for (size_t i = 0; i < samples_to_output; i++) {
                            write_a[i] = (int32_t)buf_a[i] << 4;
                            write_b[i] = (int32_t)buf_b[i] << 4;
                        }
                        rb_write_finished(rb_a, sample_bytes);
                        rb_write_finished(rb_b, sample_bytes);
                    }
                } else {
                    size_t sample_bytes = samples_to_output * sizeof(int16_t);

                    void *write_a = rb_write_ptr(rb_a, sample_bytes);
                    void *write_b = rb_write_ptr(rb_b, sample_bytes);

                    if (write_a && write_b) {
                        memcpy(write_a, buf_a, sample_bytes);
                        memcpy(write_b, buf_b, sample_bytes);
                        rb_write_finished(rb_a, sample_bytes);
                        rb_write_finished(rb_b, sample_bytes);
                    }
                }
            }
        }

        batch_count++;

        // Throttle based on playback speed
        playback_speed_t speed = atomic_load(&ctx->speed);
        if (speed != PLAYBACK_SPEED_MAX) {
            float multiplier = speed_multipliers[speed];
            if (multiplier > 0) {
                // Calculate delay based on samples output and speed
                // At 40kHz, 65536 samples = 1.6384 seconds of real-time audio
                // We want to output in ~2ms chunks to match simulated mode
                uint32_t delay_ms = (uint32_t)(PLAYBACK_UPDATE_INTERVAL_MS / multiplier);
                if (delay_ms > 0) {
                    thrd_sleep_ms(delay_ms);
                }
            }
        }
    }

    fprintf(stderr, "[PLAYBACK] Playback thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(buf_a);
    free(buf_b);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_playback_start(gui_app_t *app, const char *file_a, const char *file_b) {
    if (!app) return -1;
    if (!file_a && !file_b) {
        fprintf(stderr, "[PLAYBACK] No files specified\n");
        return -1;
    }

#if LIBFLAC_ENABLED != 1
    fprintf(stderr, "[PLAYBACK] FLAC support not compiled in\n");
    gui_app_set_status(app, "FLAC support not available");
    return -1;
#else

    // Stop any existing playback
    if (gui_playback_is_running(app)) {
        gui_playback_stop(app);
    }

    fprintf(stderr, "[PLAYBACK] Starting playback\n");
    if (file_a) fprintf(stderr, "[PLAYBACK]   Channel A: %s\n", file_a);
    if (file_b) fprintf(stderr, "[PLAYBACK]   Channel B: %s\n", file_b);

    // Reset state
    memset(&s_playback, 0, sizeof(s_playback));
    s_playback.app = app;

    // Allocate decode buffers (larger than output buffer to allow streaming)
    s_playback.decode_buf_size = PLAYBACK_BUFFER_SIZE * 8;
    s_playback.decode_buf_a = (int16_t *)malloc(s_playback.decode_buf_size * sizeof(int16_t));
    s_playback.decode_buf_b = (int16_t *)malloc(s_playback.decode_buf_size * sizeof(int16_t));

    if (!s_playback.decode_buf_a || !s_playback.decode_buf_b) {
        fprintf(stderr, "[PLAYBACK] Failed to allocate decode buffers\n");
        free(s_playback.decode_buf_a);
        free(s_playback.decode_buf_b);
        return -1;
    }

    // Open and initialize decoders
    if (file_a) {
        strncpy(s_playback.info_a.filepath, file_a, sizeof(s_playback.info_a.filepath) - 1);

        s_playback.file_a = fopen(file_a, "rb");
        if (!s_playback.file_a) {
            fprintf(stderr, "[PLAYBACK] Failed to open file A: %s\n", file_a);
            goto error_cleanup;
        }

        s_playback.decoder_a = FLAC__stream_decoder_new();
        if (!s_playback.decoder_a) {
            fprintf(stderr, "[PLAYBACK] Failed to create decoder A\n");
            goto error_cleanup;
        }

        FLAC__stream_decoder_set_md5_checking(s_playback.decoder_a, false);

        FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_FILE(
            s_playback.decoder_a,
            s_playback.file_a,
            decoder_write_cb_a,
            decoder_metadata_cb_a,
            decoder_error_cb,
            &s_playback
        );

        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Failed to init decoder A: %s\n",
                    FLAC__StreamDecoderInitStatusString[status]);
            goto error_cleanup;
        }

        // Process metadata to get file info
        FLAC__stream_decoder_process_until_end_of_metadata(s_playback.decoder_a);
    }

    if (file_b) {
        strncpy(s_playback.info_b.filepath, file_b, sizeof(s_playback.info_b.filepath) - 1);

        s_playback.file_b = fopen(file_b, "rb");
        if (!s_playback.file_b) {
            fprintf(stderr, "[PLAYBACK] Failed to open file B: %s\n", file_b);
            goto error_cleanup;
        }

        s_playback.decoder_b = FLAC__stream_decoder_new();
        if (!s_playback.decoder_b) {
            fprintf(stderr, "[PLAYBACK] Failed to create decoder B\n");
            goto error_cleanup;
        }

        FLAC__stream_decoder_set_md5_checking(s_playback.decoder_b, false);

        FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_FILE(
            s_playback.decoder_b,
            s_playback.file_b,
            decoder_write_cb_b,
            decoder_metadata_cb_b,
            decoder_error_cb,
            &s_playback
        );

        if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
            fprintf(stderr, "[PLAYBACK] Failed to init decoder B: %s\n",
                    FLAC__StreamDecoderInitStatusString[status]);
            goto error_cleanup;
        }

        FLAC__stream_decoder_process_until_end_of_metadata(s_playback.decoder_b);
    }

    // Calculate total samples (max of both channels)
    s_playback.total_samples = s_playback.info_a.total_samples;
    if (s_playback.info_b.total_samples > s_playback.total_samples) {
        s_playback.total_samples = s_playback.info_b.total_samples;
    }

    // Reset app statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, PLAYBACK_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Initialize playback state
    atomic_store(&s_playback.state, PLAYBACK_STATE_PLAYING);
    atomic_store(&s_playback.speed, PLAYBACK_SPEED_1X);
    atomic_store(&s_playback.loop_enabled, false);
    atomic_store(&s_playback.seek_requested, false);
    atomic_store(&s_playback.current_sample, 0);
    atomic_store(&s_playback.running, true);

    // Start playback thread
    thrd_t thread;
    if (thrd_create(&thread, playback_thread_func, &s_playback) != thrd_success) {
        fprintf(stderr, "[PLAYBACK] Failed to create playback thread\n");
        goto error_cleanup;
    }
    s_playback.playback_thread = (void *)(uintptr_t)thread;

    app->is_capturing = true;
    gui_app_set_status(app, "Playback started");

    return 0;

error_cleanup:
    if (s_playback.decoder_a) {
        FLAC__stream_decoder_delete(s_playback.decoder_a);
        s_playback.decoder_a = NULL;
    }
    if (s_playback.decoder_b) {
        FLAC__stream_decoder_delete(s_playback.decoder_b);
        s_playback.decoder_b = NULL;
    }
    if (s_playback.file_a) {
        fclose(s_playback.file_a);
        s_playback.file_a = NULL;
    }
    if (s_playback.file_b) {
        fclose(s_playback.file_b);
        s_playback.file_b = NULL;
    }
    free(s_playback.decode_buf_a);
    free(s_playback.decode_buf_b);
    s_playback.decode_buf_a = NULL;
    s_playback.decode_buf_b = NULL;

    gui_app_set_status(app, "Playback failed to start");
    return -1;

#endif // LIBFLAC_ENABLED
}

void gui_playback_stop(gui_app_t *app) {
    if (!atomic_load(&s_playback.running)) return;

    fprintf(stderr, "[PLAYBACK] Stopping playback\n");

    atomic_store(&s_playback.running, false);
    atomic_store(&s_playback.state, PLAYBACK_STATE_STOPPED);

    if (s_playback.playback_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)s_playback.playback_thread;
        thrd_join(thread, NULL);
        s_playback.playback_thread = NULL;
    }

#if LIBFLAC_ENABLED == 1
    if (s_playback.decoder_a) {
        FLAC__stream_decoder_finish(s_playback.decoder_a);
        FLAC__stream_decoder_delete(s_playback.decoder_a);
        s_playback.decoder_a = NULL;
    }
    if (s_playback.decoder_b) {
        FLAC__stream_decoder_finish(s_playback.decoder_b);
        FLAC__stream_decoder_delete(s_playback.decoder_b);
        s_playback.decoder_b = NULL;
    }
#endif

    // Note: FILE handles are managed by FLAC decoder after init_FILE
    s_playback.file_a = NULL;
    s_playback.file_b = NULL;

    free(s_playback.decode_buf_a);
    free(s_playback.decode_buf_b);
    s_playback.decode_buf_a = NULL;
    s_playback.decode_buf_b = NULL;

    atomic_store(&app->stream_synced, false);
    app->is_capturing = false;

    gui_app_set_status(app, "Playback stopped");
}

bool gui_playback_is_running(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback.running);
}

playback_state_t gui_playback_get_state(gui_app_t *app) {
    (void)app;
    return (playback_state_t)atomic_load(&s_playback.state);
}

void gui_playback_pause(gui_app_t *app) {
    (void)app;
    if (atomic_load(&s_playback.state) == PLAYBACK_STATE_PLAYING) {
        atomic_store(&s_playback.state, PLAYBACK_STATE_PAUSED);
    }
}

void gui_playback_resume(gui_app_t *app) {
    (void)app;
    if (atomic_load(&s_playback.state) == PLAYBACK_STATE_PAUSED) {
        atomic_store(&s_playback.state, PLAYBACK_STATE_PLAYING);
    }
}

void gui_playback_toggle_pause(gui_app_t *app) {
    playback_state_t state = gui_playback_get_state(app);
    if (state == PLAYBACK_STATE_PLAYING) {
        gui_playback_pause(app);
    } else if (state == PLAYBACK_STATE_PAUSED) {
        gui_playback_resume(app);
    }
}

void gui_playback_seek_normalized(gui_app_t *app, double position) {
    (void)app;
    if (position < 0.0) position = 0.0;
    if (position > 1.0) position = 1.0;

    uint64_t target = (uint64_t)(position * s_playback.total_samples);
    atomic_store(&s_playback.seek_target, target);
    atomic_store(&s_playback.seek_requested, true);
}

void gui_playback_seek_sample(gui_app_t *app, uint64_t sample) {
    (void)app;
    if (sample > s_playback.total_samples) {
        sample = s_playback.total_samples;
    }
    atomic_store(&s_playback.seek_target, sample);
    atomic_store(&s_playback.seek_requested, true);
}

uint64_t gui_playback_get_position_samples(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback.current_sample);
}

double gui_playback_get_position_normalized(gui_app_t *app) {
    (void)app;
    if (s_playback.total_samples == 0) return 0.0;
    return (double)atomic_load(&s_playback.current_sample) / s_playback.total_samples;
}

double gui_playback_get_position_seconds(gui_app_t *app) {
    (void)app;
    uint64_t samples = atomic_load(&s_playback.current_sample);
    return (double)samples / PLAYBACK_SAMPLE_RATE;
}

uint64_t gui_playback_get_total_samples(gui_app_t *app) {
    (void)app;
    return s_playback.total_samples;
}

double gui_playback_get_duration_seconds(gui_app_t *app) {
    (void)app;
    return (double)s_playback.total_samples / PLAYBACK_SAMPLE_RATE;
}

void gui_playback_set_speed(gui_app_t *app, playback_speed_t speed) {
    (void)app;
    if (speed >= PLAYBACK_SPEED_COUNT) speed = PLAYBACK_SPEED_1X;
    atomic_store(&s_playback.speed, speed);
}

playback_speed_t gui_playback_get_speed(gui_app_t *app) {
    (void)app;
    return (playback_speed_t)atomic_load(&s_playback.speed);
}

bool gui_playback_get_file_info_a(gui_app_t *app, playback_file_info_t *info) {
    (void)app;
    if (!info) return false;
    *info = s_playback.info_a;
    return s_playback.info_a.valid;
}

bool gui_playback_get_file_info_b(gui_app_t *app, playback_file_info_t *info) {
    (void)app;
    if (!info) return false;
    *info = s_playback.info_b;
    return s_playback.info_b.valid;
}

void gui_playback_set_loop(gui_app_t *app, bool loop) {
    (void)app;
    atomic_store(&s_playback.loop_enabled, loop);
}

bool gui_playback_get_loop(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_playback.loop_enabled);
}
