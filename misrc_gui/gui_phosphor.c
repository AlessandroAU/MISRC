/*
 * MISRC GUI - Digital Phosphor Display Implementation (GPU-Accelerated)
 *
 * Simulates analog oscilloscope phosphor persistence with heatmap coloring.
 * Uses GPU render textures for accumulation/decay and shaders for effects.
 *
 * Architecture:
 *   - Two RenderTexture2D per channel (ping-pong for persistence)
 *   - Waveforms drawn directly to GPU texture via raylib primitives
 *   - Single shader pass: decay previous frame + bloom + colormap
 */

#include "gui_phosphor.h"
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

//-----------------------------------------------------------------------------
// Shader Code (embedded GLSL)
//-----------------------------------------------------------------------------

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

// Fragment shader - composites previous frame with decay + applies bloom + colormap
// Reads from previous frame texture, applies decay and bloom, outputs heatmap color
static const char *phosphor_composite_fs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_FRAG_IN "vec2 fragTexCoord;\n"
    GLSL_FRAG_OUT
    "uniform sampler2D texture0;\n"    // Previous frame (accumulated intensity)
    "uniform vec2 texelSize;\n"        // 1.0 / texture dimensions
    "uniform float decayRate;\n"       // Persistence decay (0.8 = slow fade)
    "\n"
    "vec4 intensityToHeatmap(float intensity) {\n"
    "    if (intensity < 0.02) return vec4(0.0, 0.0, 0.0, 0.0);\n"
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
    "    // Sample current pixel and neighbors for bloom\n"
    "    float center = " GLSL_TEXTURE "(texture0, fragTexCoord).r;\n"
    "    float up1    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -texelSize.y)).r;\n"
    "    float down1  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  texelSize.y)).r;\n"
    "    float up2    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -2.0*texelSize.y)).r;\n"
    "    float down2  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  2.0*texelSize.y)).r;\n"
    "    float up3    = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0, -3.0*texelSize.y)).r;\n"
    "    float down3  = " GLSL_TEXTURE "(texture0, fragTexCoord + vec2(0.0,  3.0*texelSize.y)).r;\n"
    "\n"
    "    // Combine with bloom weights (vertical blur for CRT-like glow)\n"
    "    float intensity = center + 0.5 * (up1 + down1) + 0.25 * (up2 + down2) + 0.1 * (up3 + down3);\n"
    "    intensity = min(intensity, 1.0);\n"
    "\n"
    "    " GLSL_FRAG_COLOR " = intensityToHeatmap(intensity);\n"
    "}\n";

// Simple decay shader - used for the persistence pass (writes to ping-pong buffer)
// Uses higher threshold (0.02) to ensure bloom neighbors also decay fully
static const char *phosphor_decay_fs =
    GLSL_VERSION_STRING
    GLSL_PRECISION
    GLSL_FRAG_IN "vec2 fragTexCoord;\n"
    GLSL_FRAG_OUT
    "uniform sampler2D texture0;\n"
    "uniform float decayRate;\n"
    "\n"
    "void main() {\n"
    "    float intensity = " GLSL_TEXTURE "(texture0, fragTexCoord).r;\n"
    "    intensity = intensity * decayRate;\n"
    "    if (intensity < 0.001) intensity = 0.0;\n"
    "    " GLSL_FRAG_COLOR " = vec4(intensity, 0.0, 0.0, 1.0);\n"
    "}\n";

//-----------------------------------------------------------------------------
// Shader State
//-----------------------------------------------------------------------------

static Shader phosphor_composite_shader = {0};
static Shader phosphor_decay_shader = {0};
static bool phosphor_shaders_loaded = false;

// Composite shader uniforms
static int composite_texelSize_loc = -1;
static int composite_decayRate_loc = -1;

// Decay shader uniforms
static int decay_decayRate_loc = -1;

//-----------------------------------------------------------------------------
// Performance Tracing
//-----------------------------------------------------------------------------

#define PHOSPHOR_PERF_ENABLED 1  // Set to 0 to disable tracing
#define PHOSPHOR_PERF_SAMPLES 60 // Rolling average over N frames

typedef struct {
    double update_times[PHOSPHOR_PERF_SAMPLES];
    double render_times[PHOSPHOR_PERF_SAMPLES];
    int sample_index;
    int sample_count;
    double last_log_time;
} phosphor_perf_t;

static phosphor_perf_t perf_a = {0};
static phosphor_perf_t perf_b = {0};

static void perf_record(phosphor_perf_t *perf, double update_ms, double render_ms) {
    perf->update_times[perf->sample_index] = update_ms;
    perf->render_times[perf->sample_index] = render_ms;
    perf->sample_index = (perf->sample_index + 1) % PHOSPHOR_PERF_SAMPLES;
    if (perf->sample_count < PHOSPHOR_PERF_SAMPLES) perf->sample_count++;
}

static void perf_log(phosphor_perf_t *perf, int channel, double now) {
    if (perf->sample_count == 0) return;
    // Log every 2 seconds
    if (now - perf->last_log_time < 2.0) return;
    perf->last_log_time = now;

    double update_sum = 0, render_sum = 0;
    double update_max = 0, render_max = 0;
    for (int i = 0; i < perf->sample_count; i++) {
        update_sum += perf->update_times[i];
        render_sum += perf->render_times[i];
        if (perf->update_times[i] > update_max) update_max = perf->update_times[i];
        if (perf->render_times[i] > render_max) render_max = perf->render_times[i];
    }
    double update_avg = update_sum / perf->sample_count;
    double render_avg = render_sum / perf->sample_count;

    TraceLog(LOG_DEBUG, "PHOSPHOR CH%c: update=%.2fms (max %.2f), render=%.2fms (max %.2f), total=%.2fms",
             channel == 0 ? 'A' : 'B',
             update_avg, update_max, render_avg, render_max, update_avg + render_avg);
}

static bool init_phosphor_shaders(void) {
    if (phosphor_shaders_loaded) return true;

    // Load composite shader (decay + bloom + colormap)
    phosphor_composite_shader = LoadShaderFromMemory(phosphor_vs, phosphor_composite_fs);
    if (phosphor_composite_shader.id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR: Failed to load composite shader");
        return false;
    }
    composite_texelSize_loc = GetShaderLocation(phosphor_composite_shader, "texelSize");
    composite_decayRate_loc = GetShaderLocation(phosphor_composite_shader, "decayRate");

    // Load decay shader (persistence buffer update)
    phosphor_decay_shader = LoadShaderFromMemory(phosphor_vs, phosphor_decay_fs);
    if (phosphor_decay_shader.id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR: Failed to load decay shader");
        UnloadShader(phosphor_composite_shader);
        return false;
    }
    decay_decayRate_loc = GetShaderLocation(phosphor_decay_shader, "decayRate");

    phosphor_shaders_loaded = true;
    TraceLog(LOG_INFO, "PHOSPHOR: GPU shaders loaded successfully");
    return true;
}

static void cleanup_phosphor_shaders(void) {
    if (phosphor_shaders_loaded) {
        UnloadShader(phosphor_composite_shader);
        UnloadShader(phosphor_decay_shader);
        phosphor_shaders_loaded = false;
        phosphor_composite_shader.id = 0;
        phosphor_decay_shader.id = 0;
    }
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
    if (app->phosphor_rt_valid &&
        app->phosphor_width == width && app->phosphor_height == height) {
        return true;  // Already correct size
    }

    // Free existing render textures
    if (app->phosphor_rt_valid) {
        UnloadRenderTexture(app->phosphor_rt_a[0]);
        UnloadRenderTexture(app->phosphor_rt_a[1]);
        UnloadRenderTexture(app->phosphor_rt_b[0]);
        UnloadRenderTexture(app->phosphor_rt_b[1]);
        app->phosphor_rt_valid = false;
    }

    // Initialize GPU shaders
    if (!init_phosphor_shaders()) {
        TraceLog(LOG_WARNING, "PHOSPHOR: Failed to load shaders");
        return false;
    }

    // Create render textures for ping-pong (channel A)
    app->phosphor_rt_a[0] = LoadRenderTexture(width, height);
    app->phosphor_rt_a[1] = LoadRenderTexture(width, height);

    // Create render textures for ping-pong (channel B)
    app->phosphor_rt_b[0] = LoadRenderTexture(width, height);
    app->phosphor_rt_b[1] = LoadRenderTexture(width, height);

    // Verify all render textures loaded
    if (app->phosphor_rt_a[0].id == 0 || app->phosphor_rt_a[1].id == 0 ||
        app->phosphor_rt_b[0].id == 0 || app->phosphor_rt_b[1].id == 0) {
        TraceLog(LOG_WARNING, "PHOSPHOR: Failed to create render textures");
        // Cleanup any that did load
        if (app->phosphor_rt_a[0].id) UnloadRenderTexture(app->phosphor_rt_a[0]);
        if (app->phosphor_rt_a[1].id) UnloadRenderTexture(app->phosphor_rt_a[1]);
        if (app->phosphor_rt_b[0].id) UnloadRenderTexture(app->phosphor_rt_b[0]);
        if (app->phosphor_rt_b[1].id) UnloadRenderTexture(app->phosphor_rt_b[1]);
        return false;
    }

    // Set texture filtering for smooth appearance
    SetTextureFilter(app->phosphor_rt_a[0].texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(app->phosphor_rt_a[1].texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(app->phosphor_rt_b[0].texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(app->phosphor_rt_b[1].texture, TEXTURE_FILTER_BILINEAR);

    // Clear all render textures
    BeginTextureMode(app->phosphor_rt_a[0]); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(app->phosphor_rt_a[1]); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(app->phosphor_rt_b[0]); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(app->phosphor_rt_b[1]); ClearBackground(BLACK); EndTextureMode();

    app->phosphor_width = width;
    app->phosphor_height = height;
    app->phosphor_rt_index_a = 0;
    app->phosphor_rt_index_b = 0;
    app->phosphor_rt_valid = true;

    TraceLog(LOG_INFO, "PHOSPHOR: Initialized %dx%d render textures", width, height);
    return true;
}

void gui_phosphor_clear(gui_app_t *app) {
    if (!app || !app->phosphor_rt_valid) return;

    BeginTextureMode(app->phosphor_rt_a[0]); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(app->phosphor_rt_a[1]); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(app->phosphor_rt_b[0]); ClearBackground(BLACK); EndTextureMode();
    BeginTextureMode(app->phosphor_rt_b[1]); ClearBackground(BLACK); EndTextureMode();
}

void gui_phosphor_cleanup(gui_app_t *app) {
    if (!app) return;

    if (app->phosphor_rt_valid) {
        UnloadRenderTexture(app->phosphor_rt_a[0]);
        UnloadRenderTexture(app->phosphor_rt_a[1]);
        UnloadRenderTexture(app->phosphor_rt_b[0]);
        UnloadRenderTexture(app->phosphor_rt_b[1]);
        app->phosphor_rt_valid = false;
    }

    app->phosphor_width = 0;
    app->phosphor_height = 0;

    cleanup_phosphor_shaders();
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

// Per-channel timing storage (updated in update, logged in render)
static double update_time_a = 0, update_time_b = 0;

void gui_phosphor_update(gui_app_t *app, int channel,
                         const waveform_sample_t *samples, size_t sample_count,
                         float amplitude_scale) {
    if (!app || !samples || sample_count < 2 || !app->phosphor_rt_valid) return;

    // Check per-channel mode
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    if (trig->scope_mode != SCOPE_MODE_PHOSPHOR) return;

#if PHOSPHOR_PERF_ENABLED
    double start_time = GetTime();
#endif

    // Get current render texture for this channel
    RenderTexture2D *rt_pair = (channel == 0) ? app->phosphor_rt_a : app->phosphor_rt_b;
    int *rt_index = (channel == 0) ? &app->phosphor_rt_index_a : &app->phosphor_rt_index_b;

    int current = *rt_index;
    int next = 1 - current;

    int buf_width = app->phosphor_width;
    int buf_height = app->phosphor_height;

    // Scale factor: half height = full amplitude
    float scale = amplitude_scale * 0.5f;
    float center_y = buf_height * 0.5f;

    // === Pass 1: Apply decay to previous frame, write to next buffer ===
    BeginTextureMode(rt_pair[next]);
    ClearBackground(BLACK);

    // Draw decayed previous frame
    float decayRate = PHOSPHOR_DECAY_RATE;
    SetShaderValue(phosphor_decay_shader, decay_decayRate_loc, &decayRate, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(phosphor_decay_shader);
    // Render textures are flipped in Y, so we need to flip when drawing
    DrawTextureRec(rt_pair[current].texture,
                   (Rectangle){0, 0, (float)buf_width, -(float)buf_height},
                   (Vector2){0, 0}, WHITE);
    EndShaderMode();

    // === Pass 2: Draw new waveform on top (additive) ===
    // Use additive blending for intensity accumulation
    BeginBlendMode(BLEND_ADDITIVE);

    // Waveform color - white with hit intensity as alpha
    // The shader will interpret red channel as intensity
    unsigned char hit_intensity = (unsigned char)(PHOSPHOR_HIT_INCREMENT * 255.0f);
    Color waveColor = {hit_intensity, 0, 0, 255};

    // Draw waveform as connected line segments
    for (size_t i = 0; i < sample_count - 1; i++) {
        float y0 = center_y - samples[i].value * scale * buf_height;
        float y1 = center_y - samples[i + 1].value * scale * buf_height;

        float x0 = (float)i;
        float x1 = (float)(i + 1);

        // Main waveform line
        DrawLineEx((Vector2){x0, y0}, (Vector2){x1, y1}, 1.0f, waveColor);

        // Draw envelope (min/max peaks) with lower intensity
        float min_y = center_y - samples[i].min_val * scale * buf_height;
        float max_y = center_y - samples[i].max_val * scale * buf_height;

        if (max_y > min_y) {
            float t = max_y; max_y = min_y; min_y = t;
        }

        // Only draw envelope if it differs from main value
        if (min_y - max_y > 2.0f) {
            unsigned char env_intensity = (unsigned char)(PHOSPHOR_HIT_INCREMENT * 0.2f * 255.0f);
            Color envColor = {env_intensity, 0, 0, 255};
            DrawLineEx((Vector2){x0, max_y}, (Vector2){x0, min_y}, 1.0f, envColor);
        }
    }

    EndBlendMode();
    EndTextureMode();

    // Swap buffers
    *rt_index = next;

#if PHOSPHOR_PERF_ENABLED
    double elapsed = (GetTime() - start_time) * 1000.0;  // Convert to ms
    if (channel == 0) update_time_a = elapsed;
    else update_time_b = elapsed;
#endif
}

void gui_phosphor_render(gui_app_t *app, int channel, float x, float y) {
    if (!app || !app->phosphor_rt_valid) return;

    // Check per-channel mode
    channel_trigger_t *trig = (channel == 0) ? &app->trigger_a : &app->trigger_b;
    if (trig->scope_mode != SCOPE_MODE_PHOSPHOR) return;

#if PHOSPHOR_PERF_ENABLED
    double start_time = GetTime();
#endif

    // Get current render texture
    RenderTexture2D *rt_pair = (channel == 0) ? app->phosphor_rt_a : app->phosphor_rt_b;
    int rt_index = (channel == 0) ? app->phosphor_rt_index_a : app->phosphor_rt_index_b;
    RenderTexture2D *current_rt = &rt_pair[rt_index];

    int buf_width = app->phosphor_width;
    int buf_height = app->phosphor_height;

    // Set up composite shader uniforms
    float texelSize[2] = {1.0f / buf_width, 1.0f / buf_height};
    float decayRate = PHOSPHOR_DECAY_RATE;
    SetShaderValue(phosphor_composite_shader, composite_texelSize_loc, texelSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(phosphor_composite_shader, composite_decayRate_loc, &decayRate, SHADER_UNIFORM_FLOAT);

    // Draw with composite shader (applies bloom + colormap)
    BeginShaderMode(phosphor_composite_shader);
    // Flip Y when drawing render texture
    DrawTextureRec(current_rt->texture,
                   (Rectangle){0, 0, (float)buf_width, -(float)buf_height},
                   (Vector2){x, y}, WHITE);
    EndShaderMode();

#if PHOSPHOR_PERF_ENABLED
    double render_ms = (GetTime() - start_time) * 1000.0;
    double update_ms = (channel == 0) ? update_time_a : update_time_b;
    phosphor_perf_t *perf = (channel == 0) ? &perf_a : &perf_b;
    perf_record(perf, update_ms, render_ms);
    perf_log(perf, channel, GetTime());
#endif
}
