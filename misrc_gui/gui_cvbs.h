/*
 * MISRC GUI - CVBS Video Decoder Module
 *
 * Decodes composite video (PAL/NTSC) from raw ADC samples.
 * Provides frame buffer display and phosphor waveform visualization.
 */

#ifndef GUI_CVBS_H
#define GUI_CVBS_H

#include "gui_app.h"
#include "gui_trigger.h"
#include "raylib.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//-----------------------------------------------------------------------------
// Video Format Constants
//-----------------------------------------------------------------------------

// Video format detection
typedef enum {
    CVBS_FORMAT_UNKNOWN,
    CVBS_FORMAT_PAL,
    CVBS_FORMAT_NTSC,
    CVBS_FORMAT_SECAM
} cvbs_format_t;

// Frame dimensions
#define CVBS_FRAME_WIDTH      720   // Standard horizontal resolution
#define CVBS_PAL_HEIGHT       576   // PAL (D1) active lines
#define CVBS_NTSC_HEIGHT      486   // NTSC (D1) active lines
#define CVBS_MAX_HEIGHT       576   // Maximum (PAL/SECAM)

// Line counts
#define CVBS_PAL_TOTAL_LINES  625
#define CVBS_NTSC_TOTAL_LINES 525
#define CVBS_PAL_ACTIVE_LINES 576
#define CVBS_NTSC_ACTIVE_LINES 480

// Timing at 40 MSPS (from gui_trigger.h constants)
#define CVBS_SAMPLES_PER_LINE_PAL   CVBS_PAL_LINE_SAMPLES    // 2560
#define CVBS_SAMPLES_PER_LINE_NTSC  CVBS_NTSC_LINE_SAMPLES   // 2540

// Phosphor display dimensions
#define CVBS_PHOSPHOR_WIDTH   1024  // Phosphor display width in pixels
#define CVBS_PHOSPHOR_HEIGHT  256   // Phosphor display height in pixels

//-----------------------------------------------------------------------------
// Decoder State Structures
//-----------------------------------------------------------------------------

// Frame synchronization state
typedef struct {
    cvbs_format_t format;          // Detected video format
    int total_lines;               // Total lines per frame (525/625)
    int active_lines;              // Active video lines (480/576)
    int current_line;              // Current line being decoded (0-based)
    int current_field;             // Current field (0=odd/first, 1=even/second)
    bool in_vsync;                 // Currently in vertical sync region
    bool frame_complete;           // A complete frame is ready for display
    int frames_decoded;            // Total frames decoded
} cvbs_frame_state_t;

// Sync pulse classification
typedef enum {
    PULSE_NONE,          // No pulse / noise
    PULSE_HSYNC,         // Normal H-sync (~4.7µs = 188 samples)
    PULSE_EQUALIZING,    // Equalizing pulse (~2.35µs = 94 samples, half-line rate)
    PULSE_SERRATION      // Serration pulse (broad, ~27.3µs = 1092 samples)
} cvbs_pulse_type_t;

// Line period PLL state (persistent across buffers)
typedef struct {
    float line_period;             // Current estimate of line period in samples
    float phase_error;             // Accumulated phase error
    float last_hsync_phase;        // Phase of last H-sync (0-1 within line)
    size_t samples_since_hsync;    // Samples since last confirmed H-sync edge
    int hsync_lock_count;          // Consecutive good H-syncs (for lock detection)
    bool locked;                   // True if PLL is locked to H-sync
} cvbs_line_pll_t;

// V-sync detection state (persistent across buffers)
typedef struct {
    int abnormal_pulse_count;      // Count of non-HSYNC pulses in current window
    int half_line_pulse_count;     // Count of half-line rate pulses
    int window_line_count;         // Lines in current detection window
    bool in_vsync_region;          // Currently detecting V-sync region
    bool awaiting_first_hsync;     // True if we detected V-sync and waiting for first H-sync
    int vsync_start_line;          // Line where V-sync region started
    size_t last_pulse_pos;         // Position of last sync pulse
    cvbs_pulse_type_t last_pulse;  // Type of last detected pulse
} cvbs_vsync_state_t;

// Adaptive threshold state (percentile-based)
typedef struct {
    int16_t sync_tip;              // Estimated sync tip level (lowest ~5%)
    int16_t blanking;              // Estimated blanking level (~30%)
    int16_t black;                 // Estimated black level
    int16_t white;                 // Estimated white level (highest ~95%)
    int16_t threshold;             // Current sync threshold
    float dc_offset;               // Running DC offset estimate (high-pass)
} cvbs_adaptive_levels_t;

// Sample accumulation buffer size - only need a few lines for continuous processing
// PAL line: 2560 samples, NTSC line: 2540 samples
// Keep 32 lines worth for margin (32 * 2560 = 82K samples)
#define CVBS_ACCUM_SIZE         (128 * 1024)  // 128K samples (~50 lines worth)

// Main decoder structure
typedef struct cvbs_decoder {
    // Track whether we've received each field for the current frame
    bool field_ready[2];
    // Frame state
    cvbs_frame_state_t state;

    // Frame buffer (grayscale, 720 x max_height)
    uint8_t *frame_buffer;         // Decoded video frame
    int frame_width;               // Always CVBS_FRAME_WIDTH (720)
    int frame_height;              // Current height based on format

    // Double buffering for display
    uint8_t *display_buffer;       // Buffer currently being displayed
    bool display_ready;            // Display buffer has valid data

    // GPU resources for video display
    Image frame_image;
    Texture2D frame_texture;
    bool texture_valid;

    // Phosphor display for line waveforms
    float *phosphor_buffer;        // Intensity accumulation buffer
    Color *phosphor_pixels;        // RGBA pixel buffer for display
    Image phosphor_image;
    Texture2D phosphor_texture;
    bool phosphor_valid;
    int phosphor_width;
    int phosphor_height;

    // Sample accumulation for cross-buffer line detection
    int16_t *accum_buffer;         // Accumulation buffer for line overlap
    size_t accum_count;            // Current samples in accumulation buffer

    // NEW: Robust sync detection state (persistent across buffers)
    cvbs_adaptive_levels_t adaptive;   // Adaptive threshold tracking
    cvbs_line_pll_t pll;               // Line period PLL
    cvbs_vsync_state_t vsync;          // V-sync detection state

    // Legacy sync tracking (kept for compatibility)
    cvbs_levels_t levels;          // Current signal levels
    int lines_since_vsync;         // Lines counted since last V-sync detection

    // Statistics
    int sync_errors;               // Count of sync detection failures
    int format_changes;            // Count of format auto-detections

    // Configuration (internal)
    int phosphor_line_counter;     // Counter for line skip tracking
} cvbs_decoder_t;

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

// Initialize decoder with maximum dimensions
// Returns true on success, false on allocation failure
bool gui_cvbs_init(cvbs_decoder_t *decoder);

// Cleanup decoder resources
void gui_cvbs_cleanup(cvbs_decoder_t *decoder);

// Reset decoder state (clear frame, reset sync)
void gui_cvbs_reset(cvbs_decoder_t *decoder);

//-----------------------------------------------------------------------------
// Decoding
//-----------------------------------------------------------------------------

// Process a buffer of raw ADC samples
// Call this from the extraction thread with each new buffer
void gui_cvbs_process_buffer(cvbs_decoder_t *decoder,
                              const int16_t *buf, size_t count);

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

// Render the decoded video frame
// Scales to fit within the given rectangle while maintaining aspect ratio
void gui_cvbs_render_frame(cvbs_decoder_t *decoder,
                            float x, float y, float width, float height);

// Render phosphor waveform display of all lines
void gui_cvbs_render_phosphor(cvbs_decoder_t *decoder,
                               float x, float y, float width, float height,
                               Color channel_color);

// Decay phosphor intensity (call once per frame)
void gui_cvbs_decay_phosphor(cvbs_decoder_t *decoder);

//-----------------------------------------------------------------------------
// Configuration
//-----------------------------------------------------------------------------

// Set video format manually (PAL/NTSC)
// Uses cvbs_format_select_t from gui_app.h (CVBS_SELECT_PAL/CVBS_SELECT_NTSC)
void gui_cvbs_set_format(cvbs_decoder_t *decoder, int format_select);

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

// Get detected format
cvbs_format_t gui_cvbs_get_format(cvbs_decoder_t *decoder);

// Get format name string
const char *gui_cvbs_get_format_name(cvbs_decoder_t *decoder);

#endif // GUI_CVBS_H
