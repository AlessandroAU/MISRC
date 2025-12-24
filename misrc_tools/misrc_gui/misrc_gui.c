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

    // Set render app for custom element font access
    set_render_app(&app);

    // Enumerate available devices
    gui_app_enumerate_devices(&app);

    // Enable auto-reconnect by default
    app.auto_reconnect_enabled = true;

    // Autoconnect if a device is found
    if (app.device_count > 0) {
        gui_app_set_status(&app, "Connecting...");
        if (gui_app_start_capture(&app) == 0) {
            gui_app_set_status(&app, "Connected");
        } else {
            gui_app_set_status(&app, "Failed to connect. Click Connect to retry.");
            app.reconnect_pending = true;
            app.reconnect_attempt_time = GetTime();
        }
    } else {
        gui_app_set_status(&app, "No devices found. Connect a device and restart.");
    }

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

        // Auto-reconnect logic
        if (app.auto_reconnect_enabled) {
            double now = GetTime();

            // Detect connection loss via callback timeout (no data for 2+ seconds)
            if (app.is_capturing && gui_capture_device_timeout(&app, 2000)) {
                // Device was disconnected unexpectedly - clean up properly
                fprintf(stderr, "[GUI] Device timeout detected, disconnecting...\n");
                gui_app_stop_capture(&app);
                gui_app_clear_display(&app);
                app.reconnect_pending = true;
                app.reconnect_attempt_time = now;
                app.reconnect_attempts = 0;
                gui_app_set_status(&app, "Connection lost. Reconnecting...");
            }

            // Attempt reconnection if pending
            if (app.reconnect_pending && !app.is_capturing) {
                double retry_delay = (app.reconnect_attempts < 3) ? 1.0 : 3.0;  // 1s for first 3, then 3s
                if (now - app.reconnect_attempt_time >= retry_delay) {
                    app.reconnect_attempt_time = now;
                    app.reconnect_attempts++;

                    // Re-enumerate devices in case device was reconnected
                    gui_app_enumerate_devices(&app);

                    if (app.device_count > 0) {
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "Reconnecting (attempt %d)...", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf);

                        if (gui_app_start_capture(&app) == 0) {
                            app.reconnect_pending = false;
                            app.reconnect_attempts = 0;
                            gui_app_set_status(&app, "Reconnected");
                        }
                    } else {
                        char status_buf[128];
                        snprintf(status_buf, sizeof(status_buf), "No device found (attempt %d)", app.reconnect_attempts);
                        gui_app_set_status(&app, status_buf);
                    }
                }
            }
        }

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

        // Handle oscilloscope mouse interaction (drag to set trigger level)
        handle_oscilloscope_interaction(&app);

        // Render
        BeginDrawing();
        ClearBackground(COLOR_BG);

        // Render Clay UI (custom elements are handled via CLAY_RENDER_COMMAND_TYPE_CUSTOM)
        Clay_Raylib_Render(render_commands, fonts);

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
