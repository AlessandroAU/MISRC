/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Uses software PLL for H-sync tracking and provides frame buffer display.
 */

#include "gui_cvbs.h"
#include "gui_trigger.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

//-----------------------------------------------------------------------------
// Internal Constants
//-----------------------------------------------------------------------------

// Back porch offset (after H-sync, before active video)
#define BACK_PORCH_SAMPLES      228  // ~7µs - slightly more than standard to skip color burst

// Field detection constants
#define PAL_FIELD_LINES         312  // Lines per PAL field (312.5 rounded)
#define NTSC_FIELD_LINES        262  // Lines per NTSC field (262.5 rounded)
#define PAL_ACTIVE_START        23   // First active line in PAL field
#define NTSC_ACTIVE_START       21   // First active line in NTSC field

// Field heights (half of full frame)
#define PAL_FIELD_HEIGHT        288  // 576/2
#define NTSC_FIELD_HEIGHT       243  // 486/2

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

// Luma lowpass filter to remove chroma subcarrier
// At 40 MSPS: NTSC color burst = 3.58 MHz (~11 samples/cycle)
//             PAL color burst = 4.43 MHz (~9 samples/cycle)
// We use a simple box filter averaging over ~1 chroma cycle to null it out
#define LUMA_LPF_TAPS  11  // Average over ~11 samples (one NTSC chroma cycle)

// Decode a single video line from samples to grayscale pixels
static void decode_line_to_pixels(const int16_t *samples, size_t sample_count,
                                  const cvbs_levels_t *levels,
                                  uint8_t *pixels, int pixel_width) {
    if (!samples || !levels || !pixels || pixel_width <= 0) return;
    if (sample_count < (size_t)(BACK_PORCH_SAMPLES + pixel_width)) return;

    // Skip back porch to get to active video
    const int16_t *active_start = samples + BACK_PORCH_SAMPLES;
    size_t active_samples = sample_count - BACK_PORCH_SAMPLES;

    // Limit to expected active region
    if (active_samples > CVBS_ACTIVE_SAMPLES) {
        active_samples = CVBS_ACTIVE_SAMPLES;
    }

    // Calculate scale factor for sample-to-pixel mapping
    float samples_per_pixel = (float)active_samples / pixel_width;
    float level_range = (float)(levels->white_level - levels->black_level);
    if (level_range < 1.0f) level_range = 1.0f;

    // Half the filter width for symmetric averaging
    int half_taps = LUMA_LPF_TAPS / 2;

    // Decode each pixel with lowpass filtering to remove chroma
    for (int px = 0; px < pixel_width; px++) {
        // Find corresponding sample position
        size_t sample_pos = (size_t)(px * samples_per_pixel);
        if (sample_pos >= active_samples) sample_pos = active_samples - 1;

        // Apply box filter: average samples centered on this position
        // This nulls out the chroma subcarrier (averaging over one cycle)
        int32_t sum = 0;
        int count = 0;
        for (int t = -half_taps; t <= half_taps; t++) {
            int idx = (int)sample_pos + t;
            if (idx >= 0 && idx < (int)active_samples) {
                sum += active_start[idx];
                count++;
            }
        }

        // Get filtered sample value
        float filtered = (count > 0) ? (float)sum / count : (float)active_start[sample_pos];

        // Clamp to black-white range and normalize
        float normalized = (filtered - (float)levels->black_level) / level_range;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        pixels[px] = (uint8_t)(normalized * 255.0f);
    }
}

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

bool gui_cvbs_init(cvbs_decoder_t *decoder) {
    if (!decoder) return false;

    memset(decoder, 0, sizeof(cvbs_decoder_t));

    // Allocate field buffers (each field is half the frame height)
    // Max field height is PAL: 288 lines
    decoder->field_height = PAL_FIELD_HEIGHT;
    size_t field_size = (size_t)CVBS_FRAME_WIDTH * (size_t)(CVBS_MAX_HEIGHT / 2);

    decoder->field_buffer[0] = (uint8_t *)calloc(field_size, 1);
    if (!decoder->field_buffer[0]) {
        return false;
    }

    decoder->field_buffer[1] = (uint8_t *)calloc(field_size, 1);
    if (!decoder->field_buffer[1]) {
        free(decoder->field_buffer[0]);
        decoder->field_buffer[0] = NULL;
        return false;
    }

    // Allocate deinterlaced frame buffer at full-frame resolution (D1)
    decoder->frame_width = CVBS_FRAME_WIDTH;
    decoder->frame_height = CVBS_PAL_HEIGHT;  // Start with PAL full-frame height
    decoder->frame_buffer = (uint8_t *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, 1);
    if (!decoder->frame_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        return false;
    }

    // Allocate display buffer (double buffering)
    decoder->display_buffer = (uint8_t *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, 1);
    if (!decoder->display_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        return false;
    }

    // Create raylib Image for video display (RGBA format at full-frame resolution)
    // Allocate our own pixel buffer so we control the memory
    Color *frame_pixels = (Color *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, sizeof(Color));
    if (!frame_pixels) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        return false;
    }
    decoder->frame_image.data = frame_pixels;
    decoder->frame_image.width = CVBS_FRAME_WIDTH;
    decoder->frame_image.height = CVBS_MAX_HEIGHT;
    decoder->frame_image.mipmaps = 1;
    decoder->frame_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    decoder->texture_valid = false;

    // Allocate line buffer for cross-boundary line assembly
    decoder->line_buffer = (int16_t *)calloc(CVBS_LINE_BUFFER_SIZE, sizeof(int16_t));
    if (!decoder->line_buffer) {
        free(decoder->field_buffer[0]);
        free(decoder->field_buffer[1]);
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        free(decoder->frame_image.data);
        decoder->field_buffer[0] = NULL;
        decoder->field_buffer[1] = NULL;
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        decoder->frame_image.data = NULL;
        return false;
    }
    decoder->line_buffer_count = 0;

    // Initialize adaptive levels
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));

    // Initialize software PLL with PAL default
    decoder->pll.phase = 0;
    decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    decoder->pll.freq_adjust = 0;
    decoder->pll.phase_error = 0;
    decoder->pll.phase_integral = 0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->pll.current_line = 0;
    decoder->pll.samples_in_line = 0;
    decoder->pll.total_samples = 0;

    // Initialize lowpass filter state
    decoder->lpf_state = 0.0;
    decoder->lpf_output = 0.0;

    // Initialize edge detection state (uses filtered signal)
    decoder->last_filtered_above = true;
    decoder->global_sample_pos = 0;

    // Initialize H-sync pulse tracking
    decoder->in_hsync_pulse = false;
    decoder->hsync_pulse_start = 0;

    // Initialize V-sync detector state
    decoder->vsync_last_edge_pos = 0;

    // Initialize V-sync detection state
    memset(&decoder->vsync, 0, sizeof(decoder->vsync));

    // Initialize legacy state
    decoder->lines_since_vsync = 0;

    // Initialize frame state
    decoder->state.format = CVBS_FORMAT_UNKNOWN;
    decoder->state.total_lines = 0;
    decoder->state.active_lines = 0;
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    decoder->state.frames_decoded = 0;

    return true;
}

void gui_cvbs_cleanup(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    if (decoder->texture_valid) {
        UnloadTexture(decoder->frame_texture);
        decoder->texture_valid = false;
    }

    // Free frame image data (we allocated it ourselves)
    if (decoder->frame_image.data) {
        free(decoder->frame_image.data);
        decoder->frame_image.data = NULL;
    }

    free(decoder->field_buffer[0]);
    free(decoder->field_buffer[1]);
    free(decoder->frame_buffer);
    free(decoder->display_buffer);
    free(decoder->line_buffer);

    decoder->field_buffer[0] = NULL;
    decoder->field_buffer[1] = NULL;
    decoder->frame_buffer = NULL;
    decoder->display_buffer = NULL;
    decoder->line_buffer = NULL;
}

void gui_cvbs_reset(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Clear field buffers
    size_t field_size = (size_t)CVBS_FRAME_WIDTH * (size_t)(CVBS_MAX_HEIGHT / 2);
    if (decoder->field_buffer[0]) {
        memset(decoder->field_buffer[0], 0, field_size);
    }
    if (decoder->field_buffer[1]) {
        memset(decoder->field_buffer[1], 0, field_size);
    }

    // Clear frame buffers
    if (decoder->frame_buffer) {
        memset(decoder->frame_buffer, 0, CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT);
    }
    if (decoder->display_buffer) {
        memset(decoder->display_buffer, 0, CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT);
    }

    // Reset line buffer
    decoder->line_buffer_count = 0;

    // Reset adaptive levels
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));

    // Reset software PLL (keep line period consistent with selected system)
    decoder->pll.phase = 0;
    if (decoder->state.format == CVBS_FORMAT_NTSC) {
        decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_NTSC;
    } else {
        // PAL + SECAM + UNKNOWN use PAL timing
        decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    }
    decoder->pll.freq_adjust = 0;
    decoder->pll.phase_error = 0;
    decoder->pll.phase_integral = 0;
    decoder->pll.good_sync_count = 0;
    decoder->pll.bad_sync_count = 0;
    decoder->pll.locked = false;
    decoder->pll.current_line = 0;
    decoder->pll.samples_in_line = 0;
    // Don't reset total_samples - keep for debug

    // Reset lowpass filter state
    decoder->lpf_state = 0.0;
    decoder->lpf_output = 0.0;

    // Reset edge detection state (uses filtered signal)
    decoder->last_filtered_above = true;
    decoder->global_sample_pos = 0;

    // Reset H-sync pulse tracking
    decoder->in_hsync_pulse = false;
    decoder->hsync_pulse_start = 0;

    // Reset V-sync detector state
    decoder->vsync_last_edge_pos = 0;

    // Reset V-sync detection state
    memset(&decoder->vsync, 0, sizeof(decoder->vsync));

    // Reset legacy state
    decoder->lines_since_vsync = 0;

    // Reset frame state (preserve decoder->state.format set by gui_cvbs_set_format)
    decoder->state.current_line = 0;
    decoder->state.current_field = 0;
    decoder->state.in_vsync = false;
    decoder->state.frame_complete = false;
    decoder->display_ready = false;

    // Reset statistics
    decoder->sync_errors = 0;
    decoder->format_changes = 0;

    // Reset debug statistics
    memset(&decoder->debug, 0, sizeof(decoder->debug));
}

//-----------------------------------------------------------------------------
// Adaptive Level Estimation
//-----------------------------------------------------------------------------

// Update min/max from a filtered sample (called from main processing loop)
// Uses the already-filtered signal from sync detection to reject noise
static inline void accumulate_filtered_level(cvbs_decoder_t *decoder, int16_t filtered) {
    if (decoder->adaptive.field_min == 0 && decoder->adaptive.field_max == 0) {
        // First sample of field
        decoder->adaptive.field_min = filtered;
        decoder->adaptive.field_max = filtered;
    } else {
        if (filtered < decoder->adaptive.field_min)
            decoder->adaptive.field_min = filtered;
        if (filtered > decoder->adaptive.field_max)
            decoder->adaptive.field_max = filtered;
    }
}

// Commit accumulated levels at end of field (called from start_new_field_pll)
static void commit_adaptive_levels(cvbs_decoder_t *decoder) {
    // Only update if we accumulated valid data
    if (decoder->adaptive.field_min == 0 && decoder->adaptive.field_max == 0) return;

    int16_t field_min = decoder->adaptive.field_min;
    int16_t field_max = decoder->adaptive.field_max;

    // Reset accumulators for next field
    decoder->adaptive.field_min = 0;
    decoder->adaptive.field_max = 0;

    // Exponential moving average for stability (update once per field)
    const float alpha = 0.1f;  // ~10 fields to converge (~200ms)

    if (decoder->adaptive.sync_tip == 0 && decoder->adaptive.white == 0) {
        // First time - initialize directly
        decoder->adaptive.sync_tip = field_min;
        decoder->adaptive.white = field_max;
    } else {
        // Smooth update
        decoder->adaptive.sync_tip = (int16_t)(decoder->adaptive.sync_tip * (1.0f - alpha) +
                                               field_min * alpha);
        decoder->adaptive.white = (int16_t)(decoder->adaptive.white * (1.0f - alpha) +
                                            field_max * alpha);
    }

    // Derive other levels
    int16_t range = decoder->adaptive.white - decoder->adaptive.sync_tip;
    if (range < 100) range = 100;  // Minimum range to avoid division issues

    // For CVBS, the sync tip is at IRE -40, blanking at IRE 0, black at IRE ~7.5, white at IRE 100
    // So sync is about 40/140 = 28.5% of the range below blanking
    // Threshold at 25% above sync tip (matching gui_trigger.h CVBS_SYNC_MARGIN)
    // This is well into the sync pulse region for reliable edge detection
    decoder->adaptive.threshold = decoder->adaptive.sync_tip + (int16_t)(range * CVBS_SYNC_MARGIN);

    // Blanking level: ~28% of range (above sync, at black level start)
    decoder->adaptive.blanking = decoder->adaptive.sync_tip + (int16_t)(range * 0.28f);

    // Black level: ~32% of range (just above blanking)
    decoder->adaptive.black = decoder->adaptive.sync_tip + (int16_t)(range * 0.32f);

    // Update legacy levels for compatibility
    decoder->levels.sig_min = decoder->adaptive.sync_tip;
    decoder->levels.sig_max = decoder->adaptive.white;
    decoder->levels.range = range;
    decoder->levels.sync_threshold = decoder->adaptive.threshold;
    decoder->levels.black_level = decoder->adaptive.black;
    decoder->levels.white_level = decoder->adaptive.white;
}

//-----------------------------------------------------------------------------
// Software PLL-based CVBS Decoder
//
// This approach uses a software PLL to track H-sync timing:
// - PLL maintains a phase counter that represents position within a line
// - When H-sync edges are detected, PLL phase is corrected
// - Samples are written to frame buffer based on PLL line counter
// - V-sync detection resets the line counter to start a new field
// - Missing H-syncs don't break the display - PLL interpolates
//-----------------------------------------------------------------------------

// PLL tuning constants
#define PLL_PHASE_GAIN      0.15    // Proportional gain for phase correction
#define PLL_INTEGRAL_GAIN   0.005   // Integral gain for frequency drift
#define PLL_LOCK_THRESHOLD  100     // Phase error below this = good sync
#define PLL_LOCK_COUNT      10      // Good syncs needed to declare lock
#define PLL_UNLOCK_COUNT    5       // Bad syncs to lose lock

// H-sync pulse validation (aligned with gui_trigger.h constants)
#define HSYNC_MIN_WIDTH     CVBS_HSYNC_MIN_WIDTH  // 100 samples (~2.5µs minimum)
#define HSYNC_MAX_WIDTH     CVBS_HSYNC_MAX_WIDTH  // 280 samples (~7µs maximum)

// Lowpass filter coefficient (IIR single-pole)
// At 40 MSPS, a cutoff of ~500kHz gives alpha ≈ 0.08
// Lower alpha = more smoothing (removes HF noise while preserving sync edges)
#define LPF_ALPHA           0.08

//-----------------------------------------------------------------------------
// Lowpass Filter for Sync Detection
//-----------------------------------------------------------------------------

// Apply IIR lowpass filter to a sample
// This smooths out high-frequency noise while preserving sync pulse edges
static inline double apply_lowpass(cvbs_decoder_t *decoder, int16_t sample) {
    // Single-pole IIR: y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
    decoder->lpf_state = LPF_ALPHA * (double)sample + (1.0 - LPF_ALPHA) * decoder->lpf_state;
    decoder->lpf_output = decoder->lpf_state;
    return decoder->lpf_output;
}

//-----------------------------------------------------------------------------
// Separate H-sync and V-sync Detectors
//-----------------------------------------------------------------------------

// V-sync detector result
typedef struct {
    bool edge_detected;        // True if falling edge detected
    size_t interval;           // Samples since last falling edge
    bool vsync_complete;       // True if V-sync sequence completed
} vsync_result_t;

// H-sync detector result
typedef struct {
    bool pulse_complete;       // True if a valid H-sync pulse ended
    size_t pulse_width;        // Width of the completed pulse
    double phase_at_sync;      // PLL phase when sync was detected
} hsync_result_t;

// Detect V-sync by tracking falling edge intervals
// V-sync region has half-line rate pulses (equalizing + serration)
// Returns vsync_complete=true when V-sync sequence ends
static vsync_result_t detect_vsync(cvbs_decoder_t *decoder, double filtered,
                                    int16_t threshold, size_t sample_pos) {
    vsync_result_t result = {false, 0, false};
    bool is_above = (filtered > threshold);

    // Check for falling edge (signal goes below threshold)
    if (decoder->last_filtered_above && !is_above) {
        result.edge_detected = true;
        result.interval = sample_pos - decoder->vsync_last_edge_pos;
        decoder->vsync_last_edge_pos = sample_pos;

        // Check interval for V-sync detection
        cvbs_vsync_state_t *vs = &decoder->vsync;

        // Half-line interval is ~1280 samples (half of 2560)
        // Allow generous tolerance: 1000-1600 samples
        bool is_half_line = (result.interval >= 1000 && result.interval <= 1600);

        if (is_half_line) {
            vs->half_line_count++;
            // V-sync region has multiple half-line pulses
            if (vs->half_line_count >= 6 && !vs->in_vsync) {
                vs->in_vsync = true;
            }
        } else {
            // Full line interval - if we were in V-sync, it's ending
            if (vs->in_vsync && result.interval >= 2200 && result.interval <= 3000) {
                vs->in_vsync = false;
                vs->half_line_count = 0;
                result.vsync_complete = true;
            }
            vs->half_line_count = 0;
        }
    }

    return result;
}

// Detect H-sync by measuring pulse width
// Only valid H-sync pulses (100-400 samples) are reported
// Returns pulse_complete=true when a valid H-sync pulse ends
static hsync_result_t detect_hsync(cvbs_decoder_t *decoder, double filtered,
                                    int16_t threshold, size_t sample_pos,
                                    double current_pll_phase) {
    hsync_result_t result = {false, 0, 0.0};
    bool is_above = (filtered > threshold);

    // Check for falling edge - start of potential H-sync pulse
    if (decoder->last_filtered_above && !is_above) {
        decoder->in_hsync_pulse = true;
        decoder->hsync_pulse_start = sample_pos;
    }

    // Check for rising edge - end of pulse
    if (!decoder->last_filtered_above && is_above && decoder->in_hsync_pulse) {
        decoder->in_hsync_pulse = false;
        size_t pulse_width = sample_pos - decoder->hsync_pulse_start;

        // Only accept pulses in H-sync range (100-400 samples)
        if (pulse_width >= HSYNC_MIN_WIDTH && pulse_width <= HSYNC_MAX_WIDTH) {
            result.pulse_complete = true;
            result.pulse_width = pulse_width;
            result.phase_at_sync = current_pll_phase;
        }
    }

    return result;
}

//-----------------------------------------------------------------------------
// Deinterlacer
//-----------------------------------------------------------------------------

// Deinterlace two fields into a full frame using weave with bob fallback
// - Weave: interleave even/odd fields (best quality when both present)
// - Bob: interpolate missing field lines (for partial frames)
static void deinterlace_fields(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->field_buffer[0] || !decoder->field_buffer[1]) return;
    if (!decoder->frame_buffer) return;

    int field_height = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                       NTSC_FIELD_HEIGHT : PAL_FIELD_HEIGHT;
    int frame_height = field_height * 2;

    // Clamp to buffer limits
    if (frame_height > decoder->frame_height) {
        frame_height = decoder->frame_height;
        field_height = frame_height / 2;
    }

    uint8_t *field0 = decoder->field_buffer[0];  // Even/first field (lines 0, 2, 4...)
    uint8_t *field1 = decoder->field_buffer[1];  // Odd/second field (lines 1, 3, 5...)
    uint8_t *frame = decoder->frame_buffer;

    // Check which fields are valid
    bool have_field0 = decoder->field_ready[0];
    bool have_field1 = decoder->field_ready[1];

    if (have_field0 && have_field1) {
        // Weave mode: interleave both fields for full vertical resolution
        for (int fl = 0; fl < field_height; fl++) {
            uint8_t *src0 = field0 + (size_t)fl * CVBS_FRAME_WIDTH;
            uint8_t *src1 = field1 + (size_t)fl * CVBS_FRAME_WIDTH;

            // Even lines from field 0, odd lines from field 1
            uint8_t *dst_even = frame + (size_t)(fl * 2) * CVBS_FRAME_WIDTH;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * CVBS_FRAME_WIDTH;

            memcpy(dst_even, src0, CVBS_FRAME_WIDTH);
            memcpy(dst_odd, src1, CVBS_FRAME_WIDTH);
        }
    } else if (have_field0) {
        // Bob mode with field 0 only: duplicate lines with slight blur
        for (int fl = 0; fl < field_height; fl++) {
            uint8_t *src = field0 + (size_t)fl * CVBS_FRAME_WIDTH;
            uint8_t *dst_even = frame + (size_t)(fl * 2) * CVBS_FRAME_WIDTH;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * CVBS_FRAME_WIDTH;

            memcpy(dst_even, src, CVBS_FRAME_WIDTH);

            // Interpolate odd line from adjacent even lines
            if (fl < field_height - 1) {
                uint8_t *src_next = field0 + (size_t)(fl + 1) * CVBS_FRAME_WIDTH;
                for (int x = 0; x < CVBS_FRAME_WIDTH; x++) {
                    dst_odd[x] = (uint8_t)(((int)src[x] + (int)src_next[x]) / 2);
                }
            } else {
                // Last line - just duplicate
                memcpy(dst_odd, src, CVBS_FRAME_WIDTH);
            }
        }
    } else if (have_field1) {
        // Bob mode with field 1 only
        for (int fl = 0; fl < field_height; fl++) {
            uint8_t *src = field1 + (size_t)fl * CVBS_FRAME_WIDTH;
            uint8_t *dst_even = frame + (size_t)(fl * 2) * CVBS_FRAME_WIDTH;
            uint8_t *dst_odd = frame + (size_t)(fl * 2 + 1) * CVBS_FRAME_WIDTH;

            memcpy(dst_odd, src, CVBS_FRAME_WIDTH);

            // Interpolate even line from adjacent odd lines
            if (fl > 0) {
                uint8_t *src_prev = field1 + (size_t)(fl - 1) * CVBS_FRAME_WIDTH;
                for (int x = 0; x < CVBS_FRAME_WIDTH; x++) {
                    dst_even[x] = (uint8_t)(((int)src_prev[x] + (int)src[x]) / 2);
                }
            } else {
                // First line - just duplicate
                memcpy(dst_even, src, CVBS_FRAME_WIDTH);
            }
        }
    }
    // If neither field ready, frame_buffer keeps its previous content
}

//-----------------------------------------------------------------------------
// Field Completion
//-----------------------------------------------------------------------------

// Complete a field - deinterlace and copy to display buffer
static void complete_field_pll(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    int line_count = decoder->pll.current_line;

    // Get expected field parameters
    int expected_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                         NTSC_FIELD_LINES : PAL_FIELD_LINES;

    // Only accept if we got a reasonable field (at least 50% of expected)
    if (line_count < (expected_lines / 2)) {
        return;
    }

    // Mark this field as received
    int field_idx = decoder->state.current_field ? 1 : 0;
    decoder->field_ready[field_idx] = true;

    // Deinterlace fields into frame buffer
    // Note: field_ready flags persist so weave mode works on every field after
    // the first two fields are received (not just every other frame)
    deinterlace_fields(decoder);

    // Copy deinterlaced frame to display buffer
    int frame_h = decoder->frame_height;
    if (frame_h > CVBS_MAX_HEIGHT) frame_h = CVBS_MAX_HEIGHT;

    memcpy(decoder->display_buffer, decoder->frame_buffer,
           (size_t)CVBS_FRAME_WIDTH * (size_t)frame_h);
    decoder->display_ready = true;

    // Count full frames when we have both fields
    if (decoder->field_ready[0] && decoder->field_ready[1]) {
        decoder->state.frame_complete = true;
        decoder->state.frames_decoded++;
        // Don't clear field_ready - keep both marked so weave mode continues
        // Fields will be overwritten in-place by subsequent decoding
    }

    decoder->debug.fields_decoded++;
    decoder->debug.lines_decoded_last = line_count;
}

// Start a new field after V-sync
static void start_new_field_pll(cvbs_decoder_t *decoder) {
    // Complete previous field
    complete_field_pll(decoder);

    // Commit accumulated levels from completed field (before starting new one)
    // This ensures levels only update at field boundaries, eliminating shimmer
    commit_adaptive_levels(decoder);

    // Reset line counter for new field, but DON'T reset PLL phase
    // The PLL should continue tracking smoothly - resetting phase causes
    // the first ~60 lines to wobble as PLL re-locks after V-sync
    decoder->pll.current_line = 0;
    // decoder->pll.phase = 0;  // Don't reset - let PLL free-run through V-sync

    // Alternate fields for interlacing
    decoder->state.current_field = 1 - decoder->state.current_field;
    decoder->lines_since_vsync = 0;

    // Reset hsync counter for new field's debug stats
    decoder->debug.hsyncs_last_field = 0;
    decoder->debug.hsyncs_expected = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                                      NTSC_FIELD_LINES : PAL_FIELD_LINES;

    decoder->debug.vsync_found++;
}

// Process a detected H-sync edge - update PLL
static void pll_process_hsync(cvbs_decoder_t *decoder, double phase_at_sync) {
    cvbs_pll_state_t *pll = &decoder->pll;

    // Calculate phase error: how far off was our prediction?
    // Ideal sync should happen at phase = 0 (or line_period)
    double phase_error = phase_at_sync;

    // Wrap to -half_period to +half_period
    if (phase_error > pll->line_period / 2) {
        phase_error -= pll->line_period;
    }

    // Update PLL lock status based on phase error magnitude
    if (fabs(phase_error) < PLL_LOCK_THRESHOLD) {
        pll->good_sync_count++;
        pll->bad_sync_count = 0;
        if (pll->good_sync_count >= PLL_LOCK_COUNT) {
            pll->locked = true;
        }
    } else {
        pll->bad_sync_count++;
        pll->good_sync_count = 0;
        if (pll->bad_sync_count >= PLL_UNLOCK_COUNT) {
            pll->locked = false;
        }
    }

    // Reject obviously bad syncs (too far off)
    if (fabs(phase_error) > pll->line_period * 0.4) {
        // This sync is way off - probably noise or V-sync region
        // Don't adjust PLL, just increment line counter if phase wrapped
        return;
    }

    // Apply phase correction (proportional)
    pll->phase -= phase_error * PLL_PHASE_GAIN;

    // Accumulate for integral term (frequency drift correction)
    pll->phase_integral += phase_error * PLL_INTEGRAL_GAIN;

    // Limit integral term to prevent runaway
    if (pll->phase_integral > 50) pll->phase_integral = 50;
    if (pll->phase_integral < -50) pll->phase_integral = -50;

    // Apply integral correction to frequency
    pll->freq_adjust = pll->phase_integral;

    // Store for derivative term (not currently used)
    pll->phase_error = phase_error;

    decoder->debug.hsyncs_last_field++;
}

// Decode current line from line buffer to field buffer
static void decode_current_line(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->line_buffer) return;

    cvbs_pll_state_t *pll = &decoder->pll;
    int line_num = pll->current_line;

    // Get field parameters
    int active_start = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                       NTSC_ACTIVE_START : PAL_ACTIVE_START;
    int max_field_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                          NTSC_FIELD_HEIGHT : PAL_FIELD_HEIGHT;

    // Only decode active video lines (skip VBI)
    if (line_num >= active_start && line_num < active_start + max_field_lines) {
        int field_line = line_num - active_start;

        // Write to the appropriate field buffer (not directly to frame)
        int field_idx = decoder->state.current_field ? 1 : 0;
        uint8_t *field_buf = decoder->field_buffer[field_idx];

        if (field_buf && field_line >= 0 && field_line < max_field_lines) {
            uint8_t *row_ptr = field_buf + ((size_t)field_line * (size_t)CVBS_FRAME_WIDTH);

            // Use samples from line buffer
            int samples_available = decoder->line_buffer_count;
            if (samples_available > 100) {  // Need minimum samples
                decode_line_to_pixels(decoder->line_buffer, samples_available,
                                     &decoder->levels, row_ptr, CVBS_FRAME_WIDTH);
            }
        }
    }

    // Clear line buffer for next line
    decoder->line_buffer_count = 0;
}

// Debug counter for periodic logging
static int s_debug_counter = 0;

void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count) {
    if (!decoder || !buf || count < 100) return;
    if (!decoder->line_buffer) return;

    // Track incoming data
    decoder->debug.buffers_received++;
    decoder->debug.samples_received += count;

    // Check for minimum signal strength (skip check until first V-sync commits levels)
    if (decoder->levels.range < 100 && decoder->debug.vsync_found > 0) {
        decoder->sync_errors++;
        return;
    }

    int16_t threshold = decoder->adaptive.threshold;
    cvbs_pll_state_t *pll = &decoder->pll;

    // Process each sample
    for (size_t i = 0; i < count; i++) {
        int16_t sample = buf[i];

        // Apply lowpass filter for cleaner sync detection
        double filtered = apply_lowpass(decoder, sample);

        // Accumulate min/max from filtered signal (every 16th sample for efficiency)
        // This reuses the sync detection filter to reject noise from level estimates
        if ((i & 0xF) == 0) {
            accumulate_filtered_level(decoder, (int16_t)filtered);
        }

        // Store UNFILTERED sample in line buffer (for video decoding)
        // We want full bandwidth for the video, just filtered for sync detect
        if (decoder->line_buffer_count < CVBS_LINE_BUFFER_SIZE) {
            decoder->line_buffer[decoder->line_buffer_count++] = sample;
        }

        // Advance PLL phase
        pll->phase += 1.0;
        pll->samples_in_line++;
        pll->total_samples++;
        decoder->global_sample_pos++;

        // Run V-sync detector on filtered signal
        vsync_result_t vr = detect_vsync(decoder, filtered, threshold,
                                          decoder->global_sample_pos);
        if (vr.vsync_complete) {
            // V-sync detected - start new field
            start_new_field_pll(decoder);
        }

        // Run H-sync detector on filtered signal
        hsync_result_t hr = detect_hsync(decoder, filtered, threshold,
                                          decoder->global_sample_pos, pll->phase);
        if (hr.pulse_complete) {
            // Valid H-sync pulse detected - update PLL
            pll_process_hsync(decoder, hr.phase_at_sync);
        }

        // Update filtered edge state for next iteration
        decoder->last_filtered_above = (filtered > threshold);

        // Calculate effective period with current freq_adjust (updated by PLL each H-sync)
        double effective_period = pll->line_period + pll->freq_adjust;

        // Check if PLL indicates line complete (phase >= line_period)
        if (pll->phase >= effective_period) {
            // Line complete - decode it
            decode_current_line(decoder);

            // Advance to next line
            pll->current_line++;
            pll->phase -= effective_period;  // Keep fractional phase
            pll->samples_in_line = 0;
            decoder->lines_since_vsync++;

            // Check for field overflow (shouldn't happen with proper V-sync)
            int max_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                           NTSC_FIELD_LINES + 20 : PAL_FIELD_LINES + 20;
            if (pll->current_line > max_lines) {
                // Too many lines - force new field
                start_new_field_pll(decoder);
            }
        }
    }

    // Debug logging periodically (every ~10 seconds at 50 fields/sec)
    s_debug_counter++;
    if (s_debug_counter % 500 == 0) {
        fprintf(stderr, "[CVBS-PLL] fields=%d frames=%d line=%d phase=%.0f "
                        "locked=%d hsyncs=%d/%d freq_adj=%.2f\n",
                decoder->debug.fields_decoded,
                decoder->state.frames_decoded,
                pll->current_line,
                pll->phase,
                pll->locked ? 1 : 0,
                decoder->debug.hsyncs_last_field,
                decoder->debug.hsyncs_expected,
                pll->freq_adjust);
    }
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height) {
    if (!decoder || !decoder->display_ready) {
        // No frame available - draw placeholder
        DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){20, 20, 30, 255});
        DrawText("No Signal", (int)(x + width/2 - 40), (int)(y + height/2 - 10),
                 20, (Color){100, 100, 100, 255});
        return;
    }

    // Update texture if needed
    if (!decoder->texture_valid) {
        decoder->frame_texture = LoadTextureFromImage(decoder->frame_image);
        SetTextureFilter(decoder->frame_texture, TEXTURE_FILTER_BILINEAR);
        decoder->texture_valid = true;
    }

    // Get frame height
    int field_h = decoder->frame_height;
    if (field_h > CVBS_MAX_HEIGHT) field_h = CVBS_MAX_HEIGHT;

    // Convert grayscale to RGBA for the image
    Color *pixels = (Color *)decoder->frame_image.data;
    for (int row = 0; row < field_h; row++) {
        uint8_t *src_row = decoder->display_buffer + (row * CVBS_FRAME_WIDTH);
        Color *dst_row = pixels + (row * CVBS_FRAME_WIDTH);
        for (int col = 0; col < CVBS_FRAME_WIDTH; col++) {
            uint8_t gray = src_row[col];
            dst_row[col] = (Color){gray, gray, gray, 255};
        }
    }

    // Upload to GPU
    UpdateTexture(decoder->frame_texture, decoder->frame_image.data);

    // Calculate aspect-correct rectangle (4:3 aspect ratio for CVBS)
    float aspect = 4.0f / 3.0f;
    float display_aspect = width / height;

    float draw_w, draw_h;
    if (display_aspect > aspect) {
        // Height limited
        draw_h = height;
        draw_w = height * aspect;
    } else {
        // Width limited
        draw_w = width;
        draw_h = width / aspect;
    }

    float draw_x = x + (width - draw_w) / 2;
    float draw_y = y + (height - draw_h) / 2;

    // Draw the video frame - raylib will scale from field resolution to display
    Rectangle src = {0, 0, (float)CVBS_FRAME_WIDTH, (float)field_h};
    Rectangle dst = {draw_x, draw_y, draw_w, draw_h};
    DrawTexturePro(decoder->frame_texture, src, dst, (Vector2){0, 0}, 0, WHITE);

    // Note: Format info and frame counter removed - system selector is now
    // an overlay dropdown in the panel (see render_cvbs_system_overlay)
}

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder) {
    if (!decoder) return CVBS_FORMAT_UNKNOWN;
    return decoder->state.format;
}

const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder) {
    if (!decoder) return "Unknown";

    switch (decoder->state.format) {
        case CVBS_FORMAT_PAL:   return "PAL 720x576";
        case CVBS_FORMAT_NTSC:  return "NTSC 720x486";
        case CVBS_FORMAT_SECAM: return "SECAM 720x576";
        default:                return "Detecting...";
    }
}

void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select) {
    if (!decoder) return;

    // format_select: 0=PAL, 1=NTSC, 2=SECAM
    cvbs_format_t new_format = CVBS_FORMAT_NTSC;
    if (format_select == 0) new_format = CVBS_FORMAT_PAL;
    else if (format_select == 2) new_format = CVBS_FORMAT_SECAM;

    if (decoder->state.format != new_format) {
        decoder->state.format = new_format;

        if (new_format == CVBS_FORMAT_NTSC) {
            decoder->state.total_lines = CVBS_NTSC_TOTAL_LINES;
            decoder->state.active_lines = CVBS_NTSC_ACTIVE_LINES;
            decoder->frame_height = CVBS_NTSC_HEIGHT;
            decoder->field_height = NTSC_FIELD_HEIGHT;
            decoder->pll.line_period = CVBS_NTSC_LINE_SAMPLES;
        } else {
            // PAL and SECAM share line/field geometry for luma
            decoder->state.total_lines = CVBS_PAL_TOTAL_LINES;
            decoder->state.active_lines = CVBS_PAL_ACTIVE_LINES;
            decoder->frame_height = CVBS_PAL_HEIGHT;
            decoder->field_height = PAL_FIELD_HEIGHT;
            decoder->pll.line_period = CVBS_PAL_LINE_SAMPLES;
        }

        // Reset decode state after changing system
        gui_cvbs_reset(decoder);
    }
}

