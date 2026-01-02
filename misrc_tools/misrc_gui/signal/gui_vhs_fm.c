/*
 * MISRC GUI - VHS FM Video Demodulator Module
 *
 * Stub/boilerplate - FM demodulation to be implemented.
 */

#include "gui_vhs_fm.h"
#include "../visualization/gui_text.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------

#define FM_PI 3.14159265358979323846


//-----------------------------------------------------------------------------
// Initialization and Cleanup
//-----------------------------------------------------------------------------

bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder) {
    if (!decoder) return false;

    memset(decoder, 0, sizeof(vhs_fm_decoder_t));

    // Set default format
    decoder->state.format = VHS_FM_FORMAT_PAL;
    decoder->frame_width = VHS_FM_FRAME_WIDTH;
    atomic_store(&decoder->frame_height, VHS_FM_PAL_HEIGHT);
    decoder->field_height = VHS_FM_PAL_FIELD_HEIGHT;

    // Allocate scope buffers
    for (int i = 0; i < VHS_FM_SCOPE_COUNT; i++) {
        decoder->scope.buffer[i] = calloc(VHS_FM_SCOPE_BUFFER_SIZE, sizeof(float));
        if (!decoder->scope.buffer[i]) {
            gui_vhs_fm_cleanup(decoder);
            return false;
        }
    }

    // Initialize scope state
    decoder->display_mode = VHS_FM_DISPLAY_SCOPE;
    decoder->scope.active_trace = VHS_FM_SCOPE_DEMOD;
    decoder->scope.zoom_scale = 1.0f;
    decoder->scope.y_scale = 1.0f;
    decoder->scope.trigger_enabled = true;
    decoder->scope.trigger_level = 0.0f;

    printf("[VHS FM] Initialized (stub)\n");
    return true;
}

void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;

    for (int i = 0; i < VHS_FM_SCOPE_COUNT; i++) {
        free(decoder->scope.buffer[i]);
        decoder->scope.buffer[i] = NULL;
    }

    memset(decoder, 0, sizeof(vhs_fm_decoder_t));
}

void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;

    for (int i = 0; i < VHS_FM_SCOPE_COUNT; i++) {
        if (decoder->scope.buffer[i]) {
            memset(decoder->scope.buffer[i], 0, VHS_FM_SCOPE_BUFFER_SIZE * sizeof(float));
        }
    }
    decoder->scope.write_idx = 0;
    decoder->scope.buffer_full = false;
    decoder->scope.triggered = false;

}

//-----------------------------------------------------------------------------
// Processing - TODO: Implement FM demodulation
//-----------------------------------------------------------------------------

void gui_vhs_fm_process_buffer(vhs_fm_decoder_t *decoder,
                                const int16_t *buf, size_t count) {
    if (!decoder || !buf || count < 100) return;

    // TODO: Implement FM demodulation here
    //
    // For now, just store raw samples in scope buffers
    for (size_t i = 0; i < count; i++) {
        float sample = (float)buf[i] / 32768.0f;

        int idx = decoder->scope.write_idx;

        // Store raw input
        if (decoder->scope.buffer[VHS_FM_SCOPE_RAW]) {
            decoder->scope.buffer[VHS_FM_SCOPE_RAW][idx] = sample;
        }

        // Placeholder: copy raw to demod for now
        if (decoder->scope.buffer[VHS_FM_SCOPE_DEMOD]) {
            decoder->scope.buffer[VHS_FM_SCOPE_DEMOD][idx] = sample;
        }

        // Trigger detection
        if (decoder->scope.trigger_enabled && !decoder->scope.triggered) {
            int prev_idx = (idx - 1 + VHS_FM_SCOPE_BUFFER_SIZE) % VHS_FM_SCOPE_BUFFER_SIZE;
            float prev = decoder->scope.buffer[VHS_FM_SCOPE_DEMOD][prev_idx];
            if (prev < decoder->scope.trigger_level && sample >= decoder->scope.trigger_level) {
                decoder->scope.triggered_idx = idx;
                decoder->scope.triggered = true;
            }
        }

        decoder->scope.write_idx = (idx + 1) % VHS_FM_SCOPE_BUFFER_SIZE;
        if (idx == VHS_FM_SCOPE_BUFFER_SIZE - 1) {
            decoder->scope.buffer_full = true;
            decoder->scope.triggered = false;
        }
    }
}

//-----------------------------------------------------------------------------
// Rendering - Scope View
//-----------------------------------------------------------------------------

static void render_scope(vhs_fm_decoder_t *decoder, float x, float y, float width, float height) {
    if (!decoder) return;

    // Background
    DrawRectangle((int)x, (int)y, (int)width, (int)height, (Color){20, 20, 25, 255});

    // Get raw buffer
    float *buf = decoder->scope.buffer[VHS_FM_SCOPE_RAW];
    if (!buf || !decoder->scope.buffer_full) return;

    // Simple: draw most recent samples, 1 sample per pixel
    int start_idx = (decoder->scope.write_idx - (int)width + VHS_FM_SCOPE_BUFFER_SIZE) % VHS_FM_SCOPE_BUFFER_SIZE;

    // Draw waveform (fixed scale: -1 to +1)
    Color trace_color = (Color){100, 255, 100, 255};

    float prev_x = x;
    float prev_y = y + height / 2;

    for (int px = 0; px < (int)width; px++) {
        int sample_idx = (start_idx + px) % VHS_FM_SCOPE_BUFFER_SIZE;
        float val = buf[sample_idx];

        // Map -1..+1 to screen
        float normalized = (val + 1.0f) / 2.0f;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        float draw_x = x + px;
        float draw_y = y + height * (1.0f - normalized);

        if (px > 0) {
            DrawLineV((Vector2){prev_x, prev_y}, (Vector2){draw_x, draw_y}, trace_color);
        }

        prev_x = draw_x;
        prev_y = draw_y;
    }
}

void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder,
                              float x, float y, float width, float height) {
    if (!decoder) return;
    render_scope(decoder, x, y, width, height);
}

//-----------------------------------------------------------------------------
// Input Handling
//-----------------------------------------------------------------------------

static bool vhs_fm_handle_click(void *state, struct gui_app *app, int channel,
                                 Vector2 pos, Rectangle bounds) {
    (void)state;
    (void)app;
    (void)channel;
    (void)pos;
    (void)bounds;
    return false;
}

static bool vhs_fm_handle_scroll(void *state, float delta, Rectangle bounds) {
    (void)state;
    (void)delta;
    (void)bounds;
    return false;
}

//-----------------------------------------------------------------------------
// Stub implementations
//-----------------------------------------------------------------------------

void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder) {
    (void)decoder;
}

void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select) {
    if (!decoder) return;
    decoder->state.format = (format_select == 1) ? VHS_FM_FORMAT_NTSC : VHS_FM_FORMAT_PAL;
}

vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder) {
    return decoder ? decoder->state.format : VHS_FM_FORMAT_UNKNOWN;
}

const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder) {
    if (!decoder) return "Unknown";
    switch (decoder->state.format) {
        case VHS_FM_FORMAT_PAL: return "PAL";
        case VHS_FM_FORMAT_NTSC: return "NTSC";
        default: return "Unknown";
    }
}

//-----------------------------------------------------------------------------
// Panel Interface
//-----------------------------------------------------------------------------

static void *vhs_fm_vtable_create(void) {
    vhs_fm_decoder_t *decoder = calloc(1, sizeof(vhs_fm_decoder_t));
    if (decoder && gui_vhs_fm_init(decoder)) {
        return decoder;
    }
    free(decoder);
    return NULL;
}

static void vhs_fm_vtable_destroy(void *state) {
    if (state) {
        gui_vhs_fm_cleanup((vhs_fm_decoder_t *)state);
        free(state);
    }
}

static void vhs_fm_vtable_clear(void *state) {
    if (state) {
        gui_vhs_fm_reset((vhs_fm_decoder_t *)state);
    }
}

static void vhs_fm_vtable_process(void *state, const int16_t *samples,
                                   size_t count, uint32_t sample_rate) {
    (void)sample_rate;
    if (!state || !samples || count == 0) return;
    gui_vhs_fm_process_buffer((vhs_fm_decoder_t *)state, samples, count);
}

static void vhs_fm_vtable_render(void *state, struct gui_app *app, int channel,
                                  Rectangle bounds, Color color) {
    (void)app;
    (void)channel;
    (void)color;
    gui_vhs_fm_render_frame((vhs_fm_decoder_t *)state, bounds.x, bounds.y, bounds.width, bounds.height);
}

static const panel_vtable_t s_vhs_fm_vtable = {
    .name = "VHS FM",
    .create = vhs_fm_vtable_create,
    .destroy = vhs_fm_vtable_destroy,
    .clear = vhs_fm_vtable_clear,
    .process = vhs_fm_vtable_process,
    .render = vhs_fm_vtable_render,
    .render_overlay = NULL,
    .handle_click = vhs_fm_handle_click,
    .handle_scroll = vhs_fm_handle_scroll,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_vhs_fm_panel_register(void) {
    panel_register(PANEL_VIEW_VHS_FM, &s_vhs_fm_vtable);
}
