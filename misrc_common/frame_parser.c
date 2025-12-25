/*
 * MISRC Common - Frame Parser Implementation
 *
 * Shared frame parsing and sync tracking for hsdaoh callbacks.
 */

#include "frame_parser.h"
#include <hsdaoh_crc.h>

#include <string.h>

#ifndef _WIN32
    #if defined(__APPLE__) || defined(__MACH__)
        #include <libkern/OSByteOrder.h>
        #define le16toh(x) OSSwapLittleToHostInt16(x)
        #define le32toh(x) OSSwapLittleToHostInt32(x)
    #elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        #include <sys/endian.h>
        #define le16toh(x) letoh16(x)
        #define le32toh(x) letoh32(x)
    #else
        #include <endian.h>
    #endif
#else
    /* Windows is always little-endian */
    #define le16toh(x) (x)
    #define le32toh(x) (x)
#endif

/*-----------------------------------------------------------------------------
 * Line Parsing
 *-----------------------------------------------------------------------------*/

parsed_line_t frame_parse_line(const uint8_t *line_dat, int width,
                                bool has_stream_id, bool has_crc)
{
    parsed_line_t result = {0};
    const uint16_t *words = (const uint16_t *)line_dat;

    /* Extract payload length from last word, apply 12-bit mask */
    result.payload_len = le16toh(words[width - 1]) & 0x0FFF;

    /* Extract CRC from second-to-last word if present */
    if (has_crc) {
        result.crc = le16toh(words[width - 2]);
    }

    /* Extract stream ID from third-to-last word if present */
    if (has_stream_id) {
        result.stream_id = le16toh(words[width - 3]) & 0x0FFF;
    }

    /* Validate payload length */
    result.valid = (result.payload_len <= (uint16_t)(width - 1));

    return result;
}

/*-----------------------------------------------------------------------------
 * Frame Sync Tracking
 *-----------------------------------------------------------------------------*/

void frame_sync_init(frame_sync_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

frame_sync_result_t frame_check_sync(frame_sync_state_t *state,
                                      uint16_t framecounter,
                                      unsigned int sync_threshold)
{
    /* Check for duplicate frame */
    if (framecounter == state->last_frame_cnt) {
        return FRAME_SYNC_DUPLICATE;
    }

    /* Check for missed frames */
    uint16_t expected = (state->last_frame_cnt + 1) & 0xFFFF;
    bool in_order = (framecounter == expected);

    if (in_order) {
        state->in_order_cnt++;
    } else {
        state->in_order_cnt = 0;
    }

    state->last_frame_cnt = framecounter;

    /* Check if we just acquired sync */
    if (!state->stream_synced && state->in_order_cnt > sync_threshold) {
        state->stream_synced = true;
        state->non_sync_cnt = 0;
        return FRAME_SYNC_ACQUIRED;
    }

    if (!in_order && state->stream_synced) {
        return FRAME_SYNC_MISSED;
    }

    return FRAME_SYNC_OK;
}

/*-----------------------------------------------------------------------------
 * CRC Validation
 *-----------------------------------------------------------------------------*/

void frame_crc_init(frame_crc_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

bool frame_check_crc(frame_crc_state_t *state, const uint8_t *line_dat,
                     int width, uint16_t received_crc, enum crc_config crc_config)
{
    if (crc_config == CRC_NONE) {
        return true;  /* No CRC checking */
    }

    uint16_t expected_crc;

    if (crc_config == CRC16_1_LINE) {
        expected_crc = state->last_crc[0];
    } else {
        /* CRC16_2_LINE */
        expected_crc = state->last_crc[1];
    }

    /* Update running CRC values */
    state->last_crc[1] = state->last_crc[0];
    state->last_crc[0] = crc16_ccitt(line_dat, width * sizeof(uint16_t));

    return (received_crc == expected_crc);
}

/*-----------------------------------------------------------------------------
 * Idle Count Validation
 *-----------------------------------------------------------------------------*/

void frame_idle_init(frame_idle_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

int frame_check_idle(frame_idle_state_t *state, const uint8_t *line_dat,
                     uint16_t payload_len, int width,
                     bool has_stream_id, bool has_crc)
{
    /* Calculate idle field length */
    uint16_t idle_len = (width - 1) - payload_len;
    if (has_stream_id) idle_len--;
    if (has_crc) idle_len--;

    /* Check idle counts using hsdaoh library function */
    const uint16_t *idle_start = (const uint16_t *)line_dat + payload_len;
    return hsdaoh_check_idle_cnt(&state->idle_cnt, (uint16_t *)idle_start, idle_len);
}

/*-----------------------------------------------------------------------------
 * Combined Frame Validation State
 *-----------------------------------------------------------------------------*/

void frame_parser_init(frame_parser_state_t *state)
{
    frame_sync_init(&state->sync);
    frame_crc_init(&state->crc);
    frame_idle_init(&state->idle);
    state->frames_since_error = 0;
}

/*-----------------------------------------------------------------------------
 * High-Level Frame Processing
 *-----------------------------------------------------------------------------*/

frame_process_result_t frame_process(frame_parser_state_t *state,
                                      const uint8_t *buf,
                                      unsigned int width,
                                      unsigned int height,
                                      const metadata_t *meta,
                                      unsigned int sync_threshold)
{
    frame_process_result_t result = {0};
    result.sync_result = FRAME_SYNC_OK;
    result.valid = false;
    result.report_errors = false;

    /* Validate magic number (need le32toh for big-endian systems) */
    if (le32toh(meta->magic) != HSDAOH_MAGIC) {
        state->sync.stream_synced = false;
        state->sync.in_order_cnt = 0;
        state->sync.non_sync_cnt++;
        result.sync_result = FRAME_SYNC_LOST;
        return result;
    }

    /* Check for duplicate frame */
    if (meta->framecounter == state->sync.last_frame_cnt) {
        result.sync_result = FRAME_SYNC_DUPLICATE;
        return result;
    }

    /* Check frame ordering */
    uint16_t expected = (state->sync.last_frame_cnt + 1) & 0xFFFF;
    bool in_order = (meta->framecounter == expected);

    if (in_order) {
        state->sync.in_order_cnt++;
    } else {
        state->sync.in_order_cnt = 0;
        if (state->sync.stream_synced) {
            result.sync_result = FRAME_SYNC_MISSED;
        }
    }

    state->sync.last_frame_cnt = meta->framecounter;

    /* Parse all lines - always parse to update CRC/idle state */
    bool has_stream_id = (meta->flags & FLAG_STREAM_ID_PRESENT) != 0;
    bool has_crc = (meta->crc_config != CRC_NONE);

    for (unsigned int line = 0; line < height; line++) {
        const uint8_t *line_dat = buf + (width * sizeof(uint16_t) * line);

        parsed_line_t parsed = frame_parse_line(line_dat, (int)width,
                                                 has_stream_id, has_crc);

        if (!parsed.valid) {
            /* Invalid payload length - frame is corrupt */
            if (!state->sync.stream_synced) {
                state->sync.non_sync_cnt++;
            }
            return result;
        }

        /* Check idle counts */
        result.error_count += frame_check_idle(&state->idle, line_dat,
                                                parsed.payload_len, (int)width,
                                                has_stream_id, has_crc);

        /* Check CRC if enabled - always update state, only count errors when synced */
        if (has_crc) {
            uint16_t expected_crc = (meta->crc_config == CRC16_1_LINE)
                                    ? state->crc.last_crc[0]
                                    : state->crc.last_crc[1];

            if (parsed.crc != expected_crc && state->sync.stream_synced) {
                result.error_count++;
            }

            /* Always update CRC state */
            state->crc.last_crc[1] = state->crc.last_crc[0];
            state->crc.last_crc[0] = crc16_ccitt(line_dat, width * sizeof(uint16_t));
        }

        /* Accumulate payload sizes by stream (only when synced) */
        if (parsed.payload_len > 0 && state->sync.stream_synced) {
            if (parsed.stream_id == 0) {
                result.stream0_bytes += parsed.payload_len * sizeof(uint16_t);
            } else if (parsed.stream_id == 1) {
                result.stream1_bytes += parsed.payload_len * sizeof(uint16_t);
            }
        }
    }

    /* Handle errors - only count when synced */
    if (result.error_count > 0 && state->sync.stream_synced) {
        result.report_errors = true;
        state->frames_since_error = 0;
        return result;  /* Frame has errors, don't mark as valid */
    }

    state->frames_since_error++;

    /* Check for sync acquisition (after processing frame, like CLI) */
    if (!state->sync.stream_synced && result.error_count == 0 &&
        state->sync.in_order_cnt > sync_threshold) {
        state->sync.stream_synced = true;
        state->sync.non_sync_cnt = 0;
        result.sync_result = FRAME_SYNC_ACQUIRED;
        /* Don't set valid yet - first synced frame is for priming */
        return result;
    }

    if (state->sync.stream_synced) {
        result.valid = true;
    }

    return result;
}

size_t frame_copy_payloads_cb(const uint8_t *buf,
                               unsigned int width,
                               unsigned int height,
                               const metadata_t *meta,
                               uint8_t *out_stream0,
                               uint8_t *out_stream1,
                               frame_payload_cb_t callback,
                               void *ctx)
{
    bool has_stream_id = (meta->flags & FLAG_STREAM_ID_PRESENT) != 0;
    bool has_crc = (meta->crc_config != CRC_NONE);

    size_t offset0 = 0;
    size_t offset1 = 0;

    for (unsigned int line = 0; line < height; line++) {
        const uint8_t *line_dat = buf + (width * sizeof(uint16_t) * line);

        parsed_line_t parsed = frame_parse_line(line_dat, (int)width,
                                                 has_stream_id, has_crc);

        if (!parsed.valid || parsed.payload_len == 0) {
            continue;
        }

        size_t payload_bytes = parsed.payload_len * sizeof(uint16_t);

        /* Call filter callback if provided */
        if (callback && !callback(ctx, parsed.stream_id, line_dat, payload_bytes)) {
            continue;
        }

        if (parsed.stream_id == 0 && out_stream0) {
            memcpy(out_stream0 + offset0, line_dat, payload_bytes);
            offset0 += payload_bytes;
        } else if (parsed.stream_id == 1 && out_stream1) {
            memcpy(out_stream1 + offset1, line_dat, payload_bytes);
            offset1 += payload_bytes;
        }
    }

    return offset0 + offset1;
}

size_t frame_copy_payloads(const uint8_t *buf,
                           unsigned int width,
                           unsigned int height,
                           const metadata_t *meta,
                           uint8_t *out_stream0,
                           uint8_t *out_stream1)
{
    return frame_copy_payloads_cb(buf, width, height, meta,
                                   out_stream0, out_stream1, NULL, NULL);
}
