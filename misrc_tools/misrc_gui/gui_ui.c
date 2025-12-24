/*
 * MISRC GUI - UI Layout Implementation
 *
 * Clay-based declarative UI layout (Clay v0.14 API)
 */

#include "gui_ui.h"
#include "gui_render.h"
#include <clay.h>
#include <stdio.h>
#include <string.h>

// Color conversions
static inline Clay_Color to_clay_color(Color c) {
    return (Clay_Color){ c.r, c.g, c.b, c.a };
}

// Format helpers - use separate buffers to avoid overwriting
static char temp_buf1[64];
static char temp_buf2[64];
static char temp_buf3[64];
static char temp_buf4[64];
static char temp_buf5[64];
static char device_dropdown_buf[64];

static Clay_String make_string(const char *str) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(str), .chars = str };
}

// Get bounding boxes from Clay after layout (call after Clay_EndLayout)
Clay_BoundingBox gui_get_oscilloscope_a_bounds(void) {
    Clay_ElementData data = Clay_GetElementData(CLAY_ID("OscilloscopeCanvasA"));
    return data.found ? data.boundingBox : (Clay_BoundingBox){0};
}

Clay_BoundingBox gui_get_oscilloscope_b_bounds(void) {
    Clay_ElementData data = Clay_GetElementData(CLAY_ID("OscilloscopeCanvasB"));
    return data.found ? data.boundingBox : (Clay_BoundingBox){0};
}

Clay_BoundingBox gui_get_vu_meter_a_bounds(void) {
    Clay_ElementData data = Clay_GetElementData(CLAY_ID("VUMeterA"));
    return data.found ? data.boundingBox : (Clay_BoundingBox){0};
}

Clay_BoundingBox gui_get_vu_meter_b_bounds(void) {
    Clay_ElementData data = Clay_GetElementData(CLAY_ID("VUMeterB"));
    return data.found ? data.boundingBox : (Clay_BoundingBox){0};
}

// Render the toolbar
static void render_toolbar(gui_app_t *app) {
    CLAY(CLAY_ID("Toolbar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(48) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .padding = { 8, 8, 8, 8 },
            .childGap = 12
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        // Title
        CLAY_TEXT(CLAY_STRING("MISRC Capture"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer1"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_GROW(0) } }
        }) {}

        // Device label
        CLAY_TEXT(CLAY_STRING("Device:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        // Device dropdown button
        Color dropdown_color = app->device_dropdown_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        CLAY(CLAY_ID("DeviceDropdown"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(250), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 10, 10, 0, 0 }
            },
            .backgroundColor = to_clay_color(dropdown_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            const char *device_name = app->device_count > 0 ?
                app->devices[app->selected_device].name : "No devices";
            snprintf(device_dropdown_buf, sizeof(device_dropdown_buf), "%s", device_name);
            CLAY_TEXT(make_string(device_dropdown_buf),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer2"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

        // Start/Stop button
        Color start_color = app->is_capturing ? COLOR_CLIP_RED : COLOR_SYNC_GREEN;
        CLAY(CLAY_ID("StartButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(start_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->is_capturing ? CLAY_STRING("Stop") : CLAY_STRING("Start"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = { 255, 255, 255, 255 } }));
        }

        // Record button
        Color record_color = app->is_recording ? COLOR_CLIP_RED : COLOR_BUTTON;
        if (!app->is_capturing) record_color = (Color){ 50, 50, 55, 255 };
        CLAY(CLAY_ID("RecordButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(record_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            Color text_color = app->is_capturing ? COLOR_TEXT : COLOR_TEXT_DIM;
            CLAY_TEXT(app->is_recording ? CLAY_STRING("Stop Rec") : CLAY_STRING("Record"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(text_color) }));
        }

        // Settings button
        CLAY(CLAY_ID("SettingsButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(32), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(app->settings_panel_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(CLAY_STRING("*"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));
        }
    }
}

// Render the channels panel - each channel has VU meter + waveform grouped together
static void render_channels_panel(gui_app_t *app) {
    (void)app;
    CLAY(CLAY_ID("ChannelsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 4, 4, 4, 4 },
            .childGap = 8
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG)
    }) {
        // Channel A row: VU meter + waveform
        CLAY(CLAY_ID("ChannelARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter A
            CLAY(CLAY_ID("VUMeterA"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .backgroundColor = to_clay_color(COLOR_METER_BG)
            }) {}

            // Oscilloscope canvas A
            CLAY(CLAY_ID("OscilloscopeCanvasA"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .backgroundColor = to_clay_color((Color){20, 20, 25, 255})
            }) {}
        }

        // Channel B row: VU meter + waveform
        CLAY(CLAY_ID("ChannelBRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter B
            CLAY(CLAY_ID("VUMeterB"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .backgroundColor = to_clay_color(COLOR_METER_BG)
            }) {}

            // Oscilloscope canvas B
            CLAY(CLAY_ID("OscilloscopeCanvasB"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .backgroundColor = to_clay_color((Color){20, 20, 25, 255})
            }) {}
        }
    }
}

// Render statistics panel
static void render_stats_panel(gui_app_t *app) {
    CLAY(CLAY_ID("StatsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(180), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 12, 12, 12, 12 },
            .childGap = 8
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG)
    }) {
        // Title
        CLAY_TEXT(CLAY_STRING("Statistics"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING, .textColor = to_clay_color(COLOR_TEXT) }));

        // Sync status
        bool synced = atomic_load(&app->stream_synced);
        Color sync_color = synced ? COLOR_SYNC_GREEN : COLOR_SYNC_RED;
        CLAY(CLAY_ID("SyncRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Sync:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(synced ? CLAY_STRING("OK") : CLAY_STRING("--"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(sync_color) }));
        }

        // Frame count
        uint32_t frames = atomic_load(&app->frame_count);
        snprintf(temp_buf1, sizeof(temp_buf1), "%u", frames);
        CLAY(CLAY_ID("FramesRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Frames:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(temp_buf1),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Sample count
        uint64_t samples = atomic_load(&app->total_samples);
        snprintf(temp_buf2, sizeof(temp_buf2), "%llu", (unsigned long long)samples);
        CLAY(CLAY_ID("SamplesRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Samples:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(temp_buf2),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Error count
        uint32_t errors = atomic_load(&app->error_count);
        Color error_color = errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT;
        snprintf(temp_buf3, sizeof(temp_buf3), "%u", errors);
        CLAY(CLAY_ID("ErrorsRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Errors:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(temp_buf3),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(error_color) }));
        }

        // Clip counts (combine pos + neg for display)
        uint32_t clip_a = atomic_load(&app->clip_count_a_pos) + atomic_load(&app->clip_count_a_neg);
        uint32_t clip_b = atomic_load(&app->clip_count_b_pos) + atomic_load(&app->clip_count_b_neg);

        snprintf(temp_buf4, sizeof(temp_buf4), "%u", clip_a);
        CLAY(CLAY_ID("ClipARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Clip A:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_CHANNEL_A) }));
            CLAY_TEXT(make_string(temp_buf4),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(clip_a > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        snprintf(temp_buf5, sizeof(temp_buf5), "%u", clip_b);
        CLAY(CLAY_ID("ClipBRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 8
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Clip B:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_CHANNEL_B) }));
            CLAY_TEXT(make_string(temp_buf5),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(clip_b > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }
    }
}

// Render status bar
static void render_status_bar(gui_app_t *app) {
    CLAY(CLAY_ID("StatusBar"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .padding = { 12, 12, 0, 0 },
            .childGap = 20
        },
        .backgroundColor = to_clay_color(COLOR_TOOLBAR_BG)
    }) {
        if (app->is_recording) {
            // Recording indicator
            CLAY(CLAY_ID("RecIndicator"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(12) } },
                .backgroundColor = to_clay_color(COLOR_CLIP_RED),
                .cornerRadius = CLAY_CORNER_RADIUS(6)
            }) {}

            // Recording duration
            double duration = GetTime() - app->recording_start_time;
            int hours = (int)(duration / 3600);
            int mins = (int)(duration / 60) % 60;
            int secs = (int)duration % 60;
            snprintf(temp_buf1, sizeof(temp_buf1), "%02d:%02d:%02d", hours, mins, secs);
            CLAY_TEXT(make_string(temp_buf1),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT) }));

            // File size
            uint64_t bytes = atomic_load(&app->recording_bytes);
            double mb = (double)bytes / (1024.0 * 1024.0);
            snprintf(temp_buf2, sizeof(temp_buf2), "%.1f MB", mb);
            CLAY_TEXT(make_string(temp_buf2),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        } else {
            // Status message
            snprintf(temp_buf1, sizeof(temp_buf1), "%s", app->status_message);
            CLAY_TEXT(make_string(temp_buf1),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        // Spacer
        CLAY(CLAY_ID("StatusSpacer"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

        // Sample rate
        uint32_t srate = atomic_load(&app->sample_rate);
        if (srate > 0) {
            snprintf(temp_buf3, sizeof(temp_buf3), "%u MSPS", srate / 1000000);
            CLAY_TEXT(make_string(temp_buf3),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }
    }
}

// Main layout function
void gui_render_layout(gui_app_t *app) {
    // Root container
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM
        }
    }) {
        // Toolbar
        render_toolbar(app);

        // Main content area
        CLAY(CLAY_ID("MainArea"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 2
            }
        }) {
            // Channels panel (VU meters + waveforms grouped by channel)
            render_channels_panel(app);

            // Statistics panel
            render_stats_panel(app);
        }

        // Status bar
        render_status_bar(app);
    }

    // Device dropdown overlay (if open)
    if (app->device_dropdown_open && app->device_count > 0) {
        CLAY(CLAY_ID("DeviceDropdownOverlay"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(250), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_TOP_TO_BOTTOM
            },
            .floating = {
                .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                .parentId = CLAY_ID("DeviceDropdown").id,
                .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
            },
            .backgroundColor = to_clay_color(COLOR_PANEL_BG),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            for (int i = 0; i < app->device_count; i++) {
                Color item_color = (i == app->selected_device) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;

                CLAY(CLAY_IDI("DeviceOption", i), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                        .padding = { 10, 10, 0, 0 }
                    },
                    .backgroundColor = to_clay_color(item_color)
                }) {
                    // Use a static buffer for device name - each iteration overwrites
                    static char dev_name_buf[64];
                    snprintf(dev_name_buf, sizeof(dev_name_buf), "%s", app->devices[i].name);
                    CLAY_TEXT(make_string(dev_name_buf),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_DROPDOWN, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }
    }
}

// Handle UI interactions
void gui_handle_interactions(gui_app_t *app) {
    // Handle clicks
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        // Check start button
        if (Clay_PointerOver(CLAY_ID("StartButton"))) {
            if (app->is_capturing) {
                gui_app_stop_capture(app);
            } else {
                gui_app_start_capture(app);
            }
        }

        // Check record button
        if (Clay_PointerOver(CLAY_ID("RecordButton")) && app->is_capturing) {
            if (app->is_recording) {
                gui_app_stop_recording(app);
            } else {
                gui_app_start_recording(app);
            }
        }

        // Check settings button
        if (Clay_PointerOver(CLAY_ID("SettingsButton"))) {
            app->settings_panel_open = !app->settings_panel_open;
            // Settings panel not yet implemented - show status message
            if (app->settings_panel_open) {
                gui_app_set_status(app, "Settings panel coming soon. Press * again to close.");
            } else {
                gui_app_set_status(app, "Ready.");
            }
        }

        // Check device dropdown
        if (Clay_PointerOver(CLAY_ID("DeviceDropdown"))) {
            app->device_dropdown_open = !app->device_dropdown_open;
        } else if (app->device_dropdown_open) {
            // Check device options
            bool clicked_option = false;
            for (int i = 0; i < app->device_count; i++) {
                if (Clay_PointerOver(CLAY_IDI("DeviceOption", i))) {
                    app->selected_device = i;
                    app->device_dropdown_open = false;
                    clicked_option = true;
                    break;
                }
            }
            if (!clicked_option) {
                app->device_dropdown_open = false;
            }
        }
    }
}
