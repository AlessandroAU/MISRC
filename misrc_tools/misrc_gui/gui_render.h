#ifndef GUI_RENDER_H
#define GUI_RENDER_H

#include "gui_app.h"
#include "clay.h"

// Colors for rendering
#define COLOR_CHANNEL_A       (Color){ 80, 220, 100, 255 }
#define COLOR_CHANNEL_B       (Color){ 220, 200, 80, 255 }
#define COLOR_GRID            (Color){ 60, 60, 70, 100 }
#define COLOR_GRID_MAJOR      (Color){ 80, 80, 95, 150 }
#define COLOR_TEXT_DIM        (Color){ 140, 140, 155, 255 }
#define COLOR_METER_BG        (Color){ 25, 25, 30, 255 }
#define COLOR_METER_GREEN     (Color){ 50, 200, 80, 255 }
#define COLOR_METER_YELLOW    (Color){ 220, 200, 50, 255 }
#define COLOR_METER_RED       (Color){ 220, 50, 50, 255 }
#define COLOR_CLIP_RED        (Color){ 255, 50, 50, 255 }

// Custom element rendering functions (called from clay_renderer_raylib.c)
void render_oscilloscope_custom(Clay_BoundingBox bounds, void *osc_data);
void render_vu_meter_custom(Clay_BoundingBox bounds, void *vu_data);

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
