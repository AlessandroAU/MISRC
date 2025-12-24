#ifndef GUI_APP_H
#define GUI_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "raylib.h"

// Forward declarations
typedef struct hsdaoh_dev hsdaoh_dev_t;
typedef struct sc_handle sc_handle_t;

// Display buffer size (samples per channel for oscilloscope)
#define DISPLAY_BUFFER_SIZE 4096
#define MAX_DEVICES 16
#define MAX_FILENAME_LEN 256

// Waveform display sample with resampled value and peak envelope
// Values are normalized floats in range -1.0 to 1.0
typedef struct {
    float value;              // Resampled waveform value (via libsoxr)
    float min_val, max_val;   // Raw min/max peaks for envelope display
} waveform_sample_t;

// VU meter state - tracks positive and negative separately for AC signals
typedef struct vu_meter_state {
    float level_pos;          // Current smoothed positive level (0-1)
    float level_neg;          // Current smoothed negative level (0-1)
    float peak_pos;           // Peak hold positive (0-1)
    float peak_neg;           // Peak hold negative (0-1)
    float peak_hold_time_pos; // Time since positive peak was captured
    float peak_hold_time_neg; // Time since negative peak was captured
} vu_meter_state_t;

// Per-channel trigger configuration and state
// Simplified: rising edge only, always auto-update
typedef struct {
    bool enabled;              // Trigger enabled for this channel
    int16_t level;             // Trigger level (-2048 to +2047, 12-bit range)
    float zoom_scale;          // Samples per pixel (1.0 = max zoom, higher = more zoomed out)
    int trigger_display_pos;   // Where trigger appears in display buffer (-1 if not triggered)
    atomic_int display_width;  // Actual pixel width of oscilloscope display (updated by renderer, read by extraction thread)
} channel_trigger_t;

// Zoom limits
#define ZOOM_SCALE_MIN 1.0f    // 1 sample per pixel (max zoom in)
#define ZOOM_SCALE_MAX 128.0f  // 128 samples per pixel (max zoom out)
#define ZOOM_SCALE_DEFAULT 32.0f

// Device info for enumeration
typedef struct {
    char name[64];
    char serial[64];
    bool is_simple_capture;   // true = OS video capture, false = hsdaoh
    int index;
} device_info_t;

// GUI settings (bound to UI controls)
typedef struct {
    int device_index;
    char output_filename_a[MAX_FILENAME_LEN];
    char output_filename_b[MAX_FILENAME_LEN];
    bool capture_a;
    bool capture_b;
    bool use_flac;
    int flac_level;           // 0-8
    bool show_grid;
    float time_scale;         // Time per division (ms)
    float amplitude_scale;    // Amplitude scale factor
} gui_settings_t;

// Main application state
typedef struct gui_app {
    // Device handles
    hsdaoh_dev_t *hs_dev;
    sc_handle_t *sc_dev;

    // Capture state
    bool is_capturing;
    bool is_recording;

    // Device enumeration
    device_info_t devices[MAX_DEVICES];
    int device_count;
    int selected_device;

    // Per-channel display buffers for waveform
    waveform_sample_t display_samples_a[DISPLAY_BUFFER_SIZE];
    waveform_sample_t display_samples_b[DISPLAY_BUFFER_SIZE];
    size_t display_samples_available_a;
    size_t display_samples_available_b;

    // VU meter state (updated on main thread from atomic values)
    vu_meter_state_t vu_a;
    vu_meter_state_t vu_b;

    // Atomic values from capture thread (separate pos/neg for AC signals)
    atomic_uint_fast16_t peak_a_pos;  // Maximum positive sample (0-2047)
    atomic_uint_fast16_t peak_a_neg;  // Maximum negative sample (0-2048, stored as positive)
    atomic_uint_fast16_t peak_b_pos;
    atomic_uint_fast16_t peak_b_neg;

    // Statistics (atomic, updated by capture thread)
    atomic_uint_fast64_t total_samples;
    atomic_uint_fast64_t samples_a;         // Per-channel sample count
    atomic_uint_fast64_t samples_b;
    atomic_uint_fast32_t frame_count;
    atomic_uint_fast32_t error_count;
    atomic_uint_fast32_t error_count_a;     // Per-channel error counts
    atomic_uint_fast32_t error_count_b;
    atomic_uint_fast32_t clip_count_a_pos;  // Positive clipping (sample >= 2047)
    atomic_uint_fast32_t clip_count_a_neg;  // Negative clipping (sample <= -2048)
    atomic_uint_fast32_t clip_count_b_pos;
    atomic_uint_fast32_t clip_count_b_neg;
    atomic_bool stream_synced;
    atomic_uint_fast32_t sample_rate;

    // Recording state
    double recording_start_time;
    atomic_uint_fast64_t recording_bytes;        // Legacy: total raw bytes
    atomic_uint_fast64_t recording_raw_a;        // Raw input bytes channel A
    atomic_uint_fast64_t recording_raw_b;        // Raw input bytes channel B
    atomic_uint_fast64_t recording_compressed_a; // Compressed output bytes channel A
    atomic_uint_fast64_t recording_compressed_b; // Compressed output bytes channel B

    // GUI settings
    gui_settings_t settings;

    // UI state
    bool settings_panel_open;
    bool device_dropdown_open;
    char status_message[256];
    double status_message_time;

    // Auto-reconnect state
    bool auto_reconnect_enabled;
    bool reconnect_pending;
    double reconnect_attempt_time;
    int reconnect_attempts;

    // Device disconnect detection (timestamp of last successful callback)
    atomic_uint_fast64_t last_callback_time_ms;

    // Fonts
    Font *fonts;

    // Per-channel trigger configuration (includes zoom level per channel)
    channel_trigger_t trigger_a;
    channel_trigger_t trigger_b;

} gui_app_t;

// Application lifecycle
void gui_app_init(gui_app_t *app);
void gui_app_cleanup(gui_app_t *app);

// Device management
void gui_app_enumerate_devices(gui_app_t *app);
int gui_app_start_capture(gui_app_t *app);
void gui_app_stop_capture(gui_app_t *app);
int gui_app_start_recording(gui_app_t *app);
void gui_app_stop_recording(gui_app_t *app);

// Update functions (called each frame)
void gui_app_update_vu_meters(gui_app_t *app, float dt);
void gui_app_update_display_buffer(gui_app_t *app);

// Clear display (called when device disconnects)
void gui_app_clear_display(gui_app_t *app);

// Status messages
void gui_app_set_status(gui_app_t *app, const char *message);

// Constants for VU meter
#define VU_ATTACK_TIME 0.01f      // 10ms attack
#define VU_RELEASE_TIME 0.3f      // 300ms release
#define PEAK_HOLD_DURATION 2.0f   // 2 second peak hold
#define PEAK_DECAY_RATE 0.5f      // Decay rate after hold

// Note: Color definitions are in gui_ui.h and gui_render.h

#endif // GUI_APP_H
