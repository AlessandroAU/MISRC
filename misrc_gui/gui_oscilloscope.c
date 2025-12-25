/*
 * MISRC GUI - Oscilloscope and Trigger Implementation
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 */

#include "gui_oscilloscope.h"
#include "gui_phosphor.h"
#include "gui_trigger.h"
#include "gui_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Grid Settings
//-----------------------------------------------------------------------------

#define GRID_DIVISIONS_X 10
#define GRID_DIVISIONS_Y 4  // Per channel

//-----------------------------------------------------------------------------
// Static State
//-----------------------------------------------------------------------------

// App pointer for font access
static gui_app_t *s_osc_app = NULL;

// Oscilloscope bounds for mouse interaction (stored per channel)
static Rectangle s_osc_bounds[2] = {0};
static bool s_osc_bounds_valid[2] = {false, false};
static int s_dragging_channel = -1;  // Which channel is being dragged (-1 = none)

// Cleanup oscilloscope resources
void gui_oscilloscope_cleanup(void) {
    // No static resources to clean up - phosphor cleanup is in gui_phosphor module
}

//-----------------------------------------------------------------------------
// Legacy Phosphor Wrappers (forward to gui_phosphor module)
//-----------------------------------------------------------------------------

bool gui_oscilloscope_init_phosphor(gui_app_t *app, int width, int height) {
    return gui_phosphor_init(app, width, height);
}

void gui_oscilloscope_clear_phosphor(gui_app_t *app) {
    gui_phosphor_clear(app);
}

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

// Set the app reference (called from set_render_app in gui_render.c)
void gui_oscilloscope_set_app(gui_app_t *app) {
    s_osc_app = app;
}

// Helper to draw text using the app's font
static void draw_text_with_font(const char *text, float x, float y, int fontSize, Color color) {
    if (s_osc_app && s_osc_app->fonts) {
        Font font = s_osc_app->fonts[0];
        DrawTextEx(font, text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)x, (int)y, fontSize, color);
    }
}

// Helper to measure text using the app's font
static int measure_text_with_font(const char *text, int fontSize) {
    if (s_osc_app && s_osc_app->fonts) {
        Font font = s_osc_app->fonts[0];
        Vector2 size = MeasureTextEx(font, text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return MeasureText(text, fontSize);
}

// Draw grid for a single channel with amplitude scale ticks
static void draw_channel_grid(float x, float y, float width, float height,
                               const char *label, Color channel_color, bool show_grid) {
    // Background slightly darker than main bg
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){25, 25, 30, 255});

    float center_y = y + height / 2;

    if (show_grid) {
        // Vertical grid lines (time divisions)
        for (int i = 1; i < GRID_DIVISIONS_X; i++) {
            float gx = x + (width * i / GRID_DIVISIONS_X);
            DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height}, COLOR_GRID);
        }

        // Horizontal grid lines (amplitude divisions)
        for (int i = 1; i < GRID_DIVISIONS_Y; i++) {
            float gy = y + (height * i / GRID_DIVISIONS_Y);
            DrawLineV((Vector2){x, gy}, (Vector2){x + width, gy}, COLOR_GRID);
        }
    }

    // Center line (0V reference) - always show
    DrawLineEx((Vector2){x, center_y}, (Vector2){x + width, center_y}, 1.0f, COLOR_GRID_MAJOR);

    // Border
    DrawRectangleLinesEx((Rectangle){x, y, width, height}, 1, COLOR_GRID_MAJOR);

    // Amplitude scale ticks on left side
    const char *tick_labels[] = { "+1", "+0.5", "0", "-0.5", "-1" };
    float tick_positions[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (int i = 0; i < 5; i++) {
        float tick_y = y + height * tick_positions[i];
        // Tick mark
        DrawLineEx((Vector2){x, tick_y}, (Vector2){x + 4, tick_y}, 1.0f, COLOR_GRID_MAJOR);
        // Label (offset to not overlap with border)
        draw_text_with_font(tick_labels[i], x + 6, tick_y - 6, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
    }

    // Channel label in top-right corner
    int label_width = measure_text_with_font(label, FONT_SIZE_OSC_LABEL);
    draw_text_with_font(label, x + width - label_width - 8, y + 4, FONT_SIZE_OSC_LABEL, channel_color);
}

//-----------------------------------------------------------------------------
// Oscilloscope Rendering
//-----------------------------------------------------------------------------

void render_oscilloscope_channel(gui_app_t *app, float x, float y, float width, float height,
                                  int channel, const char *label, Color channel_color) {
    // Store app pointer for font access
    s_osc_app = app;

    // Store bounds for mouse interaction
    if (channel >= 0 && channel < 2) {
        s_osc_bounds[channel] = (Rectangle){x, y, width, height};
        s_osc_bounds_valid[channel] = true;

        // Store actual display width for the processing thread (atomic for thread safety)
        channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
        int new_display_width = (int)width;
        if (new_display_width < 100) new_display_width = 100;  // Minimum reasonable width
        if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
        atomic_store(&trig->display_width, new_display_width);
    }

    // Draw channel grid
    draw_channel_grid(x, y, width, height, label, channel_color, app->settings.show_grid);

    float center_y = y + height / 2.0f;
    float scale = (height / 2.0f) * app->settings.amplitude_scale;

    // Draw trigger level line and position marker if enabled for this channel
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    if (trig->enabled) {
        // Convert trigger level (-2048 to +2047) to normalized (-1 to +1)
        float level_norm = trig->level / 2048.0f;
        float level_y = center_y - level_norm * scale;

        // Clamp to channel bounds
        if (level_y < y) level_y = y;
        if (level_y > y + height) level_y = y + height;

        // Draw dashed trigger level line (semi-transparent channel color)
        Color trig_color = { channel_color.r, channel_color.g, channel_color.b, 128 };
        float dash_len = 8.0f;
        float gap_len = 4.0f;
        for (float dx = 0; dx < width; dx += dash_len + gap_len) {
            float dash_end = dx + dash_len;
            if (dash_end > width) dash_end = width;
            DrawLineEx((Vector2){x + dx, level_y}, (Vector2){x + dash_end, level_y}, 1.0f, trig_color);
        }

        // Draw small trigger arrow on the left edge
        float arrow_size = 6.0f;
        Vector2 arrow_tip = { x + 2, level_y };
        Vector2 arrow_top = { x + 2 + arrow_size, level_y - arrow_size/2 };
        Vector2 arrow_bot = { x + 2 + arrow_size, level_y + arrow_size/2 };
        DrawTriangle(arrow_tip, arrow_bot, arrow_top, trig_color);

        // Draw vertical trigger position marker at actual trigger position (if triggered)
        if (trig->trigger_display_pos >= 0 && trig->trigger_display_pos < (int)width) {
            float trigger_x = x + (float)trig->trigger_display_pos;

            Color marker_color = { channel_color.r, channel_color.g, channel_color.b, 80 };
            DrawLineEx((Vector2){trigger_x, y}, (Vector2){trigger_x, y + height}, 1.0f, marker_color);

            // Draw small "T" marker at the trigger intersection
            Color t_marker_color = { channel_color.r, channel_color.g, channel_color.b, 200 };
            DrawLineEx((Vector2){trigger_x - 4, level_y - 8}, (Vector2){trigger_x + 4, level_y - 8}, 2.0f, t_marker_color);
            DrawLineEx((Vector2){trigger_x, level_y - 8}, (Vector2){trigger_x, level_y - 2}, 2.0f, t_marker_color);
        }
    }

    int display_width = (int)width;
    if (display_width > DISPLAY_BUFFER_SIZE) {
        display_width = DISPLAY_BUFFER_SIZE;
    }

    // Get per-channel display buffer and sample count
    waveform_sample_t *samples;
    size_t samples_available;
    if (channel == 0) {
        samples = app->display_samples_a;
        samples_available = app->display_samples_available_a;
    } else {
        samples = app->display_samples_b;
        samples_available = app->display_samples_available_b;
    }

    if (samples_available == 0) {
        const char *text = "No Signal";
        int text_width = measure_text_with_font(text, FONT_SIZE_OSC_MSG);
        draw_text_with_font(text, x + width/2 - text_width/2, y + height/2 - 12, FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    int samples_to_draw = (samples_available < (size_t)display_width) ? (int)samples_available : display_width;

    // Render based on per-channel display mode
    bool is_phosphor_mode = (trig->scope_mode == SCOPE_MODE_PHOSPHOR);

    if (is_phosphor_mode) {
        // Phosphor display (digital persistence with heatmap)
        int buf_width = (int)width;
        int buf_height = (int)height;

        // Initialize/resize phosphor buffers if needed
        if (buf_width > 0 && buf_height > 0) {
            gui_phosphor_init(app, buf_width, buf_height);
        }

        // Update and render phosphor
        gui_phosphor_update(app, channel, samples, samples_to_draw, app->settings.amplitude_scale);
        gui_phosphor_render(app, channel, x, y);
    } else {
        // Line mode: draw peak envelope as filled area
        Color envelope_color = { channel_color.r, channel_color.g, channel_color.b, 60 };

        for (int px = 0; px < samples_to_draw; px++) {
            waveform_sample_t *sample = &samples[px];
            float px_x = x + px;

            float min_y = center_y - sample->min_val * scale;
            float max_y = center_y - sample->max_val * scale;

            // Clamp to channel bounds
            if (min_y < y) min_y = y;
            if (min_y > y + height) min_y = y + height;
            if (max_y < y) max_y = y;
            if (max_y > y + height) max_y = y + height;

            // Draw vertical line from min to max (envelope)
            if (max_y < min_y) {
                DrawLineV((Vector2){px_x, max_y}, (Vector2){px_x, min_y}, envelope_color);
            }
        }
    }

    // Draw resampled waveform as connected line
    float prev_py = center_y;
    Color waveform_color = is_phosphor_mode ?
        (Color){channel_color.r, channel_color.g, channel_color.b, 200} : channel_color;

    for (int px = 0; px < samples_to_draw; px++) {
        waveform_sample_t *sample = &samples[px];
        float px_x = x + px;

        float py = center_y - sample->value * scale;

        // Clamp to channel bounds
        if (py < y) py = y;
        if (py > y + height) py = y + height;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, waveform_color);
        }

        prev_py = py;
    }
}

//-----------------------------------------------------------------------------
// Mouse Interaction
//-----------------------------------------------------------------------------

void handle_oscilloscope_interaction(gui_app_t *app) {
    if (!app) return;

    Vector2 mouse = GetMousePosition();
    bool mouse_down = IsMouseButtonDown(MOUSE_LEFT_BUTTON);
    bool mouse_pressed = IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
    bool mouse_released = IsMouseButtonReleased(MOUSE_LEFT_BUTTON);

    // Check if mouse is over either oscilloscope
    int hover_channel = -1;
    for (int ch = 0; ch < 2; ch++) {
        if (s_osc_bounds_valid[ch]) {
            Rectangle bounds = s_osc_bounds[ch];
            if (mouse.x >= bounds.x && mouse.x < bounds.x + bounds.width &&
                mouse.y >= bounds.y && mouse.y < bounds.y + bounds.height) {
                hover_channel = ch;
                break;
            }
        }
    }

    // Start dragging on mouse press over oscilloscope
    if (mouse_pressed && hover_channel >= 0) {
        s_dragging_channel = hover_channel;
        // Enable trigger when starting to drag
        channel_trigger_t *trig = (hover_channel == 0) ? &app->trigger_a : &app->trigger_b;
        trig->enabled = true;
    }

    // Update trigger level while dragging
    if (mouse_down && s_dragging_channel >= 0) {
        int ch = s_dragging_channel;
        if (s_osc_bounds_valid[ch]) {
            Rectangle bounds = s_osc_bounds[ch];
            channel_trigger_t *trig = (ch == 0) ? &app->trigger_a : &app->trigger_b;

            // Convert mouse Y to trigger level
            // bounds.y is top (level = +2047)
            // bounds.y + bounds.height is bottom (level = -2048)
            // center is level = 0

            float center_y = bounds.y + bounds.height / 2.0f;
            float half_height = (bounds.height / 2.0f) * app->settings.amplitude_scale;

            // Calculate normalized level (-1 to +1)
            float level_norm = (center_y - mouse.y) / half_height;

            // Clamp to valid range
            if (level_norm > 1.0f) level_norm = 1.0f;
            if (level_norm < -1.0f) level_norm = -1.0f;

            // Convert to 12-bit signed value
            trig->level = (int16_t)(level_norm * 2047.0f);
        }
    }

    // Stop dragging on mouse release
    if (mouse_released) {
        s_dragging_channel = -1;
    }

    // Mouse wheel zoom when hovering over oscilloscope
    if (hover_channel >= 0) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            channel_trigger_t *trig = (hover_channel == 0) ? &app->trigger_a : &app->trigger_b;

            // Smooth zoom: multiply/divide by a factor for each scroll step
            // Using 1.15 gives ~10% zoom per scroll tick
            const float zoom_factor = 1.10f;

            if (wheel > 0.0f) {
                // Scroll up = zoom in (fewer samples per pixel)
                trig->zoom_scale /= zoom_factor;
                if (trig->zoom_scale < ZOOM_SCALE_MIN) {
                    trig->zoom_scale = ZOOM_SCALE_MIN;
                }
            } else {
                // Scroll down = zoom out (more samples per pixel)
                trig->zoom_scale *= zoom_factor;
                if (trig->zoom_scale > ZOOM_SCALE_MAX) {
                    trig->zoom_scale = ZOOM_SCALE_MAX;
                }
            }
        }
    }

    // Change cursor when hovering over oscilloscope
    if (hover_channel >= 0 || s_dragging_channel >= 0) {
        SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
    } else {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }
}

//-----------------------------------------------------------------------------
// Trigger Detection (wrappers to gui_trigger module)
//-----------------------------------------------------------------------------

ssize_t find_trigger_point_from(const int16_t *buf, size_t count,
                                 const channel_trigger_t *trig, size_t min_index) {
    return trigger_find_from_config(buf, count, trig, min_index);
}

ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig) {
    return trigger_find_from_config(buf, count, trig, 1);
}

//-----------------------------------------------------------------------------
// Decimation and Display Buffer Processing (with libsoxr resampling)
//-----------------------------------------------------------------------------

// Resample and decimate a single channel from source buffer to its display buffer
// Calculates waveform value and min/max for peak envelope display
// Uses float decimation for smooth zooming
static size_t resample_to_buffer_smooth(waveform_sample_t *dest, const int16_t *buf,
                                         size_t num_samples, size_t start_idx, float decimation,
                                         size_t target_width) {
    const float scale = 1.0f / 2048.0f;

    // Clamp target width to buffer size
    if (target_width > DISPLAY_BUFFER_SIZE) target_width = DISPLAY_BUFFER_SIZE;
    if (target_width == 0) target_width = DISPLAY_BUFFER_SIZE;

    // Calculate how many source samples we have available
    size_t available = (start_idx < num_samples) ? (num_samples - start_idx) : 0;
    if (available == 0) return 0;

    // Calculate display count based on available samples and decimation
    size_t display_count = (size_t)((float)available / decimation);
    if (display_count > target_width) display_count = target_width;
    if (display_count == 0) return 0;

    // Process each display pixel
    for (size_t i = 0; i < display_count; i++) {
        // Calculate source window for this pixel (floating point positions)
        float src_start_f = (float)start_idx + (float)i * decimation;
        float src_end_f = src_start_f + decimation;

        size_t src_start = (size_t)src_start_f;
        size_t src_end = (size_t)src_end_f;
        if (src_end <= src_start) src_end = src_start + 1;
        if (src_end > num_samples) src_end = num_samples;
        if (src_start >= num_samples) break;

        // Find min/max within this decimation window for peak envelope
        int16_t min_val = buf[src_start];
        int16_t max_val = buf[src_start];

        for (size_t j = src_start + 1; j < src_end; j++) {
            if (buf[j] < min_val) min_val = buf[j];
            if (buf[j] > max_val) max_val = buf[j];
        }

        dest[i].min_val = (float)min_val * scale;
        dest[i].max_val = (float)max_val * scale;

        // Use first sample of decimation window for waveform value
        dest[i].value = (float)buf[src_start] * scale;
    }

    return display_count;
}

bool process_channel_display(gui_app_t *app, const int16_t *buf, size_t num_samples,
                             waveform_sample_t *display_buf, size_t *display_count,
                             channel_trigger_t *trig, int channel) {
    (void)app;      // Unused parameter
    (void)channel;  // Unused parameter (kept for API compatibility)

    // Get display width (set by renderer, defaults to DISPLAY_BUFFER_SIZE)
    // Use atomic_load for thread safety since renderer runs on main thread
    size_t display_width = (size_t)atomic_load(&trig->display_width);
    if (display_width == 0 || display_width > DISPLAY_BUFFER_SIZE) {
        display_width = DISPLAY_BUFFER_SIZE;
    }

    // Get decimation factor from zoom_scale (samples per pixel)
    float decimation = trig->zoom_scale;

    // Calculate max zoom out based on available data
    // We need: display_width * decimation <= num_samples
    float max_decimation = (float)num_samples / (float)display_width;
    if (max_decimation > ZOOM_SCALE_MAX) max_decimation = ZOOM_SCALE_MAX;

    // Clamp decimation to valid range
    if (decimation < ZOOM_SCALE_MIN) {
        decimation = ZOOM_SCALE_MIN;
        trig->zoom_scale = decimation;
    }
    if (decimation > max_decimation) {
        decimation = max_decimation;
        trig->zoom_scale = decimation;
    }

    // How many raw samples we need for the full display at this zoom
    float display_window = (float)display_width * decimation;

    // If trigger is disabled, just show the start of the buffer
    if (!trig->enabled) {
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // When zoomed out so far that display_window >= 90% of buffer,
    // there's no room for trigger positioning - just show from start
    if (display_window >= (float)num_samples * 0.9f) {
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Trigger point should appear at 10% across the display
    size_t trigger_display_pos = display_width / 10;
    float pre_trigger_raw_samples = (float)trigger_display_pos * decimation;
    float post_trigger_raw_samples = display_window - pre_trigger_raw_samples;

    // Calculate the valid search range for triggers
    // Trigger must be at least pre_trigger_raw_samples into the buffer
    // and have enough room for post_trigger_raw_samples after it
    size_t min_trig_pos = (size_t)pre_trigger_raw_samples;
    size_t max_trig_pos = num_samples - (size_t)post_trigger_raw_samples;

    // Check if there's a valid search range
    if (min_trig_pos >= max_trig_pos) {
        // No valid range - display window too large for this buffer
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // Find trigger point starting from minimum valid position
    ssize_t trig_pos = find_trigger_point_from(buf, max_trig_pos, trig, min_trig_pos);

    if (trig_pos < 0) {
        // No trigger found in valid range - hold previous display
        return false;
    }

    // Trigger found in valid range - place it at desired position
    size_t start_pos = (size_t)((float)trig_pos - pre_trigger_raw_samples);
    trig->trigger_display_pos = (int)trigger_display_pos;
    *display_count = resample_to_buffer_smooth(display_buf, buf, num_samples, start_pos, decimation, display_width);
    return true;
}

void gui_oscilloscope_update_display(gui_app_t *app, const int16_t *buf_a,
                                      const int16_t *buf_b, size_t num_samples) {
    // Process channel A
    size_t count_a = app->display_samples_available_a;
    if (process_channel_display(app, buf_a, num_samples,
                                app->display_samples_a, &count_a, &app->trigger_a, 0)) {
        app->display_samples_available_a = count_a;
    }

    // Process channel B
    size_t count_b = app->display_samples_available_b;
    if (process_channel_display(app, buf_b, num_samples,
                                app->display_samples_b, &count_b, &app->trigger_b, 1)) {
        app->display_samples_available_b = count_b;
    }
}
