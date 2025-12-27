/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Provides frame buffer display and phosphor waveform visualization.
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
// Standard back porch is ~5.7µs = 228 samples, but we add extra margin
// to skip color burst and any residual blanking
#define BACK_PORCH_SAMPLES      280  // ~7µs - slightly more than standard to skip color burst

// Field detection constants
#define PAL_FIELD_LINES         312  // Lines per PAL field (312.5 rounded)
#define NTSC_FIELD_LINES        262  // Lines per NTSC field (262.5 rounded)
#define PAL_ACTIVE_START        23   // First active line in PAL field
#define NTSC_ACTIVE_START       21   // First active line in NTSC field

// Field heights (half of full frame)
#define PAL_FIELD_HEIGHT        288  // 576/2
#define NTSC_FIELD_HEIGHT       243  // 486/2

// CVBS Phosphor settings - much lower than main phosphor since we draw ~300 lines per field
// Main phosphor draws ~1 waveform per frame, CVBS draws ~288 lines per field (50 fields/sec)
// So we need roughly 1/300th the intensity to get similar visual result
#define CVBS_PHOSPHOR_HIT       0.001f   // Much lower than main (0.5f) since many lines drawn
#define CVBS_PHOSPHOR_BLOOM1    0.004f  // Proportional bloom
#define CVBS_PHOSPHOR_BLOOM2    0.002f

// Line skip for phosphor display - skip N lines to reduce CPU load
// Value of 4 means only draw every 4th line to phosphor (still draws all lines to video)
#define CVBS_PHOSPHOR_LINE_SKIP 4

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

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

    // Decode each pixel
    for (int px = 0; px < pixel_width; px++) {
        // Find corresponding sample position
        size_t sample_pos = (size_t)(px * samples_per_pixel);
        if (sample_pos >= active_samples) sample_pos = active_samples - 1;

        // Get sample value and normalize to 0-255
        int16_t sample = active_start[sample_pos];

        // Clamp to black-white range and normalize
        float normalized = (float)(sample - levels->black_level) / level_range;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        pixels[px] = (uint8_t)(normalized * 255.0f);
    }
}

// Helper to add intensity to phosphor buffer with bounds check
static inline void add_phosphor_hit(float *buffer, int width, int height,
                                    int x, int y, float amount) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    int idx = y * width + x;
    buffer[idx] += amount;
    if (buffer[idx] > 1.0f) buffer[idx] = 1.0f;
}

// Draw line with Bresenham and bloom (similar to gui_phosphor.c)
static void draw_phosphor_line(float *buffer, int buf_width, int buf_height,
                               int x0, int y0, int x1, int y1) {
    // Quick reject
    if ((x0 < 0 && x1 < 0) || (x0 >= buf_width && x1 >= buf_width) ||
        (y0 < 0 && y1 < 0) || (y0 >= buf_height && y1 >= buf_height)) {
        return;
    }

    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0, y = y0;
    while (1) {
        // Core pixel
        add_phosphor_hit(buffer, buf_width, buf_height, x, y, CVBS_PHOSPHOR_HIT);

        // Bloom effect (vertical spread for CRT look)
        add_phosphor_hit(buffer, buf_width, buf_height, x, y - 1, CVBS_PHOSPHOR_BLOOM1);
        add_phosphor_hit(buffer, buf_width, buf_height, x, y + 1, CVBS_PHOSPHOR_BLOOM1);
        add_phosphor_hit(buffer, buf_width, buf_height, x, y - 2, CVBS_PHOSPHOR_BLOOM2);
        add_phosphor_hit(buffer, buf_width, buf_height, x, y + 2, CVBS_PHOSPHOR_BLOOM2);

        if (x == x1 && y == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

// Add a line's waveform to the phosphor buffer
static void add_line_to_phosphor(cvbs_decoder_t *decoder,
                                 const int16_t *line_start, size_t available) {
    if (!decoder || !decoder->phosphor_buffer || !line_start) return;

    int width = decoder->phosphor_width;
    int height = decoder->phosphor_height;
    if (width <= 0 || height <= 0) return;

    // Determine how many samples in a line
    size_t line_samples = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                          CVBS_SAMPLES_PER_LINE_NTSC : CVBS_SAMPLES_PER_LINE_PAL;
    if (available < line_samples) line_samples = available;

    float samples_per_pixel = (float)line_samples / width;
    float center_y = height / 2.0f;
    float scale = (height - 20.0f) / 4096.0f;  // 12-bit signed range

    // Draw this line into the phosphor buffer using line segments
    int prev_x = -1, prev_y = -1;
    for (int px = 0; px < width; px++) {
        size_t sample_idx = (size_t)(px * samples_per_pixel);
        if (sample_idx >= line_samples) sample_idx = line_samples - 1;

        int16_t sample = line_start[sample_idx];
        int screen_y = (int)(center_y - sample * scale);

        // Clamp to buffer
        if (screen_y < 0) screen_y = 0;
        if (screen_y >= height) screen_y = height - 1;

        // Draw line segment from previous point
        if (prev_x >= 0) {
            draw_phosphor_line(decoder->phosphor_buffer, width, height,
                              prev_x, prev_y, px, screen_y);
        }

        prev_x = px;
        prev_y = screen_y;
    }
}

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

bool gui_cvbs_init(cvbs_decoder_t *decoder) {
    if (!decoder) return false;

    memset(decoder, 0, sizeof(cvbs_decoder_t));

    // Allocate frame buffer at full-frame resolution (D1)
    decoder->frame_width = CVBS_FRAME_WIDTH;
    decoder->frame_height = CVBS_PAL_HEIGHT;  // Start with PAL full-frame height
    decoder->frame_buffer = (uint8_t *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, 1);
    if (!decoder->frame_buffer) {
        return false;
    }

    // Allocate display buffer (double buffering)
    decoder->display_buffer = (uint8_t *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, 1);
    if (!decoder->display_buffer) {
        free(decoder->frame_buffer);
        decoder->frame_buffer = NULL;
        return false;
    }

    // Allocate phosphor intensity buffer (GPU shader handles color conversion)
    decoder->phosphor_width = CVBS_PHOSPHOR_WIDTH;
    decoder->phosphor_height = CVBS_PHOSPHOR_HEIGHT;
    size_t phosphor_size = CVBS_PHOSPHOR_WIDTH * CVBS_PHOSPHOR_HEIGHT;

    decoder->phosphor_buffer = (float *)calloc(phosphor_size, sizeof(float));
    decoder->phosphor_pixels = NULL;  // Not needed with GPU shader path
    if (!decoder->phosphor_buffer) {
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        return false;
    }

    // Create raylib Image for video display (RGBA format at full-frame resolution)
    // Allocate our own pixel buffer so we control the memory
    Color *frame_pixels = (Color *)calloc(CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT, sizeof(Color));
    if (!frame_pixels) {
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        free(decoder->phosphor_buffer);
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        decoder->phosphor_buffer = NULL;
        return false;
    }
    decoder->frame_image.data = frame_pixels;
    decoder->frame_image.width = CVBS_FRAME_WIDTH;
    decoder->frame_image.height = CVBS_MAX_HEIGHT;
    decoder->frame_image.mipmaps = 1;
    decoder->frame_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    decoder->texture_valid = false;

    // Initialize phosphor texture (CPU-updated RGBA texture)
    decoder->phosphor_pixels = (Color *)calloc(phosphor_size, sizeof(Color));
    if (!decoder->phosphor_pixels) {
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        free(decoder->phosphor_buffer);
        free(decoder->frame_image.data);
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        decoder->phosphor_buffer = NULL;
        decoder->frame_image.data = NULL;
        return false;
    }

    decoder->phosphor_image.data = decoder->phosphor_pixels;
    decoder->phosphor_image.width = decoder->phosphor_width;
    decoder->phosphor_image.height = decoder->phosphor_height;
    decoder->phosphor_image.mipmaps = 1;
    decoder->phosphor_image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    decoder->phosphor_valid = false;

    // Allocate sample accumulation buffer for cross-buffer line detection
    decoder->accum_buffer = (int16_t *)calloc(CVBS_ACCUM_SIZE, sizeof(int16_t));
    if (!decoder->accum_buffer) {
        free(decoder->frame_buffer);
        free(decoder->display_buffer);
        free(decoder->phosphor_buffer);
        free(decoder->frame_image.data);
        decoder->frame_buffer = NULL;
        decoder->display_buffer = NULL;
        decoder->phosphor_buffer = NULL;
        decoder->frame_image.data = NULL;
        return false;
    }
    decoder->accum_count = 0;

    // Initialize adaptive levels
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));

    // Initialize PLL with PAL default
    decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    decoder->pll.phase_error = 0;
    decoder->pll.last_hsync_phase = 0;
    decoder->pll.samples_since_hsync = 0;
    decoder->pll.hsync_lock_count = 0;
    decoder->pll.locked = false;

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

    if (decoder->phosphor_valid) {
        UnloadTexture(decoder->phosphor_texture);
        decoder->phosphor_valid = false;
    }

    // Free frame image data (we allocated it ourselves)
    if (decoder->frame_image.data) {
        free(decoder->frame_image.data);
        decoder->frame_image.data = NULL;
    }

    // Free phosphor image data (we allocated it ourselves)
    if (decoder->phosphor_image.data) {
        free(decoder->phosphor_image.data);
        decoder->phosphor_image.data = NULL;
    }

    free(decoder->frame_buffer);
    free(decoder->display_buffer);
    free(decoder->phosphor_buffer);
    free(decoder->accum_buffer);

    decoder->frame_buffer = NULL;
    decoder->display_buffer = NULL;
    decoder->phosphor_buffer = NULL;
    decoder->phosphor_pixels = NULL;
    decoder->accum_buffer = NULL;
}

void gui_cvbs_reset(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    // Clear frame buffers
    if (decoder->frame_buffer) {
        memset(decoder->frame_buffer, 0, CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT);
    }
    if (decoder->display_buffer) {
        memset(decoder->display_buffer, 0, CVBS_FRAME_WIDTH * CVBS_MAX_HEIGHT);
    }
    if (decoder->phosphor_buffer) {
        memset(decoder->phosphor_buffer, 0,
               decoder->phosphor_width * decoder->phosphor_height * sizeof(float));
    }

    // Reset accumulation buffer
    decoder->accum_count = 0;

    // Reset adaptive levels
    memset(&decoder->adaptive, 0, sizeof(decoder->adaptive));

    // Reset PLL (keep line period consistent with selected system)
    if (decoder->state.format == CVBS_FORMAT_NTSC) {
        decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_NTSC;
    } else {
        // PAL + SECAM + UNKNOWN use PAL timing
        decoder->pll.line_period = CVBS_SAMPLES_PER_LINE_PAL;
    }
    decoder->pll.phase_error = 0;
    decoder->pll.last_hsync_phase = 0;
    decoder->pll.samples_since_hsync = 0;
    decoder->pll.hsync_lock_count = 0;
    decoder->pll.locked = false;

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

    // Initialize configuration
    decoder->phosphor_line_counter = 0;
}

//-----------------------------------------------------------------------------
// Decoding - Robust sync detection using timing structure
//-----------------------------------------------------------------------------

// H-sync pulse width thresholds at 40 MSPS (in samples)
// H-sync: 4.7µs = 188 samples typical
#define HSYNC_MIN_WIDTH         100   // ~2.5µs - minimum H-sync (more tolerant)
#define HSYNC_MAX_WIDTH         350   // ~8.75µs - maximum H-sync (more tolerant)

//-----------------------------------------------------------------------------
// Adaptive Level Estimation
//-----------------------------------------------------------------------------

// Update adaptive levels using percentile-like estimation
static void update_adaptive_levels(cvbs_decoder_t *decoder,
                                    const int16_t *buf, size_t count) {
    if (count < 1000) return;

    // Sample every 16th value for efficiency
    int16_t local_min = buf[0];
    int16_t local_max = buf[0];
    int64_t sum = 0;
    int sample_count = 0;

    for (size_t i = 0; i < count; i += 16) {
        int16_t s = buf[i];
        if (s < local_min) local_min = s;
        if (s > local_max) local_max = s;
        sum += s;
        sample_count++;
    }

    // Exponential moving average for stability
    const float alpha = 0.1f;  // Slow adaptation

    if (decoder->adaptive.sync_tip == 0 && decoder->adaptive.white == 0) {
        // First time - initialize directly
        decoder->adaptive.sync_tip = local_min;
        decoder->adaptive.white = local_max;
    } else {
        // Smooth update
        decoder->adaptive.sync_tip = (int16_t)(decoder->adaptive.sync_tip * (1.0f - alpha) +
                                               local_min * alpha);
        decoder->adaptive.white = (int16_t)(decoder->adaptive.white * (1.0f - alpha) +
                                            local_max * alpha);
    }

    // Derive other levels
    int16_t range = decoder->adaptive.white - decoder->adaptive.sync_tip;
    if (range < 100) range = 100;  // Minimum range to avoid division issues

    // For CVBS, the sync tip is at IRE -40, blanking at IRE 0, black at IRE ~7.5, white at IRE 100
    // So sync is about 40/140 = 28.5% of the range below blanking
    // Threshold should be midway between sync tip and blanking: ~15% of total range
    // This gives a clean crossing point in the middle of the sync pulse edge
    decoder->adaptive.threshold = decoder->adaptive.sync_tip + (int16_t)(range * 0.15f);

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

// NOTE: Complex pulse classification and PLL removed - using simplified line-count based approach

//-----------------------------------------------------------------------------
// Field Handling
//-----------------------------------------------------------------------------

// Complete a field; once we have both fields, publish a full frame for display
static void complete_field(cvbs_decoder_t *decoder) {
    if (!decoder) return;

    int line_count = decoder->lines_since_vsync;

    // Get expected field parameters
    int expected_active = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                          NTSC_FIELD_HEIGHT : PAL_FIELD_HEIGHT;
    int active_start = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                       NTSC_ACTIVE_START : PAL_ACTIVE_START;
    int actual_active = line_count - active_start;

    // Only accept if we got a reasonable field (at least 50% of expected)
    if (actual_active < (expected_active / 2)) {
        return;
    }

    // Mark this field as received
    decoder->field_ready[decoder->state.current_field ? 1 : 0] = true;

    // When both fields are ready, publish a full frame
    if (decoder->field_ready[0] && decoder->field_ready[1]) {
        int frame_h = decoder->frame_height;
        if (frame_h > CVBS_MAX_HEIGHT) frame_h = CVBS_MAX_HEIGHT;

        memcpy(decoder->display_buffer, decoder->frame_buffer,
               (size_t)CVBS_FRAME_WIDTH * (size_t)frame_h);
        decoder->display_ready = true;
        decoder->state.frame_complete = true;
        decoder->state.frames_decoded++;

        // Prepare for next frame
        decoder->field_ready[0] = false;
        decoder->field_ready[1] = false;
    }
}

// Start a new field
static void start_new_field(cvbs_decoder_t *decoder) {
    // Complete previous field if substantial
    if (decoder->lines_since_vsync > 100) {
        complete_field(decoder);
    }

    // Reset for new field
    decoder->state.current_line = 0;
    decoder->state.current_field = 1 - decoder->state.current_field;
    decoder->lines_since_vsync = 0;

    // If we're starting the first field of a new frame, clear the full-frame buffer
    if (decoder->state.current_field == 0) {
        int frame_h = decoder->frame_height;
        if (frame_h > CVBS_MAX_HEIGHT) frame_h = CVBS_MAX_HEIGHT;
        memset(decoder->frame_buffer, 0, (size_t)CVBS_FRAME_WIDTH * (size_t)frame_h);
        decoder->field_ready[0] = false;
        decoder->field_ready[1] = false;
    }
}

// Process a single video line
static void process_video_line(cvbs_decoder_t *decoder,
                               const int16_t *line_start, size_t available) {
    int active_start = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                       NTSC_ACTIVE_START : PAL_ACTIVE_START;

    int line_num = decoder->state.current_line;
    if (line_num >= active_start) {
        int field_line = line_num - active_start;
        int max_field_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                              NTSC_FIELD_HEIGHT : PAL_FIELD_HEIGHT;

        if (field_line >= 0 && field_line < max_field_lines) {
            // Interleave fields into a full-frame buffer: line = field_line*2 + field
            int out_line = field_line * 2 + (decoder->state.current_field ? 1 : 0);
            if (out_line < 0) return;
            if (out_line >= decoder->frame_height) return;

            uint8_t *row_ptr = decoder->frame_buffer + ((size_t)out_line * (size_t)CVBS_FRAME_WIDTH);
            decode_line_to_pixels(line_start, available,
                                 &decoder->levels, row_ptr, CVBS_FRAME_WIDTH);

            // Add line to phosphor display (with skip for performance)
            decoder->phosphor_line_counter++;
            if (decoder->phosphor_line_counter >= CVBS_PHOSPHOR_LINE_SKIP) {
                decoder->phosphor_line_counter = 0;
                add_line_to_phosphor(decoder, line_start, available);
            }
        }
    }

    decoder->state.current_line++;
    decoder->lines_since_vsync++;
}

//-----------------------------------------------------------------------------
// Main Buffer Processing - Continuous line-by-line approach
//-----------------------------------------------------------------------------

// Find next H-sync pulse in buffer
// Returns position after the sync pulse ends, or -1 if not found
// Sets *is_vsync_region to true if we detect V-sync (half-line rate pulses)
static ssize_t find_next_hsync(const int16_t *buf, size_t count, size_t start,
                                int16_t threshold, bool *is_vsync_region) {
    if (start + 500 >= count) return -1;
    if (is_vsync_region) *is_vsync_region = false;

    size_t last_sync_start = 0;
    int half_line_count = 0;

    for (size_t i = start + 1; i < count - 300; i++) {
        // Look for falling edge into sync
        if (buf[i - 1] >= threshold && buf[i] < threshold) {
            size_t sync_start = i;
            size_t j = i;

            // Measure pulse width
            while (j < count && buf[j] < threshold) {
                j++;
            }

            size_t pulse_width = j - sync_start;

            // Skip very short pulses (noise)
            if (pulse_width < 30) {
                i = j;
                continue;
            }

            // Check interval from last sync to detect V-sync region
            if (last_sync_start > 0) {
                size_t interval = sync_start - last_sync_start;
                // Half-line interval: 1000-1600 samples (25-40µs)
                if (interval >= 1000 && interval <= 1600) {
                    half_line_count++;
                    if (half_line_count >= 3 && is_vsync_region) {
                        *is_vsync_region = true;
                    }
                } else if (interval >= 2200 && interval <= 3000) {
                    // Full line interval - reset half-line counter
                    half_line_count = 0;
                }
            }
            last_sync_start = sync_start;

            // Accept H-sync pulses (100-350 samples = 2.5-8.75µs at 40 MSPS)
            if (pulse_width >= HSYNC_MIN_WIDTH && pulse_width <= HSYNC_MAX_WIDTH) {
                return (ssize_t)j;  // Return position after sync ends
            }

            // Skip short/long pulses (equalizing or broad)
            i = j;
        }
    }

    return -1;
}

// Debug counter for periodic logging
static int s_debug_counter = 0;

void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count) {
    if (!decoder || !buf || count < 1000) return;
    if (!decoder->accum_buffer) return;

    // Update adaptive signal levels from incoming buffer
    update_adaptive_levels(decoder, buf, count);

    // Check for minimum signal strength
    if (decoder->levels.range < 100) {
        decoder->sync_errors++;
        return;
    }

    // Append new samples to accumulation buffer
    size_t space_available = CVBS_ACCUM_SIZE - decoder->accum_count;
    size_t to_copy = (count <= space_available) ? count : space_available;

    if (to_copy > 0) {
        memcpy(decoder->accum_buffer + decoder->accum_count, buf, to_copy * sizeof(int16_t));
        decoder->accum_count += to_copy;
    }

    int16_t threshold = decoder->adaptive.threshold;
    size_t line_period = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                         CVBS_SAMPLES_PER_LINE_NTSC : CVBS_SAMPLES_PER_LINE_PAL;

    // We need at least 2 lines worth of data to process
    size_t min_required = line_period * 2;
    if (decoder->accum_count < min_required) {
        return;
    }

    // Debug logging periodically
    s_debug_counter++;
    bool do_debug = (s_debug_counter % 3000 == 0);
    if (do_debug) {
        fprintf(stderr, "[CVBS] lines=%d field=%d frames=%d accum=%zu\n",
                decoder->lines_since_vsync, decoder->state.current_field,
                decoder->state.frames_decoded, decoder->accum_count);
    }

    // Process lines from the accumulation buffer
    // Keep searching for H-sync and processing complete lines
    size_t pos = 0;
    size_t lines_processed = 0;
    size_t last_safe_pos = 0;  // Last position we can safely discard up to
    bool in_vsync = false;

    while (pos < decoder->accum_count - line_period - 500) {
        // Find next H-sync (also detects V-sync region)
        bool vsync_detected = false;
        ssize_t hsync_end = find_next_hsync(decoder->accum_buffer, decoder->accum_count,
                                             pos, threshold, &vsync_detected);

        if (hsync_end < 0) {
            // No more H-sync found
            break;
        }

        size_t line_start = (size_t)hsync_end;

        // Check if we have enough samples for a full line
        if (line_start + line_period > decoder->accum_count) {
            // Not enough data for complete line - stop here
            break;
        }

        // V-sync detection: if we detect half-line pulses, complete current field
        if (vsync_detected && !in_vsync) {
            in_vsync = true;
            // Complete field when entering V-sync
            if (decoder->lines_since_vsync > 100) {
                complete_field(decoder);
                start_new_field(decoder);
            }
        } else if (!vsync_detected && in_vsync) {
            // Exiting V-sync region - we're now at the start of a new field
            in_vsync = false;
        }

        // Fallback: if we've processed many lines without V-sync, complete field anyway
        int expected_field_lines = (decoder->state.format == CVBS_FORMAT_NTSC) ?
                                   NTSC_FIELD_LINES : PAL_FIELD_LINES;

        if (decoder->lines_since_vsync >= expected_field_lines + 20) {
            // Way past expected field length - force completion
            complete_field(decoder);
            start_new_field(decoder);
        }

        // Skip processing during V-sync region (these aren't video lines)
        if (!in_vsync) {
            // Process this video line
            process_video_line(decoder, decoder->accum_buffer + line_start, line_period);
            lines_processed++;
        }

        // Move position forward - skip most of a line to find next H-sync
        pos = line_start + (line_period * 3 / 4);
        last_safe_pos = line_start;  // We've fully consumed up to here
    }

    // Discard processed data, keeping a margin for partial line at end
    if (last_safe_pos > line_period) {
        size_t discard = last_safe_pos - line_period;  // Keep 1 line margin
        if (discard > 0 && discard < decoder->accum_count) {
            size_t remaining = decoder->accum_count - discard;
            memmove(decoder->accum_buffer, decoder->accum_buffer + discard,
                    remaining * sizeof(int16_t));
            decoder->accum_count = remaining;
        }
    }

    // Safety: if buffer is getting too full, discard more aggressively
    if (decoder->accum_count > CVBS_ACCUM_SIZE - 20000) {
        size_t discard = decoder->accum_count / 2;
        size_t remaining = decoder->accum_count - discard;
        memmove(decoder->accum_buffer, decoder->accum_buffer + discard,
                remaining * sizeof(int16_t));
        decoder->accum_count = remaining;
        decoder->sync_errors++;
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

void gui_cvbs_render_phosphor(cvbs_decoder_t *decoder,
                               float x, float y, float width, float height,
                               Color channel_color) {
    (void)channel_color;  // Unused - phosphor uses heatmap colors

    if (!decoder || !decoder->phosphor_buffer) {
        // Draw placeholder
        DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){15, 15, 25, 255});
        return;
    }

    int phos_w = decoder->phosphor_width;
    int phos_h = decoder->phosphor_height;

    // Draw background
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){15, 15, 25, 255});

    // Draw grid
    Color grid_color = {40, 40, 55, 255};
    int div_x = 10, div_y = 4;
    for (int i = 1; i < div_x; i++) {
        float gx = x + (width * i / div_x);
        DrawLine((int)gx, (int)y, (int)gx, (int)(y + height), grid_color);
    }
    for (int i = 1; i < div_y; i++) {
        float gy = y + (height * i / div_y);
        DrawLine((int)x, (int)gy, (int)(x + width), (int)gy, grid_color);
    }

    // Draw center line
    float center_y_line = y + height / 2;
    DrawLine((int)x, (int)center_y_line, (int)(x + width), (int)center_y_line, (Color){60, 60, 80, 255});

    // Lazy-create phosphor texture
    if (!decoder->phosphor_valid) {
        decoder->phosphor_texture = LoadTextureFromImage(decoder->phosphor_image);
        SetTextureFilter(decoder->phosphor_texture, TEXTURE_FILTER_BILINEAR);
        decoder->phosphor_valid = true;
    }

    // Convert intensity buffer to RGBA (simple green phosphor)
    // Note: keep this cheap; CVBS phosphor is primarily a debug visualization.
    Color *dst = decoder->phosphor_pixels;
    size_t n = (size_t)phos_w * (size_t)phos_h;
    for (size_t i = 0; i < n; i++) {
        float v = decoder->phosphor_buffer[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char g = (unsigned char)(v * 255.0f);
        dst[i] = (Color){0, g, 0, g};
    }

    UpdateTexture(decoder->phosphor_texture, decoder->phosphor_image.data);

    // Draw scaled
    Rectangle srcp = {0, 0, (float)phos_w, (float)phos_h};
    Rectangle dstp = {x, y, width, height};
    DrawTexturePro(decoder->phosphor_texture, srcp, dstp, (Vector2){0, 0}, 0, WHITE);

    // Draw sync level indicator if we have valid levels
    if (decoder->levels.range > 100) {
        float sync_norm = (float)decoder->levels.sync_threshold / 2048.0f;
        float sync_screen_y = center_y_line - sync_norm * (height / 2 - 10);
        DrawLine((int)x, (int)sync_screen_y, (int)(x + 30), (int)sync_screen_y,
                 (Color){255, 100, 100, 150});

        float black_norm = (float)decoder->levels.black_level / 2048.0f;
        float black_screen_y = center_y_line - black_norm * (height / 2 - 10);
        DrawLine((int)x, (int)black_screen_y, (int)(x + 30), (int)black_screen_y,
                 (Color){100, 100, 100, 150});
    }
}

void gui_cvbs_decay_phosphor(cvbs_decoder_t *decoder) {
    if (!decoder || !decoder->phosphor_buffer) return;

    // Simple decay for CPU phosphor buffer
    size_t n = (size_t)decoder->phosphor_width * (size_t)decoder->phosphor_height;
    const float decay = 0.92f;
    for (size_t i = 0; i < n; i++) {
        decoder->phosphor_buffer[i] *= decay;
        if (decoder->phosphor_buffer[i] < 0.0001f) decoder->phosphor_buffer[i] = 0.0f;
    }
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
            decoder->pll.line_period = CVBS_NTSC_LINE_SAMPLES;
        } else {
            // PAL and SECAM share line/field geometry for luma
            decoder->state.total_lines = CVBS_PAL_TOTAL_LINES;
            decoder->state.active_lines = CVBS_PAL_ACTIVE_LINES;
            decoder->frame_height = CVBS_PAL_HEIGHT;
            decoder->pll.line_period = CVBS_PAL_LINE_SAMPLES;
        }

        // Reset decode state after changing system
        gui_cvbs_reset(decoder);
    }
}

