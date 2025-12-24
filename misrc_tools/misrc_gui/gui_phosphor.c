/*
 * MISRC GUI - Digital Phosphor Display Implementation
 *
 * Simulates analog oscilloscope phosphor persistence with heatmap coloring.
 */

#include "gui_phosphor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

//-----------------------------------------------------------------------------
// Color Conversion
//-----------------------------------------------------------------------------

// Convert intensity (0-1) to heatmap color (blue -> green -> yellow -> red)
Color gui_phosphor_intensity_to_color(float intensity) {
    if (intensity <= 0.005f) return (Color){0, 0, 0, 0};  // Fully transparent for very dim
    if (intensity > 1.0f) intensity = 1.0f;

    unsigned char r, g, b, a;

    if (intensity < 0.25f) {
        // Blue (cold)
        float t = intensity / 0.25f;
        r = 0;
        g = (unsigned char)(20 * t);
        b = (unsigned char)(100 + 155 * t);
    } else if (intensity < 0.5f) {
        // Blue to green
        float t = (intensity - 0.25f) / 0.25f;
        r = 0;
        g = (unsigned char)(20 + 235 * t);
        b = (unsigned char)(255 - 200 * t);
    } else if (intensity < 0.75f) {
        // Green to yellow
        float t = (intensity - 0.5f) / 0.25f;
        r = (unsigned char)(255 * t);
        g = 255;
        b = (unsigned char)(55 - 55 * t);
    } else {
        // Yellow to red (hot)
        float t = (intensity - 0.75f) / 0.25f;
        r = 255;
        g = (unsigned char)(255 - 180 * t);
        b = 0;
    }

    // Full opacity once visible
    a = (unsigned char)(200 + 55 * intensity);
    return (Color){r, g, b, a};
}

//-----------------------------------------------------------------------------
// Buffer Management
//-----------------------------------------------------------------------------

bool gui_phosphor_init(gui_app_t *app, int width, int height) {
    if (!app) return false;

    // Clamp to maximum dimensions
    if (width > PHOSPHOR_MAX_WIDTH) width = PHOSPHOR_MAX_WIDTH;
    if (height > PHOSPHOR_MAX_HEIGHT) height = PHOSPHOR_MAX_HEIGHT;
    if (width < 1) width = 1;
    if (height < 1) height = 1;

    // Check if resize needed
    if (app->phosphor_a && app->phosphor_b &&
        app->phosphor_width == width && app->phosphor_height == height) {
        return true;  // Already correct size
    }

    // Free existing buffers and textures
    if (app->phosphor_textures_valid) {
        UnloadTexture(app->phosphor_texture_a);
        UnloadTexture(app->phosphor_texture_b);
        app->phosphor_textures_valid = false;
    }
    if (app->phosphor_a) { free(app->phosphor_a); app->phosphor_a = NULL; }
    if (app->phosphor_b) { free(app->phosphor_b); app->phosphor_b = NULL; }
    if (app->phosphor_pixels_a) { free(app->phosphor_pixels_a); app->phosphor_pixels_a = NULL; }
    if (app->phosphor_pixels_b) { free(app->phosphor_pixels_b); app->phosphor_pixels_b = NULL; }

    // Allocate new buffers
    size_t float_size = (size_t)width * (size_t)height * sizeof(float);
    size_t pixel_size = (size_t)width * (size_t)height * sizeof(Color);

    app->phosphor_a = (float *)calloc(1, float_size);
    app->phosphor_b = (float *)calloc(1, float_size);
    app->phosphor_pixels_a = (Color *)calloc(1, pixel_size);
    app->phosphor_pixels_b = (Color *)calloc(1, pixel_size);

    if (!app->phosphor_a || !app->phosphor_b ||
        !app->phosphor_pixels_a || !app->phosphor_pixels_b) {
        // Allocation failed - clean up
        if (app->phosphor_a) { free(app->phosphor_a); app->phosphor_a = NULL; }
        if (app->phosphor_b) { free(app->phosphor_b); app->phosphor_b = NULL; }
        if (app->phosphor_pixels_a) { free(app->phosphor_pixels_a); app->phosphor_pixels_a = NULL; }
        if (app->phosphor_pixels_b) { free(app->phosphor_pixels_b); app->phosphor_pixels_b = NULL; }
        app->phosphor_width = 0;
        app->phosphor_height = 0;
        return false;
    }

    app->phosphor_width = width;
    app->phosphor_height = height;

    // Create textures
    Image img_a = { .data = app->phosphor_pixels_a, .width = width, .height = height,
                    .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Image img_b = { .data = app->phosphor_pixels_b, .width = width, .height = height,
                    .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

    app->phosphor_image_a = img_a;
    app->phosphor_image_b = img_b;
    app->phosphor_texture_a = LoadTextureFromImage(img_a);
    app->phosphor_texture_b = LoadTextureFromImage(img_b);
    app->phosphor_textures_valid = true;

    return true;
}

void gui_phosphor_clear(gui_app_t *app) {
    if (!app) return;

    size_t buffer_size = (size_t)app->phosphor_width * (size_t)app->phosphor_height * sizeof(float);
    if (app->phosphor_a) memset(app->phosphor_a, 0, buffer_size);
    if (app->phosphor_b) memset(app->phosphor_b, 0, buffer_size);
}

void gui_phosphor_decay(gui_app_t *app) {
    if (!app) return;

    size_t total_pixels = (size_t)app->phosphor_width * (size_t)app->phosphor_height;
    if (total_pixels == 0) return;

    // Decay channel A if in phosphor mode
    if (app->trigger_a.scope_mode == SCOPE_MODE_PHOSPHOR && app->phosphor_a) {
        for (size_t i = 0; i < total_pixels; i++) {
            app->phosphor_a[i] *= PHOSPHOR_DECAY_RATE;
        }
    }
    // Decay channel B if in phosphor mode
    if (app->trigger_b.scope_mode == SCOPE_MODE_PHOSPHOR && app->phosphor_b) {
        for (size_t i = 0; i < total_pixels; i++) {
            app->phosphor_b[i] *= PHOSPHOR_DECAY_RATE;
        }
    }
}

void gui_phosphor_cleanup(gui_app_t *app) {
    if (!app) return;
    if (app->phosphor_textures_valid) {
        UnloadTexture(app->phosphor_texture_a);
        UnloadTexture(app->phosphor_texture_b);
        app->phosphor_textures_valid = false;
    }
    if (app->phosphor_a) { free(app->phosphor_a); app->phosphor_a = NULL; }
    if (app->phosphor_b) { free(app->phosphor_b); app->phosphor_b = NULL; }
    if (app->phosphor_pixels_a) { free(app->phosphor_pixels_a); app->phosphor_pixels_a = NULL; }
    if (app->phosphor_pixels_b) { free(app->phosphor_pixels_b); app->phosphor_pixels_b = NULL; }
    app->phosphor_width = 0;
    app->phosphor_height = 0;
}

//-----------------------------------------------------------------------------
// Drawing Helpers
//-----------------------------------------------------------------------------

// Helper to add intensity to a pixel with bounds checking
static inline void add_phosphor_hit(float *phosphor, int buf_width, int buf_height,
                                     int x, int y, float amount) {
    if (x < 0 || x >= buf_width || y < 0 || y >= buf_height) return;
    int idx = y * buf_width + x;
    phosphor[idx] += amount;
    if (phosphor[idx] > 1.0f) phosphor[idx] = 1.0f;
}

// Draw a line with Bresenham's algorithm and bloom effect
static void draw_phosphor_line(float *phosphor, int buf_width, int buf_height,
                                int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0, y = y0;
    while (1) {
        // Core pixel - full intensity
        add_phosphor_hit(phosphor, buf_width, buf_height, x, y, PHOSPHOR_HIT_INCREMENT);

        // Bloom: add lower intensity to neighboring pixels (creates glow)
        float bloom = PHOSPHOR_HIT_INCREMENT * 0.4f;
        add_phosphor_hit(phosphor, buf_width, buf_height, x, y - 1, bloom);
        add_phosphor_hit(phosphor, buf_width, buf_height, x, y + 1, bloom);
        add_phosphor_hit(phosphor, buf_width, buf_height, x, y - 2, bloom * 0.5f);
        add_phosphor_hit(phosphor, buf_width, buf_height, x, y + 2, bloom * 0.5f);

        if (x == x1 && y == y1) break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

void gui_phosphor_update(gui_app_t *app, int channel,
                         const waveform_sample_t *samples, size_t sample_count,
                         float amplitude_scale) {
    if (!app || !samples || sample_count < 2) return;

    // Check per-channel mode
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    if (trig->scope_mode != SCOPE_MODE_PHOSPHOR) return;

    float *phosphor = (channel == 0) ? app->phosphor_a : app->phosphor_b;
    if (!phosphor) return;

    int buf_width = app->phosphor_width;
    int buf_height = app->phosphor_height;
    if (buf_width <= 0 || buf_height <= 0) return;

    // Scale factor: half height = full amplitude
    float scale = amplitude_scale * 0.5f;
    float center_y_norm = 0.5f;

    for (size_t i = 0; i < sample_count - 1; i++) {
        // Get Y positions for this sample and next (normalized 0-1)
        float y0_norm = center_y_norm - samples[i].value * scale;
        float y1_norm = center_y_norm - samples[i + 1].value * scale;

        // Convert to pixel coordinates
        int x0 = (int)i;
        int x1 = (int)(i + 1);
        int y0 = (int)(y0_norm * (float)buf_height);
        int y1 = (int)(y1_norm * (float)buf_height);

        // Draw line with bloom
        draw_phosphor_line(phosphor, buf_width, buf_height, x0, y0, x1, y1);

        // Also add hits from min/max envelope for peak visibility
        int min_y = (int)((center_y_norm - samples[i].min_val * scale) * (float)buf_height);
        int max_y = (int)((center_y_norm - samples[i].max_val * scale) * (float)buf_height);

        // Ensure min_y > max_y (max_y is higher on screen = lower y value)
        if (max_y > min_y) { int t = max_y; max_y = min_y; min_y = t; }

        // Fill vertical envelope with gradient intensity
        for (int py = max_y; py <= min_y; py++) {
            float envelope_intensity = PHOSPHOR_HIT_INCREMENT * 0.2f;
            add_phosphor_hit(phosphor, buf_width, buf_height, x0, py, envelope_intensity);
        }
    }
}

void gui_phosphor_render(gui_app_t *app, int channel, float x, float y) {
    if (!app || !app->phosphor_textures_valid) return;

    // Check per-channel mode
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    if (trig->scope_mode != SCOPE_MODE_PHOSPHOR) return;

    float *phosphor = (channel == 0) ? app->phosphor_a : app->phosphor_b;
    Color *pixels = (channel == 0) ? app->phosphor_pixels_a : app->phosphor_pixels_b;
    Texture2D *texture = (channel == 0) ? &app->phosphor_texture_a : &app->phosphor_texture_b;

    if (!phosphor || !pixels) return;

    // Convert intensity buffer to RGBA pixels (heatmap colors)
    int total_pixels = app->phosphor_width * app->phosphor_height;
    for (int i = 0; i < total_pixels; i++) {
        pixels[i] = gui_phosphor_intensity_to_color(phosphor[i]);
    }

    // Update GPU texture with new pixel data
    UpdateTexture(*texture, pixels);

    // Draw texture at oscilloscope position
    DrawTexture(*texture, (int)x, (int)y, WHITE);
}
