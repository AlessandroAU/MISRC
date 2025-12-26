/*
 * MISRC GUI - Panel Abstraction Implementation
 *
 * Provides render dispatch and state management for panel views.
 */

#include "gui_panel.h"
#include "gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_fft.h"
#include "gui_phosphor_rt.h"
#include "gui_ui.h"
#include <stdlib.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// View Type Names (for UI dropdowns)
//-----------------------------------------------------------------------------

static const char* s_view_names[] = {
    [PANEL_VIEW_WAVEFORM_LINE] = "Line",
    [PANEL_VIEW_WAVEFORM_PHOSPHOR] = "Phosphor",
    [PANEL_VIEW_FFT] = "FFT",
};

const char* panel_view_type_name(panel_view_type_t type) {
    if (type < PANEL_VIEW_COUNT) return s_view_names[type];
    return "Unknown";
}

bool panel_view_type_available(panel_view_type_t type) {
    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            return true;
        case PANEL_VIEW_FFT:
            return gui_fft_available();
        default:
            return false;
    }
}

//-----------------------------------------------------------------------------
// View State Management
//-----------------------------------------------------------------------------

void* panel_create_view_state(panel_view_type_t type) {
    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            // Waveform views use shared phosphor from app, no per-panel state
            return NULL;
        case PANEL_VIEW_FFT: {
            fft_state_t *fft = malloc(sizeof(fft_state_t));
            if (fft) {
                if (gui_fft_init(fft)) {
                    return fft;
                }
                free(fft);
            }
            return NULL;
        }
        default:
            return NULL;
    }
}

void panel_destroy_view_state(panel_view_type_t type, void *state) {
    if (!state) return;

    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            // No state to destroy
            break;
        case PANEL_VIEW_FFT: {
            fft_state_t *fft = (fft_state_t*)state;
            gui_fft_cleanup(fft);
            free(fft);
            break;
        }
        default:
            break;
    }
}

void panel_clear_view_state(panel_view_type_t type, void *state) {
    if (!state) return;

    switch (type) {
        case PANEL_VIEW_WAVEFORM_LINE:
        case PANEL_VIEW_WAVEFORM_PHOSPHOR:
            break;
        case PANEL_VIEW_FFT: {
            fft_state_t *fft = (fft_state_t*)state;
            gui_fft_clear(fft);
            break;
        }
        default:
            break;
    }
}

//-----------------------------------------------------------------------------
// Panel Render Functions
//-----------------------------------------------------------------------------

// Forward declaration of internal render functions
static void render_waveform_line_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);
static void render_waveform_phosphor_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);
static void render_fft_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color);

// Render function table
static panel_render_fn s_render_fns[] = {
    [PANEL_VIEW_WAVEFORM_LINE] = render_waveform_line_panel,
    [PANEL_VIEW_WAVEFORM_PHOSPHOR] = render_waveform_phosphor_panel,
    [PANEL_VIEW_FFT] = render_fft_panel,
};

panel_render_fn panel_get_render_fn(panel_view_type_t type) {
    if (type < PANEL_VIEW_COUNT) return s_render_fns[type];
    return NULL;
}

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (Line Mode)
//-----------------------------------------------------------------------------

static void render_waveform_line_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    (void)state;  // Line mode doesn't use phosphor state

    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    const char *label = (channel == 0) ? "CH A" : "CH B";

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      trig->zoom_scale, sample_rate,
                      trig->enabled, trig->trigger_display_pos);

    // Get display samples
    waveform_sample_t *samples;
    size_t samples_available;
    if (channel == 0) {
        samples = app->display_samples_a;
        samples_available = app->display_samples_available_a;
    } else {
        samples = app->display_samples_b;
        samples_available = app->display_samples_available_b;
    }

    if (samples_available == 0) return;

    int display_width = (int)w;
    if (display_width > DISPLAY_BUFFER_SIZE) display_width = DISPLAY_BUFFER_SIZE;
    int samples_to_draw = (samples_available < (size_t)display_width) ?
                          (int)samples_available : display_width;

    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;

    // Draw waveform as connected line
    float prev_py = center_y;
    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + px;
        float py = center_y - samples[px].value * scale;

        // Clamp to bounds
        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, color);
        }
        prev_py = py;
    }
}

//-----------------------------------------------------------------------------
// Waveform Panel Rendering (Phosphor Mode)
//-----------------------------------------------------------------------------

static void render_waveform_phosphor_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {
    (void)state;  // Uses shared phosphor from app

    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    const char *label = (channel == 0) ? "CH A" : "CH B";

    // Draw grid with labels first
    uint32_t sample_rate = atomic_load(&app->sample_rate);
    draw_channel_grid(x, y, w, h, label, color, app->settings.show_grid,
                      trig->zoom_scale, sample_rate,
                      trig->enabled, trig->trigger_display_pos);

    // Get display samples
    waveform_sample_t *samples;
    size_t samples_available;
    if (channel == 0) {
        samples = app->display_samples_a;
        samples_available = app->display_samples_available_a;
    } else {
        samples = app->display_samples_b;
        samples_available = app->display_samples_available_b;
    }

    if (samples_available == 0) return;

    int buf_width = (int)w;
    int buf_height = (int)h;
    if (buf_width <= 0 || buf_height <= 0) return;

    int samples_to_draw = (samples_available < (size_t)buf_width) ?
                          (int)samples_available : buf_width;

    // Get phosphor state for this channel
    phosphor_rt_t *prt = (channel == 0) ? app->phosphor_a : app->phosphor_b;
    if (prt) {
        // Initialize/resize phosphor if needed
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

    // Draw line overlay on phosphor (semi-transparent)
    float center_y = y + h / 2.0f;
    float scale = (h / 2.0f) * app->settings.amplitude_scale;
    float prev_py = center_y;
    Color waveform_color = {color.r, color.g, color.b, 200};

    for (int px = 0; px < samples_to_draw; px++) {
        float px_x = x + px;
        float py = center_y - samples[px].value * scale;

        if (py < y) py = y;
        if (py > y + h) py = y + h;

        if (px > 0) {
            DrawLineEx((Vector2){px_x - 1, prev_py}, (Vector2){px_x, py}, 1.0f, waveform_color);
        }
        prev_py = py;
    }
}

//-----------------------------------------------------------------------------
// FFT Panel Rendering
//-----------------------------------------------------------------------------

static void render_fft_panel(gui_app_t *app, int channel,
    float x, float y, float w, float h, void *state, Color color) {

    fft_state_t *fft = (fft_state_t*)state;

    // If no state provided, try to use the app's FFT state (for backward compat)
    if (!fft) {
        fft = (channel == 0) ? app->fft_a : app->fft_b;
    }

    if (!fft || !fft->initialized) {
        // FFT not available - show message
        const char *text = gui_fft_available() ? "FFT Initializing..." : "FFT Not Available";
        int text_width = MeasureText(text, FONT_SIZE_OSC_MSG);
        DrawText(text, (int)(x + w/2 - text_width/2), (int)(y + h/2 - 12),
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        return;
    }

    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;

    // Get display samples
    waveform_sample_t *samples;
    size_t samples_available;
    if (channel == 0) {
        samples = app->display_samples_a;
        samples_available = app->display_samples_available_a;
    } else {
        samples = app->display_samples_b;
        samples_available = app->display_samples_available_b;
    }

    // Calculate display sample rate
    uint32_t sr = atomic_load(&app->sample_rate);
    float display_sample_rate = (trig->zoom_scale > 0 && sr > 0) ?
                                (float)sr / trig->zoom_scale : 0;

    // Process and render FFT
    gui_fft_process_display(fft, samples, samples_available, display_sample_rate);
    gui_fft_render(fft, x, y, w, h, display_sample_rate, color, app->fonts);
}

//-----------------------------------------------------------------------------
// Channel Panel Rendering (Main Entry Point)
//-----------------------------------------------------------------------------

void render_channel_panels(gui_app_t *app, int channel,
                           float x, float y, float width, float height,
                           Color channel_color) {

    // Access the inline struct from gui_app_t (uses int for view types)
    bool split;
    panel_view_type_t left_view, right_view;
    void *left_state, *right_state;

    if (channel == 0) {
        split = app->panel_config_a.split;
        left_view = (panel_view_type_t)app->panel_config_a.left_view;
        right_view = (panel_view_type_t)app->panel_config_a.right_view;
        left_state = app->panel_config_a.left_state;
        right_state = app->panel_config_a.right_state;
    } else {
        split = app->panel_config_b.split;
        left_view = (panel_view_type_t)app->panel_config_b.left_view;
        right_view = (panel_view_type_t)app->panel_config_b.right_view;
        left_state = app->panel_config_b.left_state;
        right_state = app->panel_config_b.right_state;
    }

    if (!split) {
        // Single panel - render at full width
        panel_render_fn fn = panel_get_render_fn(left_view);
        if (fn) {
            fn(app, channel, x, y, width, height, left_state, channel_color);
        }
    } else {
        // Split panels - divide width between left and right
        float half_width = width / 2.0f;
        float divider_x = x + half_width;

        // Left panel
        panel_render_fn left_fn = panel_get_render_fn(left_view);
        if (left_fn) {
            left_fn(app, channel, x, y, half_width - 1, height,
                   left_state, channel_color);
        }

        // Divider line
        DrawLineEx((Vector2){divider_x, y}, (Vector2){divider_x, y + height},
                   2.0f, COLOR_GRID_MAJOR);

        // Right panel
        panel_render_fn right_fn = panel_get_render_fn(right_view);
        if (right_fn) {
            right_fn(app, channel, divider_x + 2, y, half_width - 3, height,
                    right_state, channel_color);
        }
    }
}

//-----------------------------------------------------------------------------
// Panel Configuration Helpers
//-----------------------------------------------------------------------------

void panel_config_init_default(channel_panel_config_t *config) {
    config->split = false;
    config->left_view = PANEL_VIEW_WAVEFORM_PHOSPHOR;
    config->right_view = PANEL_VIEW_FFT;
    config->left_state = NULL;
    config->right_state = NULL;
}

void panel_config_cleanup(channel_panel_config_t *config) {
    if (config->left_state) {
        panel_destroy_view_state(config->left_view, config->left_state);
        config->left_state = NULL;
    }
    if (config->right_state) {
        panel_destroy_view_state(config->right_view, config->right_state);
        config->right_state = NULL;
    }
}

void panel_config_set_left_view(channel_panel_config_t *config, panel_view_type_t type) {
    if (config->left_view == type) return;

    // Destroy old state
    if (config->left_state) {
        panel_destroy_view_state(config->left_view, config->left_state);
        config->left_state = NULL;
    }

    // Set new view and create state
    config->left_view = type;
    config->left_state = panel_create_view_state(type);
}

void panel_config_set_right_view(channel_panel_config_t *config, panel_view_type_t type) {
    if (config->right_view == type) return;

    // Destroy old state
    if (config->right_state) {
        panel_destroy_view_state(config->right_view, config->right_state);
        config->right_state = NULL;
    }

    // Set new view and create state
    config->right_view = type;
    if (config->split) {
        config->right_state = panel_create_view_state(type);
    }
}

void panel_config_set_split(channel_panel_config_t *config, bool split) {
    if (config->split == split) return;

    config->split = split;

    if (split) {
        // Entering split mode - create right panel state
        config->right_state = panel_create_view_state(config->right_view);
    } else {
        // Leaving split mode - destroy right panel state
        if (config->right_state) {
            panel_destroy_view_state(config->right_view, config->right_state);
            config->right_state = NULL;
        }
    }
}
