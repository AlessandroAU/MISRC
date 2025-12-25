/*
 * MISRC GUI - Custom Rendering
 *
 * VU meter rendering and custom element callbacks
 */

#include "gui_render.h"
#include "gui_oscilloscope.h"
#include "gui_ui.h"
#include "raylib.h"
#include <math.h>
#include <stdio.h>

// Forward declaration of app pointer storage (for font access)
static gui_app_t *g_render_app = NULL;

// Helper to draw text using the app's font
static void draw_text_with_font(const char *text, float x, float y, int fontSize, Color color) {
    if (g_render_app && g_render_app->fonts) {
        Font font = g_render_app->fonts[0];
        DrawTextEx(font, text, (Vector2){x, y}, (float)fontSize, 1.0f, color);
    } else {
        DrawText(text, (int)x, (int)y, fontSize, color);
    }
}

// Helper to measure text using the app's font
static int measure_text_with_font(const char *text, int fontSize) {
    if (g_render_app && g_render_app->fonts) {
        Font font = g_render_app->fonts[0];
        Vector2 size = MeasureTextEx(font, text, (float)fontSize, 1.0f);
        return (int)size.x;
    }
    return MeasureText(text, fontSize);
}

// Helper to draw one direction of the meter bar with gradient
static void draw_meter_bar(float meter_x, float meter_width, float center_y,
                           float half_height, float level, bool going_up) {
    if (level < 0) level = 0;
    if (level > 1) level = 1;

    float bar_extent = level * half_height;
    if (bar_extent < 1) return;

    // Gradient thresholds
    float green_zone = half_height * 0.6f;
    float yellow_zone = half_height * 0.25f;
    float red_threshold = green_zone + yellow_zone;

    // Green portion (closest to center)
    float green_h = (bar_extent < green_zone) ? bar_extent : green_zone;
    if (going_up) {
        DrawRectangle((int)meter_x + 1, (int)(center_y - green_h),
                     (int)meter_width - 2, (int)green_h, COLOR_METER_GREEN);
    } else {
        DrawRectangle((int)meter_x + 1, (int)center_y,
                     (int)meter_width - 2, (int)green_h, COLOR_METER_GREEN);
    }

    // Yellow portion
    if (bar_extent > green_zone) {
        float yellow_h = bar_extent - green_zone;
        if (yellow_h > yellow_zone) yellow_h = yellow_zone;
        if (going_up) {
            DrawRectangle((int)meter_x + 1, (int)(center_y - green_zone - yellow_h),
                         (int)meter_width - 2, (int)yellow_h, COLOR_METER_YELLOW);
        } else {
            DrawRectangle((int)meter_x + 1, (int)(center_y + green_zone),
                         (int)meter_width - 2, (int)yellow_h, COLOR_METER_YELLOW);
        }
    }

    // Red portion (furthest from center)
    if (bar_extent > red_threshold) {
        float red_h = bar_extent - red_threshold;
        if (going_up) {
            DrawRectangle((int)meter_x + 1, (int)(center_y - bar_extent),
                         (int)meter_width - 2, (int)red_h, COLOR_METER_RED);
        } else {
            DrawRectangle((int)meter_x + 1, (int)(center_y + red_threshold),
                         (int)meter_width - 2, (int)red_h, COLOR_METER_RED);
        }
    }
}

// Bipolar VU meter for AC-coupled signals
// Shows separate positive (upward) and negative (downward) levels from center
// Positive and negative bars can have different heights for asymmetric signals
void render_vu_meter(float x, float y, float width, float height,
                     vu_meter_state_t *meter, const char *label,
                     bool is_clipping_pos, bool is_clipping_neg, Color channel_color) {
    (void)label;         // Not used - label comes from layout
    (void)channel_color; // Not used

    float padding = 4.0f;
    float clip_box_height = 16.0f;

    float meter_x = x + padding;
    float meter_width = width - 2 * padding;
    float meter_y = y + padding + clip_box_height + 4;
    float meter_height = height - 2 * padding - 2 * (clip_box_height + 4);

    float center_y = meter_y + meter_height / 2.0f;
    float half_height = meter_height / 2.0f;

    // Background
    DrawRectangle((int)meter_x, (int)meter_y, (int)meter_width, (int)meter_height, COLOR_METER_BG);

    // Center line (0 reference)
    DrawLineEx((Vector2){meter_x, center_y}, (Vector2){meter_x + meter_width, center_y},
               2.0f, (Color){100, 100, 120, 200});

    // Draw positive bar (upward from center) - independent level
    draw_meter_bar(meter_x, meter_width, center_y, half_height, meter->level_pos, true);

    // Draw negative bar (downward from center) - independent level
    draw_meter_bar(meter_x, meter_width, center_y, half_height, meter->level_neg, false);

    // Peak hold indicator for positive (white line going up)
    if (meter->peak_pos > 0.02f) {
        float peak_y = center_y - meter->peak_pos * half_height;
        DrawRectangle((int)meter_x, (int)peak_y - 1, (int)meter_width, 3, WHITE);
    }

    // Peak hold indicator for negative (white line going down)
    if (meter->peak_neg > 0.02f) {
        float peak_y = center_y + meter->peak_neg * half_height;
        DrawRectangle((int)meter_x, (int)peak_y - 1, (int)meter_width, 3, WHITE);
    }

    // Border
    DrawRectangleLinesEx((Rectangle){meter_x, meter_y, meter_width, meter_height}, 1, COLOR_GRID_MAJOR);

    // Scale ticks and labels inside the meter, center aligned
    const char *tick_labels[] = { "+1", "+.5", "0", "-.5", "-1" };
    float tick_positions[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (int i = 0; i < 5; i++) {
        float tick_y = meter_y + meter_height * tick_positions[i];
        // Tick marks on both sides
        DrawLineEx((Vector2){meter_x, tick_y}, (Vector2){meter_x + 3, tick_y}, 1.0f, COLOR_GRID_MAJOR);
        DrawLineEx((Vector2){meter_x + meter_width - 3, tick_y}, (Vector2){meter_x + meter_width, tick_y}, 1.0f, COLOR_GRID_MAJOR);
        // Label centered inside meter
        int tw = measure_text_with_font(tick_labels[i], FONT_SIZE_VU_SCALE);
        draw_text_with_font(tick_labels[i], meter_x + meter_width/2 - tw/2, tick_y - 5, FONT_SIZE_VU_SCALE, COLOR_TEXT_DIM);
    }

    // Positive clip indicator (top)
    float pos_clip_y = y + padding;
    Color pos_clip_bg = is_clipping_pos ? COLOR_CLIP_RED : (Color){40, 25, 25, 255};
    Color pos_clip_border = is_clipping_pos ? (Color){255, 100, 100, 255} : (Color){80, 50, 50, 255};
    DrawRectangle((int)meter_x, (int)pos_clip_y, (int)meter_width, (int)clip_box_height, pos_clip_bg);
    DrawRectangleLinesEx((Rectangle){meter_x, pos_clip_y, meter_width, clip_box_height}, 1, pos_clip_border);
    const char *pos_text = is_clipping_pos ? "+CLIP" : "+";
    int pos_tw = measure_text_with_font(pos_text, FONT_SIZE_VU_CLIP);
    draw_text_with_font(pos_text, meter_x + meter_width/2 - pos_tw/2, pos_clip_y + 2, FONT_SIZE_VU_CLIP,
             is_clipping_pos ? WHITE : (Color){120, 80, 80, 255});

    // Negative clip indicator (bottom)
    float neg_clip_y = meter_y + meter_height + 4;
    Color neg_clip_bg = is_clipping_neg ? COLOR_CLIP_RED : (Color){25, 25, 40, 255};
    Color neg_clip_border = is_clipping_neg ? (Color){100, 100, 255, 255} : (Color){50, 50, 80, 255};
    DrawRectangle((int)meter_x, (int)neg_clip_y, (int)meter_width, (int)clip_box_height, neg_clip_bg);
    DrawRectangleLinesEx((Rectangle){meter_x, neg_clip_y, meter_width, clip_box_height}, 1, neg_clip_border);
    const char *neg_text = is_clipping_neg ? "-CLIP" : "-";
    int neg_tw = measure_text_with_font(neg_text, FONT_SIZE_VU_CLIP);
    draw_text_with_font(neg_text, meter_x + meter_width/2 - neg_tw/2, neg_clip_y + 2, FONT_SIZE_VU_CLIP,
             is_clipping_neg ? WHITE : (Color){80, 80, 120, 255});
}

// Set the app reference for custom rendering (used by font helpers)
void set_render_app(gui_app_t *app) {
    g_render_app = app;
}

// Custom element callback for oscilloscope (called from clay_renderer_raylib.c)
void render_oscilloscope_custom(Clay_BoundingBox bounds, void *osc_data) {
    if (!osc_data || !g_render_app) return;

    // osc_data points to CustomLayoutElement_Oscilloscope which has { app, channel }
    // We extract the channel (0=A, 1=B) from the data
    typedef struct { gui_app_t *app; int channel; } OscData;
    OscData *data = (OscData *)osc_data;

    const char *label = (data->channel == 0) ? "CH A" : "CH B";
    Color color = (data->channel == 0) ? COLOR_CHANNEL_A : COLOR_CHANNEL_B;

    render_oscilloscope_channel(data->app, bounds.x, bounds.y, bounds.width, bounds.height,
                                data->channel, label, color);
}

// Custom element callback for VU meter (called from clay_renderer_raylib.c)
void render_vu_meter_custom(Clay_BoundingBox bounds, void *vu_data) {
    if (!vu_data || !g_render_app) return;

    // vu_data points to CustomLayoutElement_VUMeter which has { meter, label, is_clipping }
    typedef struct {
        vu_meter_state_t *meter;
        const char *label;
        bool is_clipping_pos;
        bool is_clipping_neg;
        Color channel_color;
    } VUData;
    VUData *data = (VUData *)vu_data;

    render_vu_meter(bounds.x, bounds.y, bounds.width, bounds.height,
                    data->meter, data->label, data->is_clipping_pos, data->is_clipping_neg,
                    data->channel_color);
}
