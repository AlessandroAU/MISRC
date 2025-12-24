/*
 * MISRC GUI - Oscilloscope and Trigger
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 */

#ifndef GUI_OSCILLOSCOPE_H
#define GUI_OSCILLOSCOPE_H

#include "gui_app.h"
#include "raylib.h"

//-----------------------------------------------------------------------------
// Oscilloscope Rendering
//-----------------------------------------------------------------------------

// Render a single channel's waveform with grid and trigger line
void render_oscilloscope_channel(gui_app_t *app, float x, float y, float width, float height,
                                  int channel, const char *label, Color channel_color);

// Handle oscilloscope mouse interaction (drag to set trigger level)
// Call this each frame after rendering
void handle_oscilloscope_interaction(gui_app_t *app);

//-----------------------------------------------------------------------------
// Trigger Detection
//-----------------------------------------------------------------------------

// Find first trigger point in sample buffer
// Returns sample index of trigger point, or -1 if no trigger found
ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig);

// Process a single channel: find trigger, decimate, update display buffer
// Returns true if display was updated, false if held (Normal mode, no trigger)
bool process_channel_display(gui_app_t *app, const int16_t *buf, size_t num_samples,
                             waveform_minmax_t *display_buf, size_t *display_count,
                             const channel_trigger_t *trig);

// Update display buffers for both channels (called from extraction thread)
void gui_oscilloscope_update_display(gui_app_t *app, const int16_t *buf_a,
                                      const int16_t *buf_b, size_t num_samples);

#endif // GUI_OSCILLOSCOPE_H
