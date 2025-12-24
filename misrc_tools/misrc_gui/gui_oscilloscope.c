/*
 * MISRC GUI - Oscilloscope and Trigger Implementation
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 */

#include "gui_oscilloscope.h"
#include "gui_ui.h"
#include <math.h>

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
    }

    // Draw channel grid
    draw_channel_grid(x, y, width, height, label, channel_color, app->settings.show_grid);

    float center_y = y + height / 2.0f;
    float scale = (height / 2.0f) * app->settings.amplitude_scale;

    // Draw trigger level line if enabled for this channel
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
    }

    int display_width = (int)width;
    if (display_width > DISPLAY_BUFFER_SIZE) {
        display_width = DISPLAY_BUFFER_SIZE;
    }

    // Get per-channel display buffer and sample count
    waveform_minmax_t *samples;
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

    // Draw waveform as connected lines
    float prev_py = center_y;

    for (int px = 0; px < samples_to_draw; px++) {
        waveform_minmax_t *sample = &samples[px];
        float px_x = x + px;

        // Use average of min/max for smoother line
        float val = (sample->min_val + sample->max_val) * 0.5f;

        float py = center_y - val * scale;

        // Clamp to channel bounds
        if (py < y) py = y;
        if (py > y + height) py = y + height;

        if (px > 0) {
            DrawLineV((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, channel_color);
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
            if (wheel > 0.0f) {
                // Scroll up = zoom in (fewer samples per pixel)
                if (trig->zoom_level < ZOOM_LEVEL_COUNT - 1) {
                    trig->zoom_level++;
                }
            } else {
                // Scroll down = zoom out (more samples per pixel)
                if (trig->zoom_level > 0) {
                    trig->zoom_level--;
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
// Trigger Detection
//-----------------------------------------------------------------------------

ssize_t find_trigger_point(const int16_t *buf, size_t count,
                           const channel_trigger_t *trig) {
    if (!trig->enabled || count < 2) return -1;

    int16_t upper = trig->level + (int16_t)trig->hysteresis;
    int16_t lower = trig->level - (int16_t)trig->hysteresis;

    // Track if we've crossed the level (for hysteresis state machine)
    bool armed = false;

    for (size_t i = 1; i < count; i++) {
        int16_t prev = buf[i - 1];
        int16_t curr = buf[i];

        // Check edge conditions based on trigger type
        bool edge_detected = false;

        if (trig->edge == TRIGGER_EDGE_RISING || trig->edge == TRIGGER_EDGE_BOTH) {
            // For rising edge: arm when below lower threshold
            if (prev < lower) {
                armed = true;
            }
            // Trigger when armed and crossing above upper threshold
            if (armed && prev < upper && curr >= upper) {
                edge_detected = true;
            }
        }

        if (!edge_detected && (trig->edge == TRIGGER_EDGE_FALLING || trig->edge == TRIGGER_EDGE_BOTH)) {
            // For falling edge: arm when above upper threshold
            if (prev > upper) {
                armed = true;
            }
            // Trigger when armed and crossing below lower threshold
            if (armed && prev > lower && curr <= lower) {
                edge_detected = true;
            }
        }

        if (edge_detected) {
            return (ssize_t)i;
        }
    }

    return -1;  // No trigger found
}

//-----------------------------------------------------------------------------
// Decimation and Display Buffer Processing
//-----------------------------------------------------------------------------

// Decimate a single channel from source buffer to its display buffer
static size_t decimate_to_buffer(waveform_minmax_t *dest, const int16_t *buf,
                                  size_t num_samples, size_t start_idx, size_t decimation) {
    const float scale = 1.0f / 2048.0f;
    const size_t target_samples = DISPLAY_BUFFER_SIZE;

    // Calculate how many source samples we have available
    size_t available = (start_idx < num_samples) ? (num_samples - start_idx) : 0;

    // Calculate display count based on available samples and decimation
    size_t display_count = available / decimation;
    if (display_count > target_samples) display_count = target_samples;

    for (size_t i = 0; i < display_count; i++) {
        size_t src_start = start_idx + i * decimation;
        size_t src_end = src_start + decimation;
        if (src_end > num_samples) src_end = num_samples;

        // Find min/max within this decimation window
        int16_t min_val = buf[src_start];
        int16_t max_val = buf[src_start];

        for (size_t j = src_start + 1; j < src_end; j++) {
            if (buf[j] < min_val) min_val = buf[j];
            if (buf[j] > max_val) max_val = buf[j];
        }

        dest[i].min_val = (float)min_val * scale;
        dest[i].max_val = (float)max_val * scale;
    }

    return display_count;
}

bool process_channel_display(gui_app_t *app, const int16_t *buf, size_t num_samples,
                             waveform_minmax_t *display_buf, size_t *display_count,
                             const channel_trigger_t *trig) {
    (void)app;  // Unused parameter

    // Get decimation factor from per-channel zoom level
    int zoom = trig->zoom_level;
    if (zoom < 0) zoom = 0;
    if (zoom >= ZOOM_LEVEL_COUNT) zoom = ZOOM_LEVEL_COUNT - 1;
    size_t decimation = (size_t)ZOOM_SAMPLES_PER_PIXEL[zoom];

    // How many raw samples we need for the full display at this zoom
    size_t display_window = DISPLAY_BUFFER_SIZE * decimation;

    // Find first trigger point in the buffer
    ssize_t trig_pos = find_trigger_point(buf, num_samples, trig);

    // Calculate start position
    size_t start_pos = 0;
    bool triggered = (trig_pos >= 0);

    if (triggered) {
        // Start display from the trigger point
        // The trigger will appear at the left edge of the display
        start_pos = (size_t)trig_pos;

        // Apply holdoff: skip ahead by holdoff samples to find a stable trigger
        // This helps with phase stability for periodic signals
        if (trig->holdoff > 0 && start_pos + trig->holdoff < num_samples) {
            // After holdoff, look for next trigger
            ssize_t next_trig = -1;
            size_t search_start = start_pos + trig->holdoff;

            int16_t upper = trig->level + (int16_t)trig->hysteresis;
            int16_t lower = trig->level - (int16_t)trig->hysteresis;
            bool armed = false;

            for (size_t i = search_start; i < num_samples; i++) {
                int16_t prev = buf[i - 1];
                int16_t curr = buf[i];

                if (trig->edge == TRIGGER_EDGE_RISING || trig->edge == TRIGGER_EDGE_BOTH) {
                    if (prev < lower) armed = true;
                    if (armed && prev < upper && curr >= upper) {
                        next_trig = (ssize_t)i;
                        break;
                    }
                }
                if (next_trig < 0 && (trig->edge == TRIGGER_EDGE_FALLING || trig->edge == TRIGGER_EDGE_BOTH)) {
                    if (prev > upper) armed = true;
                    if (armed && prev > lower && curr <= lower) {
                        next_trig = (ssize_t)i;
                        break;
                    }
                }
            }

            // Use the trigger after holdoff if found and it leaves enough samples
            if (next_trig >= 0 && (size_t)next_trig + display_window <= num_samples) {
                start_pos = (size_t)next_trig;
            }
        }

        // Ensure we have enough samples after the trigger point
        if (start_pos + display_window > num_samples) {
            // Not enough samples after trigger - adjust back
            if (num_samples >= display_window) {
                start_pos = num_samples - display_window;
            } else {
                start_pos = 0;
            }
        }
    } else {
        // No trigger found
        if (trig->enabled && trig->mode == TRIGGER_MODE_NORMAL) {
            // Normal mode: hold the last display, don't update
            return false;
        }
        // Auto mode (or trigger disabled): show start of buffer
        start_pos = 0;
    }

    // Decimate and write to display buffer
    *display_count = decimate_to_buffer(display_buf, buf, num_samples, start_pos, decimation);
    return true;
}

void gui_oscilloscope_update_display(gui_app_t *app, const int16_t *buf_a,
                                      const int16_t *buf_b, size_t num_samples) {
    // Process channel A
    size_t count_a = app->display_samples_available_a;
    if (process_channel_display(app, buf_a, num_samples,
                                app->display_samples_a, &count_a, &app->trigger_a)) {
        app->display_samples_available_a = count_a;
    }

    // Process channel B
    size_t count_b = app->display_samples_available_b;
    if (process_channel_display(app, buf_b, num_samples,
                                app->display_samples_b, &count_b, &app->trigger_b)) {
        app->display_samples_available_b = count_b;
    }
}
