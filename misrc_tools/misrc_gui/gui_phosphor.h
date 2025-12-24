/*
 * MISRC GUI - Digital Phosphor Display
 *
 * Simulates analog oscilloscope phosphor persistence with heatmap coloring.
 * Pixels accumulate intensity where waveforms pass through, creating a
 * thermal-style visualization (blue = cold/rare, red = hot/frequent).
 */

#ifndef GUI_PHOSPHOR_H
#define GUI_PHOSPHOR_H

#include "gui_app.h"
#include "raylib.h"

//-----------------------------------------------------------------------------
// Phosphor Buffer Management
//-----------------------------------------------------------------------------

// Initialize or resize phosphor buffers and GPU textures for given dimensions
// Returns true on success, false on allocation failure
// Call when oscilloscope display size changes
bool gui_phosphor_init(gui_app_t *app, int width, int height);

// Clear phosphor buffers (reset all intensity to zero)
// Call when switching display modes or resetting view
void gui_phosphor_clear(gui_app_t *app);

// Apply decay to phosphor buffers (call each frame)
// Reduces all pixel intensities by PHOSPHOR_DECAY_RATE
void gui_phosphor_decay(gui_app_t *app);

// Free all phosphor resources (buffers and textures)
// Call on application cleanup
void gui_phosphor_cleanup(gui_app_t *app);

//-----------------------------------------------------------------------------
// Phosphor Rendering
//-----------------------------------------------------------------------------

// Update phosphor buffer with waveform data (accumulates intensity)
// Call each frame before rendering with new waveform samples
// Parameters:
//   app: Application state with phosphor buffers
//   channel: 0 for channel A, 1 for channel B
//   samples: Waveform samples to draw
//   sample_count: Number of samples
//   amplitude_scale: Vertical scale factor (from settings)
void gui_phosphor_update(gui_app_t *app, int channel,
                         const waveform_sample_t *samples, size_t sample_count,
                         float amplitude_scale);

// Render phosphor texture to screen at given position
// Converts intensity buffer to heatmap colors and uploads to GPU
// Parameters:
//   app: Application state with phosphor buffers
//   channel: 0 for channel A, 1 for channel B
//   x, y: Screen position to draw texture
void gui_phosphor_render(gui_app_t *app, int channel, float x, float y);

//-----------------------------------------------------------------------------
// Utility Functions
//-----------------------------------------------------------------------------

// Convert intensity (0-1) to heatmap color (blue -> green -> yellow -> red)
// Exposed for potential use in legends/UI
Color gui_phosphor_intensity_to_color(float intensity);

#endif // GUI_PHOSPHOR_H
