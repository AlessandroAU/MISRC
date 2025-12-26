/*
 * MISRC GUI - Oscilloscope and Trigger Implementation
 *
 * Oscilloscope rendering, trigger detection, and mouse interaction
 */

#include "gui_oscilloscope.h"
#include "gui_phosphor_rt.h"
#include "gui_fft.h"
#include "gui_trigger.h"
#include "gui_popup.h"
#include "gui_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if LIBSOXR_ENABLED
#include <soxr.h>
// Forward declaration for cleanup function
static void gui_oscilloscope_cleanup_resampler(channel_trigger_t *trig);
#endif

//-----------------------------------------------------------------------------
// Grid Settings
//-----------------------------------------------------------------------------

#define GRID_DIVISIONS_Y 4  // Per channel (amplitude)
#define GRID_MIN_SPACING_PX 120  // Minimum pixels between time grid lines
#define GRID_MAX_DIVISIONS 20   // Maximum number of time divisions

//-----------------------------------------------------------------------------
// Static State
//-----------------------------------------------------------------------------

// App pointer for font access
static gui_app_t *s_osc_app = NULL;

// Oscilloscope bounds for mouse interaction (stored per channel)
static Rectangle s_osc_bounds[2] = {0};
static bool s_osc_bounds_valid[2] = {false, false};
static int s_dragging_channel = -1;  // Which channel is being dragged (-1 = none)

// Cleanup oscilloscope resources (static state)
void gui_oscilloscope_cleanup(void) {
    // No static resources to clean up - phosphor cleanup is in gui_phosphor module
}

// Cleanup per-channel resampler resources
void gui_oscilloscope_cleanup_resamplers(gui_app_t *app) {
#if LIBSOXR_ENABLED
    if (app) {
        gui_oscilloscope_cleanup_resampler(&app->trigger_a);
        gui_oscilloscope_cleanup_resampler(&app->trigger_b);
    }
#else
    (void)app;
#endif
}

//-----------------------------------------------------------------------------
// Internal Helper Functions
//-----------------------------------------------------------------------------

// Set the app reference (called from set_render_app in gui_render.c)
void gui_oscilloscope_set_app(gui_app_t *app) {
    s_osc_app = app;
}

// Helper to draw text using the app's font (Inter - for labels)
static void draw_text_with_font(const char *text, float x, float y, int fontSize, Color color) {
    if (s_osc_app && s_osc_app->fonts) {
        Font font = s_osc_app->fonts[0];
        DrawTextEx(font, text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)x, (int)y, fontSize, color);
    }
}

// Helper to draw text using monospace font (Space Mono - for numbers)
static void draw_text_mono(const char *text, float x, float y, int fontSize, Color color) {
    if (s_osc_app && s_osc_app->fonts) {
        Font font = s_osc_app->fonts[1];  // Index 1 = Space Mono
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

// Helper to measure text using monospace font
static int measure_text_mono(const char *text, int fontSize) {
    if (s_osc_app && s_osc_app->fonts) {
        Font font = s_osc_app->fonts[1];  // Index 1 = Space Mono
        Vector2 size = MeasureTextEx(font, text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return MeasureText(text, fontSize);
}

// Snap to 1-2-5 log scale sequence
// Given a rough time division, find the nearest "nice" value in the sequence:
// ...0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100...
static double snap_to_125(double value) {
    if (value <= 0) return 1.0;

    // Find the order of magnitude (power of 10)
    double log_val = log10(value);
    double magnitude = pow(10.0, floor(log_val));
    double normalized = value / magnitude;  // Will be between 1 and 10

    // Snap to 1, 2, or 5 within this magnitude
    double snapped;
    if (normalized < 1.5) {
        snapped = 1.0;
    } else if (normalized < 3.5) {
        snapped = 2.0;
    } else if (normalized < 7.5) {
        snapped = 5.0;
    } else {
        snapped = 10.0;
    }

    return snapped * magnitude;
}

// Format time value with appropriate unit (ns, us, ms, s)
static void format_time_label(char *buf, size_t buf_size, double seconds) {
    if (seconds >= 1.0) {
        snprintf(buf, buf_size, "%.3gs", seconds);
    } else if (seconds >= 0.001) {
        snprintf(buf, buf_size, "%.3gms", seconds * 1000.0);
    } else if (seconds >= 0.000001) {
        snprintf(buf, buf_size, "%.3gus", seconds * 1000000.0);
    } else {
        snprintf(buf, buf_size, "%.3gns", seconds * 1000000000.0);
    }
}

// Draw grid for a single channel with amplitude scale ticks
// zoom_scale: samples per pixel, sample_rate: samples per second
// trigger_enabled: if true, use trigger_display_pos as t=0 reference
// trigger_display_pos: pixel position of trigger point (-1 if not triggered)
static void draw_channel_grid(float x, float y, float width, float height,
                               const char *label, Color channel_color, bool show_grid,
                               float zoom_scale, uint32_t sample_rate,
                               bool trigger_enabled, int trigger_display_pos) {
    // Background slightly darker than main bg
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){25, 25, 30, 255});

    float center_y = y + height / 2;

    if (show_grid) {
        // Time-based vertical grid lines (if we have sample rate info)
        if (sample_rate > 0 && zoom_scale > 0) {
            // Calculate time per pixel
            double time_per_pixel = (double)zoom_scale / (double)sample_rate;

            // Calculate rough time division to get reasonable spacing
            double rough_division = time_per_pixel * (double)GRID_MIN_SPACING_PX;

            // Snap to 1-2-5 sequence
            double time_division = snap_to_125(rough_division);

            // Calculate pixels per division
            double pixels_per_div = time_division / time_per_pixel;

            // Determine the reference point (t=0) in pixels from left edge
            // If trigger is enabled and we have a valid position, use that as t=0
            // Otherwise, t=0 is at the left edge
            double t0_pixel = 0.0;
            if (trigger_enabled && trigger_display_pos >= 0 && trigger_display_pos < (int)width) {
                t0_pixel = (double)trigger_display_pos;
            }

            // Calculate the time offset at the left edge (will be negative if trigger is after left edge)
            double time_at_left = -t0_pixel * time_per_pixel;

            // Find the first grid line position (snap to division boundary)
            // We want the first t such that t >= time_at_left and t is a multiple of time_division
            double first_grid_time;
            if (time_at_left >= 0) {
                first_grid_time = ceil(time_at_left / time_division) * time_division;
            } else {
                first_grid_time = ceil(time_at_left / time_division) * time_division;
            }

            // Draw vertical grid lines at time intervals
            char time_buf[32];
            int division_count = 0;
            for (double t = first_grid_time; division_count < GRID_MAX_DIVISIONS; t += time_division) {
                // Convert time to pixel position
                double px = (t - time_at_left) / time_per_pixel;
                if (px >= (double)width) break;
                if (px < 0) continue;

                float gx = x + (float)px;

                // Draw grid line (use major color for t=0)
                bool is_zero = (fabs(t) < time_division * 0.01);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height},
                         is_zero ? COLOR_GRID_MAJOR : COLOR_GRID);

                // Draw time label (skip if too close to edges)
                if (gx > x + 40 && gx < x + width - 40) {
                    if (is_zero) {
                        // Draw "0" for the trigger point
                        int label_w = measure_text_mono("0", FONT_SIZE_OSC_SCALE);
                        draw_text_mono("0", gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
                    } else {
                        format_time_label(time_buf, sizeof(time_buf), fabs(t));
                        // Add sign prefix for negative times
                        char signed_buf[36];
                        if (t < 0) {
                            snprintf(signed_buf, sizeof(signed_buf), "-%s", time_buf);
                        } else {
                            snprintf(signed_buf, sizeof(signed_buf), "+%s", time_buf);
                        }
                        int label_w = measure_text_mono(signed_buf, FONT_SIZE_OSC_SCALE);
                        draw_text_mono(signed_buf, gx - label_w / 2, y + height - 16, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
                    }
                }
                division_count++;
            }

            // Show time per division in top-right corner (below channel label)
            format_time_label(time_buf, sizeof(time_buf), time_division);
            char div_label[48];
            snprintf(div_label, sizeof(div_label), "%s/div", time_buf);
            int div_label_w = measure_text_mono(div_label, FONT_SIZE_OSC_DIV);
            draw_text_mono(div_label, x + width - div_label_w - 8, y + 26, FONT_SIZE_OSC_DIV, COLOR_TEXT);
        } else {
            // Fallback: fixed divisions when no sample rate available
            const int fixed_divisions = 10;
            for (int i = 1; i < fixed_divisions; i++) {
                float gx = x + (width * i / fixed_divisions);
                DrawLineV((Vector2){gx, y}, (Vector2){gx, y + height}, COLOR_GRID);
            }
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

    // Amplitude scale ticks on left side (use mono font for numbers)
    const char *tick_labels[] = { "+1", "+0.5", "0", "-0.5", "-1" };
    float tick_positions[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (int i = 0; i < 5; i++) {
        float tick_y = y + height * tick_positions[i];
        // Tick mark
        DrawLineEx((Vector2){x, tick_y}, (Vector2){x + 4, tick_y}, 1.0f, COLOR_GRID_MAJOR);
        // Label (offset to not overlap with border)
        draw_text_mono(tick_labels[i], x + 6, tick_y - 7, FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
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

    // Get trigger state for this channel
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;

    // Store bounds for mouse interaction
    if (channel >= 0 && channel < 2) {
        s_osc_bounds[channel] = (Rectangle){x, y, width, height};
        s_osc_bounds_valid[channel] = true;

        // Store actual display width for the processing thread (atomic for thread safety)
        int new_display_width = (int)width;
        if (new_display_width < 100) new_display_width = 100;  // Minimum reasonable width
        if (new_display_width > DISPLAY_BUFFER_SIZE) new_display_width = DISPLAY_BUFFER_SIZE;
        atomic_store(&trig->display_width, new_display_width);
    }

    // Draw channel grid with time-based divisions
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, width, height, label, channel_color, app->settings.show_grid,
                      trig->zoom_scale, sample_rate,
                      trig->enabled, trig->trigger_display_pos);

    float center_y = y + height / 2.0f;
    float scale = (height / 2.0f) * app->settings.amplitude_scale;

    // Draw trigger level line and position marker if enabled for this channel
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
    bool is_split_mode = (trig->scope_mode == SCOPE_MODE_SPLIT);

    if (is_split_mode) {
        // Split mode: phosphor waveform on left half, FFT spectrum on right half
        float half_width = width / 2.0f;
        float divider_x = x + half_width;

        // Left half: phosphor waveform display
        int buf_width = (int)half_width;
        int buf_height = (int)height;
        int waveform_samples = samples_to_draw / 2;
        if (waveform_samples > buf_width) waveform_samples = buf_width;

        if (buf_width > 0 && buf_height > 0) {
            // Get phosphor state for this channel
            phosphor_rt_t *prt = (channel == 0) ? app->phosphor_a : app->phosphor_b;
            if (prt) {
                // Initialize/resize phosphor if needed (no-op if already correct size)
                phosphor_rt_init(prt, buf_width, buf_height);

                // Update phosphor
                phosphor_rt_begin_frame(prt);
                phosphor_rt_draw_waveform(prt, samples, waveform_samples, app->settings.amplitude_scale);
                phosphor_rt_end_frame(prt);

                // Render phosphor to screen
                if (trig->phosphor_color == PHOSPHOR_COLOR_OPACITY) {
                    phosphor_rt_render_opacity(prt, x, y);
                } else {
                    phosphor_rt_render(prt, x, y, false);
                }
            }

            // Draw line overlay on phosphor (semi-transparent)
            float waveform_center_y = y + height / 2.0f;
            float prev_py = waveform_center_y;
            Color waveform_color = {channel_color.r, channel_color.g, channel_color.b, 200};

            for (int px = 0; px < waveform_samples; px++) {
                waveform_sample_t *sample = &samples[px];
                float px_x = x + px;
                float py = waveform_center_y - sample->value * scale;

                if (py < y) py = y;
                if (py > y + height) py = y + height;

                if (px > 0) {
                    DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, waveform_color);
                }
                prev_py = py;
            }

            // Draw time/div label for left half waveform (grid drew it at full width position)
            if (sample_rate > 0 && trig->zoom_scale > 0) {
                double time_per_pixel = (double)trig->zoom_scale / (double)sample_rate;
                double rough_division = time_per_pixel * (double)GRID_MIN_SPACING_PX;
                double time_division = snap_to_125(rough_division);

                char time_buf[32];
                format_time_label(time_buf, sizeof(time_buf), time_division);
                char div_label[48];
                snprintf(div_label, sizeof(div_label), "%s/div", time_buf);
                int div_label_w = measure_text_mono(div_label, FONT_SIZE_OSC_DIV);
                // Position at top-right of left half (not full width)
                draw_text_mono(div_label, x + half_width - div_label_w - 8, y + 26, FONT_SIZE_OSC_DIV, COLOR_TEXT);
            }
        }

        // Draw divider line
        DrawLineEx((Vector2){divider_x, y}, (Vector2){divider_x, y + height}, 2.0f, COLOR_GRID_MAJOR);

        // Right half: FFT spectrum
        fft_state_t *fft = (channel == 0) ? app->fft_a : app->fft_b;
        if (fft && fft->initialized) {
            // Calculate display sample rate (samples per second in display space)
            // zoom_scale = raw samples per display pixel
            // sample_rate = raw samples per second
            // display_sample_rate = display pixels per second = sample_rate / zoom_scale
            uint32_t sr = atomic_load(&app->sample_rate);
            float display_sample_rate = (trig->zoom_scale > 0 && sr > 0) ?
                                        (float)sr / trig->zoom_scale : 0;

            // Process FFT from display samples
            gui_fft_process_display(fft, samples, samples_available, display_sample_rate);

            // Render FFT spectrum to right half
            gui_fft_render(fft, divider_x + 2, y, half_width - 4, height, display_sample_rate, channel_color, app->fonts);
        } else {
            // FFT not available - show message
            const char *text = gui_fft_available() ? "FFT Initializing..." : "FFT Not Available";
            int text_width = measure_text_with_font(text, FONT_SIZE_OSC_MSG);
            draw_text_with_font(text, divider_x + half_width/2 - text_width/2, y + height/2 - 12,
                               FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        }
    } else if (is_phosphor_mode) {
        // Phosphor display (digital persistence with heatmap)
        int buf_width = (int)width;
        int buf_height = (int)height;

        // Get phosphor state for this channel
        phosphor_rt_t *prt = (channel == 0) ? app->phosphor_a : app->phosphor_b;
        if (prt && buf_width > 0 && buf_height > 0) {
            // Initialize/resize phosphor if needed (no-op if already correct size)
            phosphor_rt_init(prt, buf_width, buf_height);

            // Update phosphor
            phosphor_rt_begin_frame(prt);
            phosphor_rt_draw_waveform(prt, samples, samples_to_draw, app->settings.amplitude_scale);
            phosphor_rt_end_frame(prt);

            // Render phosphor to screen
            if (trig->phosphor_color == PHOSPHOR_COLOR_OPACITY) {
                phosphor_rt_render_opacity(prt, x, y);
            } else {
                phosphor_rt_render(prt, x, y, false);
            }
        }
    }

    // Draw resampled waveform as connected line (for line and phosphor modes)
    if (!is_split_mode) {
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
}

//-----------------------------------------------------------------------------
// Mouse Interaction
//-----------------------------------------------------------------------------

void handle_oscilloscope_interaction(gui_app_t *app) {
    if (!app) return;

    // Don't process clicks if UI already consumed them (dropdown, popup, etc.)
    if (gui_ui_click_consumed()) return;

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
                // Allow going below 1.0, will be clamped in processing
                if (trig->zoom_scale < 0.5f) {
                    trig->zoom_scale = 0.5f;
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

    // Change cursor when hovering over oscilloscope (but not when popup is open)
    if (gui_popup_is_open()) {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    } else if (hover_channel >= 0 || s_dragging_channel >= 0) {
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

#if LIBSOXR_ENABLED
// Ensure resampler is initialized with correct decimation ratio
// Returns the resampler handle, creating/recreating if needed
// decimation: the zoom_scale value (samples per pixel, continuous)
static soxr_t ensure_resampler(channel_trigger_t *trig, float decimation) {
    // Check if we need to create or recreate the resampler
    // Recreate if ratio changed by more than 0.1% (to avoid floating point noise)
    float ratio_diff = fabsf(trig->resampler_ratio - decimation);
    bool need_recreate = (trig->resampler == NULL) ||
                         (ratio_diff > decimation * 0.001f);

    if (!need_recreate) {
        return (soxr_t)trig->resampler;
    }

    // Destroy old resampler if exists
    if (trig->resampler) {
        soxr_delete((soxr_t)trig->resampler);
        trig->resampler = NULL;
    }

    // Create new resampler for this decimation ratio
    // in_rate:out_rate = decimation:1
    printf("Creating soxr resampler for decimation %.3f\n", decimation);
    double in_rate = (double)decimation;
    double out_rate = 1.0;

    soxr_error_t soxr_err = NULL;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    // Use SOXR_LQ - low latency works better for non-streaming frame-by-frame processing
    soxr_quality_spec_t qual_spec = soxr_quality_spec(SOXR_QQ, 0);

    soxr_t resampler = soxr_create(in_rate, out_rate, 1, &soxr_err, &io_spec, &qual_spec, NULL);
    if (!resampler || soxr_err) {
        return NULL;
    }

    trig->resampler = resampler;
    trig->resampler_ratio = decimation;

    return resampler;
}

// Cleanup resampler for a channel (call on shutdown)
static void gui_oscilloscope_cleanup_resampler(channel_trigger_t *trig) {
    if (trig && trig->resampler) {
        soxr_delete((soxr_t)trig->resampler);
        trig->resampler = NULL;
        trig->resampler_ratio = 0.0f;
    }
}
#endif

// Resample a single channel from source buffer to display buffer using libsoxr
// Proper anti-alias filtering is applied during resampling
static size_t resample_to_buffer_smooth(channel_trigger_t *trig, waveform_sample_t *dest,
                                         const int16_t *buf, size_t num_samples,
                                         size_t start_idx, float decimation,
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

    // Calculate how many source samples we actually need for this display width
    size_t source_samples_needed = (size_t)ceilf((float)display_count * decimation);
    if (source_samples_needed > available) source_samples_needed = available;

#if LIBSOXR_ENABLED
    // Bypass soxr for 1:1 ratio (no resampling needed)
    // Use small epsilon to handle floating point imprecision
    if (decimation >= 0.999f && decimation <= 1.001f) {
        size_t count = (display_count < source_samples_needed) ? display_count : source_samples_needed;
        for (size_t i = 0; i < count; i++) {
            dest[i].value = (float)buf[start_idx + i] * scale;
        }
        return count;
    }

    // Temporary buffers for float conversion
    static float temp_input[DISPLAY_BUFFER_SIZE * 256];  // Max ~256x decimation
    static float temp_output[DISPLAY_BUFFER_SIZE];

    // Limit input size to our temp buffer
    size_t max_input = sizeof(temp_input) / sizeof(temp_input[0]);
    if (source_samples_needed > max_input) {
        source_samples_needed = max_input;
        display_count = (size_t)((float)source_samples_needed / decimation);
        if (display_count == 0) display_count = 1;
    }

    // Convert input to float with scaling
    for (size_t i = 0; i < source_samples_needed; i++) {
        temp_input[i] = (float)buf[start_idx + i] * scale;
    }

    // Get or create resampler for this decimation ratio
    soxr_t resampler = ensure_resampler(trig, decimation);
    if (!resampler) {
        return 0;
    }

    // Clear resampler state for fresh data each frame
    // (we're not doing continuous streaming, each frame is independent)
    soxr_clear(resampler);

    // Process through resampler
    size_t in_done = 0, out_done = 0;
    soxr_error_t soxr_err = soxr_process(resampler,
                                          temp_input, source_samples_needed, &in_done,
                                          temp_output, display_count, &out_done);

    if (soxr_err || out_done == 0) {
        return 0;
    }

    // Copy to destination
    for (size_t i = 0; i < out_done; i++) {
        dest[i].value = temp_output[i];
    }

    return out_done;
#else
    // No libsoxr: simple point sampling (no anti-aliasing)
    for (size_t i = 0; i < display_count; i++) {
        size_t src_idx = start_idx + (size_t)((float)i * decimation);
        if (src_idx >= num_samples) src_idx = num_samples - 1;
        dest[i].value = (float)buf[src_idx] * scale;
    }
    return display_count;
#endif
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
        trig->zoom_scale = decimation;  // Write back to snap to 1.0
    }
    if (decimation > max_decimation) {
        decimation = max_decimation;
        // Don't write back - this is a temporary limit based on available data,
        // not a user preference change. Let the user's zoom level persist.
    }

    // How many raw samples we need for the full display at this zoom
    float display_window = (float)display_width * decimation;

    // If trigger is disabled, just show the start of the buffer
    if (!trig->enabled) {
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, 0, decimation, display_width);
        return true;
    }

    // When zoomed out so far that display_window >= 90% of buffer,
    // there's no room for trigger positioning - just show from start
    if (display_window >= (float)num_samples * 0.9f) {
        trig->trigger_display_pos = -1;
        *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, 0, decimation, display_width);
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
        *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, 0, decimation, display_width);
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
    *display_count = resample_to_buffer_smooth(trig, display_buf, buf, num_samples, start_pos, decimation, display_width);
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
