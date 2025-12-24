/*
 * MISRC GUI - Digital Phosphor Display Implementation
 *
 * Simulates analog oscilloscope phosphor persistence with heatmap coloring.
 * Uses GPU shaders for fast intensity-to-color conversion.
 */

#include "gui_phosphor.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

//-----------------------------------------------------------------------------
// Shader Code (embedded GLSL)
//-----------------------------------------------------------------------------

// GLSL version detection based on raylib's approach
#if defined(GRAPHICS_API_OPENGL_ES2)
    #define GLSL_VERSION_STRING "#version 100\n"
    #define GLSL_PRECISION "precision mediump float;\n"
    #define GLSL_IN "attribute "
    #define GLSL_OUT "varying "
    #define GLSL_FRAG_IN "varying "
    #define GLSL_FRAG_OUT ""
    #define GLSL_FRAG_COLOR "gl_FragColor"
    #define GLSL_TEXTURE "texture2D"
#elif defined(GRAPHICS_API_OPENGL_ES3)
    #define GLSL_VERSION_STRING "#version 300 es\n"
    #define GLSL_PRECISION "precision mediump float;\n"
    #define GLSL_IN "in "
    #define GLSL_OUT "out "
    #define GLSL_FRAG_IN "in "
    #define GLSL_FRAG_OUT "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR "finalColor"
    #define GLSL_TEXTURE "texture"
#else  // Desktop OpenGL 3.3+
    #define GLSL_VERSION_STRING "#version 330\n"
    #define GLSL_PRECISION ""
    #define GLSL_IN "in "
    #define GLSL_OUT "out "
    #define GLSL_FRAG_IN "in "
    #define GLSL_FRAG_OUT "out vec4 finalColor;\n"
    #define GLSL_FRAG_COLOR "finalColor"
    #define GLSL_TEXTURE "texture"
#endif

// Vertex shader - standard passthrough
static const char *phosphor_vs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_IN "vec3 vertexPosition;\n"
    GLSL_IN "vec2 vertexTexCoord;\n"
    GLSL_OUT "vec2 fragTexCoord;\n"
    "uniform mat4 mvp;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

// Fragment shader - intensity to heatmap color conversion
static const char *phosphor_fs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_FRAG_IN "vec2 fragTexCoord;\n"
    GLSL_FRAG_OUT
    "uniform sampler2D texture0;\n"
    "\n"
    "vec4 intensityToHeatmap(float intensity) {\n"
    "    if (intensity <= 0.005) return vec4(0.0, 0.0, 0.0, 0.0);\n"
    "    if (intensity > 1.0) intensity = 1.0;\n"
    "\n"
    "    vec3 color;\n"
    "    if (intensity < 0.25) {\n"
    "        float t = intensity / 0.25;\n"
    "        color = vec3(0.0, 0.078 * t, 0.392 + 0.608 * t);\n"
    "    } else if (intensity < 0.5) {\n"
    "        float t = (intensity - 0.25) / 0.25;\n"
    "        color = vec3(0.0, 0.078 + 0.922 * t, 1.0 - 0.784 * t);\n"
    "    } else if (intensity < 0.75) {\n"
    "        float t = (intensity - 0.5) / 0.25;\n"
    "        color = vec3(t, 1.0, 0.216 - 0.216 * t);\n"
    "    } else {\n"
    "        float t = (intensity - 0.75) / 0.25;\n"
    "        color = vec3(1.0, 1.0 - 0.706 * t, 0.0);\n"
    "    }\n"
    "\n"
    "    float alpha = 0.784 + 0.216 * intensity;\n"
    "    return vec4(color, alpha);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    float intensity = " GLSL_TEXTURE "(texture0, fragTexCoord).r;\n"
    "    " GLSL_FRAG_COLOR " = intensityToHeatmap(intensity);\n"
    "}\n";

//-----------------------------------------------------------------------------
// Shader State
//-----------------------------------------------------------------------------

static Shader phosphor_shader = {0};
static bool phosphor_shader_loaded = false;
static int phosphor_shader_mvp_loc = -1;

static bool init_phosphor_shader(void) {
    if (phosphor_shader_loaded) return true;

    phosphor_shader = LoadShaderFromMemory(phosphor_vs, phosphor_fs);
    if (phosphor_shader.id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR: Failed to load shader, falling back to CPU rendering");
        return false;
    }

    phosphor_shader_mvp_loc = GetShaderLocation(phosphor_shader, "mvp");
    phosphor_shader_loaded = true;
    TraceLog(LOG_INFO, "PHOSPHOR: GPU shader loaded successfully");
    return true;
}

static void cleanup_phosphor_shader(void) {
    if (phosphor_shader_loaded) {
        UnloadShader(phosphor_shader);
        phosphor_shader_loaded = false;
        phosphor_shader.id = 0;
    }
}

//-----------------------------------------------------------------------------
// CPU Fallback: Color Lookup Table (LUT) for intensity-to-color conversion
//-----------------------------------------------------------------------------

#define PHOSPHOR_LUT_SIZE 256
static Color phosphor_color_lut[PHOSPHOR_LUT_SIZE];
static bool phosphor_lut_initialized = false;

static void init_phosphor_lut(void) {
    if (phosphor_lut_initialized) return;

    for (int i = 0; i < PHOSPHOR_LUT_SIZE; i++) {
        float intensity = (float)i / (float)(PHOSPHOR_LUT_SIZE - 1);
        phosphor_color_lut[i] = gui_phosphor_intensity_to_color(intensity);
    }
    phosphor_lut_initialized = true;
}

static inline Color phosphor_color_from_lut(float intensity) {
    if (intensity <= 0.005f) return (Color){0, 0, 0, 0};
    int idx = (int)(intensity * (PHOSPHOR_LUT_SIZE - 1));
    if (idx >= PHOSPHOR_LUT_SIZE) idx = PHOSPHOR_LUT_SIZE - 1;
    return phosphor_color_lut[idx];
}

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

    // Try to initialize GPU shader
    app->phosphor_use_shader = init_phosphor_shader();

    // Allocate intensity buffers (always needed)
    size_t float_size = (size_t)width * (size_t)height * sizeof(float);
    app->phosphor_a = (float *)calloc(1, float_size);
    app->phosphor_b = (float *)calloc(1, float_size);

    if (!app->phosphor_a || !app->phosphor_b) {
        if (app->phosphor_a) { free(app->phosphor_a); app->phosphor_a = NULL; }
        if (app->phosphor_b) { free(app->phosphor_b); app->phosphor_b = NULL; }
        app->phosphor_width = 0;
        app->phosphor_height = 0;
        return false;
    }

    app->phosphor_width = width;
    app->phosphor_height = height;

    if (app->phosphor_use_shader) {
        // GPU path: Create R32F textures (single channel float)
        // We upload intensity directly, shader converts to color
        Image img_a = { .data = app->phosphor_a, .width = width, .height = height,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R32 };
        Image img_b = { .data = app->phosphor_b, .width = width, .height = height,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R32 };

        app->phosphor_image_a = img_a;
        app->phosphor_image_b = img_b;
        app->phosphor_texture_a = LoadTextureFromImage(img_a);
        app->phosphor_texture_b = LoadTextureFromImage(img_b);

        // Set texture filtering to linear for smoother appearance
        SetTextureFilter(app->phosphor_texture_a, TEXTURE_FILTER_BILINEAR);
        SetTextureFilter(app->phosphor_texture_b, TEXTURE_FILTER_BILINEAR);
    } else {
        // CPU fallback: Need RGBA pixel buffers and LUT
        init_phosphor_lut();

        size_t pixel_size = (size_t)width * (size_t)height * sizeof(Color);
        app->phosphor_pixels_a = (Color *)calloc(1, pixel_size);
        app->phosphor_pixels_b = (Color *)calloc(1, pixel_size);

        if (!app->phosphor_pixels_a || !app->phosphor_pixels_b) {
            free(app->phosphor_a); app->phosphor_a = NULL;
            free(app->phosphor_b); app->phosphor_b = NULL;
            if (app->phosphor_pixels_a) { free(app->phosphor_pixels_a); app->phosphor_pixels_a = NULL; }
            if (app->phosphor_pixels_b) { free(app->phosphor_pixels_b); app->phosphor_pixels_b = NULL; }
            app->phosphor_width = 0;
            app->phosphor_height = 0;
            return false;
        }

        Image img_a = { .data = app->phosphor_pixels_a, .width = width, .height = height,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        Image img_b = { .data = app->phosphor_pixels_b, .width = width, .height = height,
                        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };

        app->phosphor_image_a = img_a;
        app->phosphor_image_b = img_b;
        app->phosphor_texture_a = LoadTextureFromImage(img_a);
        app->phosphor_texture_b = LoadTextureFromImage(img_b);
    }

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
    // NOTE: Decay is now integrated into gui_phosphor_render() for better performance.
    // This function is kept for API compatibility but does nothing.
    // The render function applies decay during the color conversion pass,
    // avoiding a separate iteration over the buffer.
    (void)app;
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
    app->phosphor_use_shader = false;

    // Cleanup shader (only when last app instance is done)
    cleanup_phosphor_shader();
}

//-----------------------------------------------------------------------------
// Drawing Helpers
//-----------------------------------------------------------------------------

// Helper to add intensity to a pixel - no bounds checking (caller must ensure validity)
static inline void add_phosphor_hit_unchecked(float *phosphor, int idx, float amount) {
    phosphor[idx] += amount;
    if (phosphor[idx] > 1.0f) phosphor[idx] = 1.0f;
}

// Helper to add intensity with bounds checking (for bloom pixels)
static inline void add_phosphor_hit(float *phosphor, int buf_width, int buf_height,
                                     int x, int y, float amount) {
    if (x < 0 || x >= buf_width || y < 0 || y >= buf_height) return;
    int idx = y * buf_width + x;
    add_phosphor_hit_unchecked(phosphor, idx, amount);
}

// Draw a line with Bresenham's algorithm and bloom effect
// Optimized: clips line to buffer bounds, reduces per-pixel bounds checks
static void draw_phosphor_line(float *phosphor, int buf_width, int buf_height,
                                int x0, int y0, int x1, int y1) {
    // Quick reject if entirely outside buffer
    if ((x0 < 0 && x1 < 0) || (x0 >= buf_width && x1 >= buf_width) ||
        (y0 < 0 && y1 < 0) || (y0 >= buf_height && y1 >= buf_height)) {
        return;
    }

    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    // Precompute bloom values
    const float hit_inc = PHOSPHOR_HIT_INCREMENT;
    const float bloom1 = hit_inc * 0.4f;
    const float bloom2 = hit_inc * 0.2f;

    int x = x0, y = y0;
    while (1) {
        // Only process pixels within x bounds
        if (x >= 0 && x < buf_width) {
            // Core pixel - check y bounds once
            if (y >= 0 && y < buf_height) {
                int idx = y * buf_width + x;
                add_phosphor_hit_unchecked(phosphor, idx, hit_inc);
            }

            // Bloom pixels - only check y bounds (x already validated)
            if (y - 1 >= 0 && y - 1 < buf_height) {
                add_phosphor_hit_unchecked(phosphor, (y - 1) * buf_width + x, bloom1);
            }
            if (y + 1 >= 0 && y + 1 < buf_height) {
                add_phosphor_hit_unchecked(phosphor, (y + 1) * buf_width + x, bloom1);
            }
            if (y - 2 >= 0 && y - 2 < buf_height) {
                add_phosphor_hit_unchecked(phosphor, (y - 2) * buf_width + x, bloom2);
            }
            if (y + 2 >= 0 && y + 2 < buf_height) {
                add_phosphor_hit_unchecked(phosphor, (y + 2) * buf_width + x, bloom2);
            }
        }

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

        // Skip envelope if x is out of bounds
        if (x0 < 0 || x0 >= buf_width) continue;

        // Clamp y range to buffer bounds (avoids per-pixel bounds check)
        if (max_y < 0) max_y = 0;
        if (min_y >= buf_height) min_y = buf_height - 1;
        if (max_y > min_y) continue;  // Entirely clipped

        // Fill vertical envelope - x already validated, y range clamped
        const float envelope_intensity = PHOSPHOR_HIT_INCREMENT * 0.2f;
        int base_idx = max_y * buf_width + x0;
        for (int py = max_y; py <= min_y; py++) {
            add_phosphor_hit_unchecked(phosphor, base_idx, envelope_intensity);
            base_idx += buf_width;  // Move to next row
        }
    }
}

void gui_phosphor_render(gui_app_t *app, int channel, float x, float y) {
    if (!app || !app->phosphor_textures_valid) return;

    // Check per-channel mode
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    if (trig->scope_mode != SCOPE_MODE_PHOSPHOR) return;

    float *phosphor = (channel == 0) ? app->phosphor_a : app->phosphor_b;
    Texture2D *texture = (channel == 0) ? &app->phosphor_texture_a : &app->phosphor_texture_b;

    if (!phosphor) return;

    int total_pixels = app->phosphor_width * app->phosphor_height;
    const float decay_rate = PHOSPHOR_DECAY_RATE;
    const float threshold = 0.005f;

    if (app->phosphor_use_shader) {
        // GPU shader path: Upload intensity buffer directly, shader does color conversion
        UpdateTexture(*texture, phosphor);

        // Draw with shader
        BeginShaderMode(phosphor_shader);
        DrawTexture(*texture, (int)x, (int)y, WHITE);
        EndShaderMode();

        // Apply decay for next frame (still on CPU since waveform drawing is CPU-based)
        for (int i = 0; i < total_pixels; i++) {
            float intensity = phosphor[i];
            if (intensity < threshold) {
                phosphor[i] = 0.0f;
            } else {
                phosphor[i] = intensity * decay_rate;
            }
        }
    } else {
        // CPU fallback path
        Color *pixels = (channel == 0) ? app->phosphor_pixels_a : app->phosphor_pixels_b;
        if (!pixels) return;

        // Convert intensity to colors, then apply decay for next frame
        for (int i = 0; i < total_pixels; i++) {
            float intensity = phosphor[i];

            if (intensity < threshold) {
                pixels[i] = (Color){0, 0, 0, 0};
                phosphor[i] = 0.0f;
            } else {
                pixels[i] = phosphor_color_from_lut(intensity);
                phosphor[i] = intensity * decay_rate;
            }
        }

        UpdateTexture(*texture, pixels);
        DrawTexture(*texture, (int)x, (int)y, WHITE);
    }
}

//-----------------------------------------------------------------------------
// Shared Rendering for External Modules
//-----------------------------------------------------------------------------

bool gui_phosphor_shader_available(void) {
    return init_phosphor_shader();
}

bool gui_phosphor_init_external_texture(Texture2D *texture, int width, int height) {
    if (!texture || width <= 0 || height <= 0) return false;

    // Try to initialize shader if not already done
    bool use_shader = init_phosphor_shader();

    if (use_shader) {
        // Create R32F texture for shader path
        // Allocate temporary float buffer for initial texture creation
        float *temp_data = (float *)calloc((size_t)width * height, sizeof(float));
        if (!temp_data) return false;

        Image img = {
            .data = temp_data,
            .width = width,
            .height = height,
            .mipmaps = 1,
            .format = PIXELFORMAT_UNCOMPRESSED_R32
        };

        *texture = LoadTextureFromImage(img);
        free(temp_data);

        SetTextureFilter(*texture, TEXTURE_FILTER_BILINEAR);
        return true;
    }

    return false;
}

void gui_phosphor_render_buffer(float *intensity_buffer, Texture2D *texture,
                                int width, int height,
                                float x, float y, float draw_width, float draw_height) {
    if (!intensity_buffer || !texture || width <= 0 || height <= 0) return;

    int total_pixels = width * height;
    const float decay_rate = PHOSPHOR_DECAY_RATE;
    const float threshold = 0.005f;

    if (phosphor_shader_loaded) {
        // GPU shader path: Upload intensity buffer directly
        UpdateTexture(*texture, intensity_buffer);

        // Draw with shader, scaling to destination rectangle
        BeginShaderMode(phosphor_shader);
        Rectangle src = {0, 0, (float)width, (float)height};
        Rectangle dst = {x, y, draw_width, draw_height};
        DrawTexturePro(*texture, src, dst, (Vector2){0, 0}, 0, WHITE);
        EndShaderMode();

        // Apply decay for next frame
        for (int i = 0; i < total_pixels; i++) {
            float intensity = intensity_buffer[i];
            if (intensity < threshold) {
                intensity_buffer[i] = 0.0f;
            } else {
                intensity_buffer[i] = intensity * decay_rate;
            }
        }
    } else {
        // CPU fallback - use LUT and draw directly
        // Note: This path doesn't have a pixel buffer, so we can't render
        // For external modules, shader path is required
        TraceLog(LOG_WARNING, "PHOSPHOR: gui_phosphor_render_buffer requires GPU shader");
    }
}
