/*
 * MISRC GUI - Graphical Capture Interface
 *
 * Real-time waveform display and capture control using raylib + Clay UI
 *
 * Copyright (C) 2024-2025 vrunk11, stefan_o
 * Licensed under GNU GPL v3 or later
 */

// Clay UI library (header-only, implementation here)
#define CLAY_IMPLEMENTATION
#include <clay.h>

#include "raylib.h"

#include "gui_app.h"
#include "gui_ui.h"
#include "gui_render.h"
#include "gui_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Global exit flag (shared with capture thread)
volatile atomic_int do_exit = 0;

// Font array for Clay
#define FONT_COUNT 1
static Font fonts[FONT_COUNT];

// Clay error handler
void clay_error_handler(Clay_ErrorData error) {
    fprintf(stderr, "Clay Error: %s\n", error.errorText.chars);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Initialize application state
    gui_app_t app = {0};
    app.fonts = fonts;

    // Initialize default settings
    app.settings.capture_a = true;
    app.settings.capture_b = true;
    app.settings.use_flac = true;
    app.settings.flac_level = 4;
    app.settings.show_grid = true;
    app.settings.time_scale = 1.0f;
    app.settings.amplitude_scale = 1.0f;
    strcpy(app.settings.output_filename_a, "capture_a.flac");
    strcpy(app.settings.output_filename_b, "capture_b.flac");

    // Initialize raylib window
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "MISRC Capture");
    SetTargetFPS(60);
    SetExitKey(0);  // Disable escape key auto-close

    // Load fonts - try to load a clean TTF font, fall back to default
    // Try common Windows fonts that render well at small sizes
    const char *font_paths[] = {
        "C:/Windows/Fonts/arial.ttf",       // Arial - widely available
        "C:/Windows/Fonts/segoeui.ttf",    // Segoe UI - modern Windows UI font
        "C:/Windows/Fonts/calibri.ttf",     // Calibri - clean and readable
        "C:/Windows/Fonts/tahoma.ttf",      // Tahoma - good at small sizes
        NULL
    };

    fonts[0] = (Font){0};
    for (int i = 0; font_paths[i] != NULL; i++) {
        if (FileExists(font_paths[i])) {
            // Load with a good size for UI text (will be scaled as needed)
            fonts[0] = LoadFontEx(font_paths[i], 32, NULL, 256);
            if (fonts[0].texture.id != 0) {
                SetTextureFilter(fonts[0].texture, TEXTURE_FILTER_BILINEAR);
                break;
            }
        }
    }

    // Fall back to default if no TTF font found
    if (fonts[0].texture.id == 0) {
        fonts[0] = GetFontDefault();
    }

    // Initialize Clay
    uint64_t clay_memory_size = Clay_MinMemorySize();
    void *clay_memory = malloc(clay_memory_size);
    if (!clay_memory) {
        fprintf(stderr, "Failed to allocate Clay memory\n");
        CloseWindow();
        return 1;
    }

    Clay_Arena clay_arena = Clay_CreateArenaWithCapacityAndMemory(clay_memory_size, clay_memory);
    Clay_Initialize(clay_arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_error_handler });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, fonts);

    // Initialize application
    gui_app_init(&app);

    // Enumerate available devices
    gui_app_enumerate_devices(&app);
    gui_app_set_status(&app, "Ready. Select a device and click Start.");

    // Main loop
    while (!WindowShouldClose() && !atomic_load(&do_exit)) {
        float dt = GetFrameTime();

        // Handle window resize
        if (IsWindowResized()) {
            Clay_SetLayoutDimensions((Clay_Dimensions){
                (float)GetScreenWidth(), (float)GetScreenHeight()
            });
        }

        // Handle keyboard shortcuts
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (app.settings_panel_open) {
                app.settings_panel_open = false;
            } else if (app.device_dropdown_open) {
                app.device_dropdown_open = false;
            }
        }

        if (IsKeyPressed(KEY_SPACE) && !app.settings_panel_open) {
            if (app.is_capturing) {
                gui_app_stop_capture(&app);
            } else {
                gui_app_start_capture(&app);
            }
        }

        if (IsKeyPressed(KEY_R) && app.is_capturing && !app.settings_panel_open) {
            if (app.is_recording) {
                gui_app_stop_recording(&app);
            } else {
                gui_app_start_recording(&app);
            }
        }

        // Update Clay mouse state
        Vector2 mouse_pos = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mouse_pos.x, mouse_pos.y },
                             IsMouseButtonDown(MOUSE_LEFT_BUTTON));
        Clay_UpdateScrollContainers(true, (Clay_Vector2){
            GetMouseWheelMoveV().x * 20.0f,
            GetMouseWheelMoveV().y * 20.0f
        }, dt);

        // Update VU meters
        gui_app_update_vu_meters(&app, dt);

        // Update display buffer from capture data
        gui_app_update_display_buffer(&app);

        // Build UI layout
        Clay_BeginLayout();
        gui_render_layout(&app);
        Clay_RenderCommandArray render_commands = Clay_EndLayout();

        // Handle Clay interactions
        gui_handle_interactions(&app);

        // Render
        BeginDrawing();
        ClearBackground(COLOR_BG);

        // Render Clay UI
        Clay_Raylib_Render(render_commands, fonts);

        // Render custom elements (oscilloscope, VU meters) on top of Clay UI
        // Channel A: VU meter + waveform
        Clay_BoundingBox osc_a_bounds = gui_get_oscilloscope_a_bounds();
        if (osc_a_bounds.width > 0 && osc_a_bounds.height > 0) {
            render_oscilloscope_channel(&app, osc_a_bounds.x, osc_a_bounds.y,
                                        osc_a_bounds.width, osc_a_bounds.height,
                                        0, "CH A", COLOR_CHANNEL_A);
        }

        Clay_BoundingBox vu_a_bounds = gui_get_vu_meter_a_bounds();
        if (vu_a_bounds.width > 0 && vu_a_bounds.height > 0) {
            bool clip_a_pos = atomic_load(&app.clip_count_a_pos) > 0;
            bool clip_a_neg = atomic_load(&app.clip_count_a_neg) > 0;
            render_vu_meter(vu_a_bounds.x, vu_a_bounds.y, vu_a_bounds.width, vu_a_bounds.height,
                           &app.vu_a, "CH A", clip_a_pos, clip_a_neg, COLOR_CHANNEL_A);
        }

        // Channel B: VU meter + waveform
        Clay_BoundingBox osc_b_bounds = gui_get_oscilloscope_b_bounds();
        if (osc_b_bounds.width > 0 && osc_b_bounds.height > 0) {
            render_oscilloscope_channel(&app, osc_b_bounds.x, osc_b_bounds.y,
                                        osc_b_bounds.width, osc_b_bounds.height,
                                        1, "CH B", COLOR_CHANNEL_B);
        }

        Clay_BoundingBox vu_b_bounds = gui_get_vu_meter_b_bounds();
        if (vu_b_bounds.width > 0 && vu_b_bounds.height > 0) {
            bool clip_b_pos = atomic_load(&app.clip_count_b_pos) > 0;
            bool clip_b_neg = atomic_load(&app.clip_count_b_neg) > 0;
            render_vu_meter(vu_b_bounds.x, vu_b_bounds.y, vu_b_bounds.width, vu_b_bounds.height,
                           &app.vu_b, "CH B", clip_b_pos, clip_b_neg, COLOR_CHANNEL_B);
        }

        // Draw FPS in debug mode
        #ifdef DEBUG
        DrawFPS(10, 10);
        #endif

        EndDrawing();
    }

    // Cleanup
    if (app.is_recording) {
        gui_app_stop_recording(&app);
    }
    if (app.is_capturing) {
        gui_app_stop_capture(&app);
    }

    gui_app_cleanup(&app);
    free(clay_memory);

    // Unload font if we loaded a TTF (not the default font)
    if (fonts[0].texture.id != 0 && fonts[0].texture.id != GetFontDefault().texture.id) {
        UnloadFont(fonts[0]);
    }

    CloseWindow();

    return 0;
}
