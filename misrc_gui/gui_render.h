#ifndef GUI_RENDER_H
#define GUI_RENDER_H

#include "gui_app.h"
#include "gui_ui.h"  // For color definitions

// Direct rendering functions for use in main render pass
void render_oscilloscope_channel(gui_app_t *app, float x, float y, float width, float height,
                                  int channel, const char *label, Color channel_color);
void render_vu_meter(float x, float y, float width, float height,
                     vu_meter_state_t *meter, const char *label,
                     bool is_clipping_pos, bool is_clipping_neg, Color channel_color);

// Set the app reference for custom rendering
void set_render_app(gui_app_t *app);

// Handle oscilloscope mouse interaction (drag to set trigger level)
// Call this each frame after rendering
void handle_oscilloscope_interaction(gui_app_t *app);

#endif // GUI_RENDER_H
