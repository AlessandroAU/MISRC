/*
 * MISRC GUI - Oscilloscope and Trigger
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 * Uses libsoxr for high-quality waveform resampling
 */

#ifndef GUI_OSCILLOSCOPE_H
#define GUI_OSCILLOSCOPE_H

#include "gui_app.h"
#include "raylib.h"

//-----------------------------------------------------------------------------
// Oscilloscope Phosphor Parameters
//-----------------------------------------------------------------------------

#define SCOPE_DECAY_RATE        0.75f   // Per-frame decay (higher = slower fade)
#define SCOPE_HIT_INCREMENT     0.25f    // Intensity added per waveform hit
#define SCOPE_BLOOM             2.0f    // Bloom factor

//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

// Cleanup oscilloscope resources (static state only)
// Call on application exit
void gui_oscilloscope_cleanup(void);

// Cleanup per-channel resampler resources
// Call when app is being destroyed
void gui_oscilloscope_cleanup_resamplers(gui_app_t *app);

//-----------------------------------------------------------------------------
// Oscilloscope Rendering
//-----------------------------------------------------------------------------

// Render a single channel's waveform with grid and trigger line
void render_oscilloscope_channel(gui_app_t *app, float x, float y, float width, float height,
                                  int channel, const char *label, Color channel_color);

// Handle oscilloscope mouse interaction (drag to set trigger level, scroll to zoom)
// Call this each frame after rendering
void handle_oscilloscope_interaction(gui_app_t *app);

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

// Find first trigger point in sample buffer
// Returns sample index of trigger point, or -1 if no trigger found
ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig);

// Process a single channel: find trigger, resample, update display buffer
// Returns true if display was updated, false if held (Normal mode, no trigger)
// Note: trig is non-const because trigger_display_pos is updated
bool process_channel_display(gui_app_t *app, const int16_t *buf, size_t num_samples,
                             waveform_sample_t *display_buf, size_t *display_count,
                             channel_trigger_t *trig, int channel);

// Update display buffers for both channels (called from extraction thread)
void gui_oscilloscope_update_display(gui_app_t *app, const int16_t *buf_a,
                                      const int16_t *buf_b, size_t num_samples);

#endif // GUI_OSCILLOSCOPE_H
