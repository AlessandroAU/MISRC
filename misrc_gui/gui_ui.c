/*
 * MISRC GUI - UI Layout Implementation
 *
 * Clay-based declarative UI layout (Clay v0.14 API)
 */

#include "gui_ui.h"
#include "gui_render.h"
#include "gui_dropdown.h"
#include <clay.h>
#include <stdio.h>
#include <string.h>

// Dropdown identifiers
#define DROPDOWN_DEVICE       "Device"
#define DROPDOWN_SCOPE_MODE   "ScopeMode"
#define DROPDOWN_TRIGGER_MODE "TriggerMode"

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
static char temp_buf6[64];
static char device_dropdown_buf[64];

// Per-channel stat buffers (separate for A and B to avoid overwrite)
static char stat_a_samples[32];
static char stat_a_peak_pos[16];
static char stat_a_peak_neg[16];
static char stat_a_clip[16];
static char stat_a_errors[16];
static char stat_b_samples[32];
static char stat_b_peak_pos[16];
static char stat_b_peak_neg[16];
static char stat_b_clip[16];
static char stat_b_errors[16];

static Clay_String make_string(const char *str) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = (int32_t)strlen(str), .chars = str };
}

// Custom element type enum (must match clay_renderer_raylib.c)
typedef enum {
    CUSTOM_LAYOUT_ELEMENT_TYPE_OSCILLOSCOPE,
    CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER
} CustomLayoutElementType;

// Custom element data structures (must match clay_renderer_raylib.c)
typedef struct {
    gui_app_t *app;
    int channel;
} CustomLayoutElement_Oscilloscope;

typedef struct {
    vu_meter_state_t *meter;
    const char *label;
    bool is_clipping_pos;
    bool is_clipping_neg;
    Color channel_color;
} CustomLayoutElement_VUMeter;

typedef struct {
    CustomLayoutElementType type;
    union {
        CustomLayoutElement_Oscilloscope oscilloscope;
        CustomLayoutElement_VUMeter vu_meter;
    } customData;
} CustomLayoutElement;

// Static storage for custom element data (persists during render)
static CustomLayoutElement s_osc_a_element;
static CustomLayoutElement s_osc_b_element;
static CustomLayoutElement s_vu_a_element;
static CustomLayoutElement s_vu_b_element;

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
        CLAY_TEXT(CLAY_STRING("MISRC Capture (Beta)"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer1"), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(20), CLAY_SIZING_GROW(0) } }
        }) {}

        // Device label
        CLAY_TEXT(CLAY_STRING("Device:"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        // Device dropdown button
        bool device_dropdown_open = gui_dropdown_is_open(DROPDOWN_DEVICE, 0);
        Color dropdown_color = device_dropdown_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
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

        // Connect/Disconnect button (next to device dropdown)
        Color connect_color = app->is_capturing ? COLOR_CLIP_RED : COLOR_SYNC_GREEN;
        CLAY(CLAY_ID("ConnectButton"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(32) },
                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
            },
            .backgroundColor = to_clay_color(connect_color),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            CLAY_TEXT(app->is_capturing ? CLAY_STRING("Disconnect") : CLAY_STRING("Connect"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = { 255, 255, 255, 255 } }));
        }

        // Spacer
        CLAY(CLAY_ID("ToolbarSpacer2"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
        }) {}

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

// Helper macro for stat row layout
#define STAT_ROW_LAYOUT { \
    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, \
    .layoutDirection = CLAY_LEFT_TO_RIGHT, \
    .childGap = 4 \
}

// Trigger level buffers
static char trig_level_a_buf[16];
static char trig_level_b_buf[16];

// Render per-channel stats panel with trigger controls
static void render_channel_stats(gui_app_t *app, int channel) {
    // Get per-channel stats and trigger
    uint64_t samples;
    uint32_t clip_pos, clip_neg, errors;
    float peak_pos, peak_neg;
    Color channel_color;
    char *buf_samples, *buf_peak_pos, *buf_peak_neg, *buf_clip, *buf_errors;
    channel_trigger_t *trig;
    char *trig_level_buf;

    if (channel == 0) {
        samples = atomic_load(&app->samples_a);
        clip_pos = atomic_load(&app->clip_count_a_pos);
        clip_neg = atomic_load(&app->clip_count_a_neg);
        errors = atomic_load(&app->error_count_a);
        peak_pos = app->vu_a.peak_pos;
        peak_neg = app->vu_a.peak_neg;
        channel_color = COLOR_CHANNEL_A;
        buf_samples = stat_a_samples;
        buf_peak_pos = stat_a_peak_pos;
        buf_peak_neg = stat_a_peak_neg;
        buf_clip = stat_a_clip;
        buf_errors = stat_a_errors;
        trig = &app->trigger_a;
        trig_level_buf = trig_level_a_buf;
    } else {
        samples = atomic_load(&app->samples_b);
        clip_pos = atomic_load(&app->clip_count_b_pos);
        clip_neg = atomic_load(&app->clip_count_b_neg);
        errors = atomic_load(&app->error_count_b);
        peak_pos = app->vu_b.peak_pos;
        peak_neg = app->vu_b.peak_neg;
        channel_color = COLOR_CHANNEL_B;
        buf_samples = stat_b_samples;
        buf_peak_pos = stat_b_peak_pos;
        buf_peak_neg = stat_b_peak_neg;
        buf_clip = stat_b_clip;
        buf_errors = stat_b_errors;
        trig = &app->trigger_b;
        trig_level_buf = trig_level_b_buf;
    }

    uint32_t clip_total = clip_pos + clip_neg;

    // Format stats
    if (samples >= 1000000000ULL) {
        snprintf(buf_samples, 32, "%.2fG", (double)samples / 1000000000.0);
    } else if (samples >= 1000000ULL) {
        snprintf(buf_samples, 32, "%.2fM", (double)samples / 1000000.0);
    } else if (samples >= 1000ULL) {
        snprintf(buf_samples, 32, "%.1fK", (double)samples / 1000.0);
    } else {
        snprintf(buf_samples, 32, "%llu", (unsigned long long)samples);
    }

    snprintf(buf_peak_pos, 16, "+%.0f%%", peak_pos * 100.0f);
    snprintf(buf_peak_neg, 16, "-%.0f%%", peak_neg * 100.0f);
    snprintf(buf_clip, 16, "%u", clip_total);
    snprintf(buf_errors, 16, "%u", errors);

    // Format trigger level
    int level_percent = (int)(trig->level * 100 / 2048);
    snprintf(trig_level_buf, 16, "%+d%%", level_percent);

    CLAY(CLAY_IDI("StatsPanel", channel), {
        .layout = {
            .sizing = { CLAY_SIZING_FIXED(150), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 6, 6, 4, 4 },
            .childGap = 2
        },
        .backgroundColor = to_clay_color((Color){ 35, 35, 42, 255 })
    }) {
        // Channel label
        CLAY_TEXT(channel == 0 ? CLAY_STRING("Channel A") : CLAY_STRING("Channel B"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS_LABEL, .textColor = to_clay_color(channel_color) }));

        // Samples row
        CLAY(CLAY_IDI("StatSamples", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY_TEXT(CLAY_STRING("Samples:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(buf_samples),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Peak row (shows both + and -)
        CLAY(CLAY_IDI("StatPeak", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY_TEXT(CLAY_STRING("Peak:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(buf_peak_pos),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_pos > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT) }));
            CLAY_TEXT(make_string(buf_peak_neg),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(peak_neg > 0.95f ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Clip row
        CLAY(CLAY_IDI("StatClip", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY_TEXT(CLAY_STRING("Clip:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(buf_clip),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(clip_total > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Errors row
        CLAY(CLAY_IDI("StatErrors", channel), { .layout = STAT_ROW_LAYOUT }) {
            CLAY_TEXT(CLAY_STRING("Errors:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(buf_errors),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(errors > 0 ? COLOR_CLIP_RED : COLOR_TEXT) }));
        }

        // Separator line
        CLAY(CLAY_IDI("StatSep", channel), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
            .backgroundColor = to_clay_color(COLOR_TEXT_DIM)
        }) {}

        // Trigger row with combined mode/off dropdown
        CLAY(CLAY_IDI("TrigRow", channel), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 4
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Trigger:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

            // Trigger mode dropdown button (includes Off option)
            const char *trig_mode_name;
            if (!trig->enabled) {
                trig_mode_name = "Off";
            } else {
                switch (trig->trigger_mode) {
                    case TRIGGER_MODE_RISING:     trig_mode_name = "Rising"; break;
                    case TRIGGER_MODE_FALLING:    trig_mode_name = "Falling"; break;
                    case TRIGGER_MODE_CVBS_HSYNC: trig_mode_name = "CVBS"; break;
                    default:                      trig_mode_name = "Rising"; break;
                }
            }
            bool trig_dropdown_open = gui_dropdown_is_open(DROPDOWN_TRIGGER_MODE, channel);
            Color btn_color = trig->enabled ? channel_color : COLOR_BUTTON;
            if (trig_dropdown_open) btn_color = COLOR_BUTTON_HOVER;
            CLAY(CLAY_IDI("TrigModeBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIXED(18) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(btn_color),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(make_string(trig_mode_name),
                    CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Trigger mode dropdown options (floating, visible when open)
        if (gui_dropdown_is_open(DROPDOWN_TRIGGER_MODE, channel)) {
            CLAY(CLAY_IDI("TrigModeOpts", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                    .parentId = CLAY_IDI("TrigModeBtn", channel).id,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                },
                .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                // Off option
                Color off_color = !trig->enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                CLAY(CLAY_IDI("TrigModeOptOff", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(off_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Off"),
                        CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
                }

                // Rising edge option
                Color rising_color = (trig->enabled && trig->trigger_mode == TRIGGER_MODE_RISING) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                CLAY(CLAY_IDI("TrigModeOptRising", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(rising_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Rising"),
                        CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
                }

                // Falling edge option
                Color falling_color = (trig->enabled && trig->trigger_mode == TRIGGER_MODE_FALLING) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                CLAY(CLAY_IDI("TrigModeOptFalling", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(falling_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Falling"),
                        CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
                }

                // CVBS H-Sync option
                Color cvbs_color = (trig->enabled && trig->trigger_mode == TRIGGER_MODE_CVBS_HSYNC) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                CLAY(CLAY_IDI("TrigModeOptCVBS", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(cvbs_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("CVBS"),
                        CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }

        // Separator line before display mode
        CLAY(CLAY_IDI("ModeSep", channel), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
            .backgroundColor = to_clay_color(COLOR_TEXT_DIM)
        }) {}

        // Display mode row
        CLAY(CLAY_IDI("ModeRow", channel), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .childGap = 4
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Display:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

            // Mode dropdown button
            const char *mode_name = (trig->scope_mode == SCOPE_MODE_PHOSPHOR) ? "Phosphor" : "Line";
            bool scope_dropdown_open = gui_dropdown_is_open(DROPDOWN_SCOPE_MODE, channel);
            CLAY(CLAY_IDI("ScopeModeBtn", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(18) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                },
                .backgroundColor = to_clay_color(scope_dropdown_open ? COLOR_BUTTON_HOVER : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                CLAY_TEXT(make_string(mode_name),
                    CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        // Per-channel scope mode dropdown options (floating, visible when open)
        if (gui_dropdown_is_open(DROPDOWN_SCOPE_MODE, channel)) {
            CLAY(CLAY_IDI("ScopeModeOpts", channel), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM
                },
                .floating = {
                    .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                    .parentId = CLAY_IDI("ScopeModeBtn", channel).id,
                    .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM }
                },
                .backgroundColor = to_clay_color(COLOR_PANEL_BG),
                .cornerRadius = CLAY_CORNER_RADIUS(3)
            }) {
                // Line mode option
                Color line_color = (trig->scope_mode == SCOPE_MODE_LINE) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                CLAY(CLAY_IDI("ScopeModeOptLine", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(line_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Line"),
                        CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
                }

                // Phosphor mode option
                Color phosphor_color = (trig->scope_mode == SCOPE_MODE_PHOSPHOR) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                CLAY(CLAY_IDI("ScopeModeOptPhos", channel), {
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20) },
                        .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
                    },
                    .backgroundColor = to_clay_color(phosphor_color)
                }) {
                    CLAY_TEXT(CLAY_STRING("Phosphor"),
                        CLAY_TEXT_CONFIG({ .fontSize = 11, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        }

    }
}

// Render the channels panel - each channel has VU meter + waveform + stats grouped together
static void render_channels_panel(gui_app_t *app) {
    // Setup custom element data for this frame
    s_vu_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_a_element.customData.vu_meter.meter = &app->vu_a;
    s_vu_a_element.customData.vu_meter.label = "CH A";
    s_vu_a_element.customData.vu_meter.is_clipping_pos = atomic_load(&app->clip_count_a_pos) > 0;
    s_vu_a_element.customData.vu_meter.is_clipping_neg = atomic_load(&app->clip_count_a_neg) > 0;
    s_vu_a_element.customData.vu_meter.channel_color = COLOR_CHANNEL_A;

    s_osc_a_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_OSCILLOSCOPE;
    s_osc_a_element.customData.oscilloscope.app = app;
    s_osc_a_element.customData.oscilloscope.channel = 0;

    s_vu_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_VU_METER;
    s_vu_b_element.customData.vu_meter.meter = &app->vu_b;
    s_vu_b_element.customData.vu_meter.label = "CH B";
    s_vu_b_element.customData.vu_meter.is_clipping_pos = atomic_load(&app->clip_count_b_pos) > 0;
    s_vu_b_element.customData.vu_meter.is_clipping_neg = atomic_load(&app->clip_count_b_neg) > 0;
    s_vu_b_element.customData.vu_meter.channel_color = COLOR_CHANNEL_B;

    s_osc_b_element.type = CUSTOM_LAYOUT_ELEMENT_TYPE_OSCILLOSCOPE;
    s_osc_b_element.customData.oscilloscope.app = app;
    s_osc_b_element.customData.oscilloscope.channel = 1;

    CLAY(CLAY_ID("ChannelsPanel"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 4, 4, 4, 4 },
            .childGap = 8
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG)
    }) {
        // Channel A row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelARow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter A - custom element
            CLAY(CLAY_ID("VUMeterA"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_vu_a_element }
            }) {}

            // Oscilloscope canvas A - custom element
            CLAY(CLAY_ID("OscilloscopeCanvasA"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_osc_a_element }
            }) {}

            // Stats panel A
            render_channel_stats(app, 0);
        }

        // Channel B row: VU meter + waveform + stats
        CLAY(CLAY_ID("ChannelBRow"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            // VU meter B - custom element
            CLAY(CLAY_ID("VUMeterB"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(70), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_vu_b_element }
            }) {}

            // Oscilloscope canvas B - custom element
            CLAY(CLAY_ID("OscilloscopeCanvasB"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                .custom = { .customData = &s_osc_b_element }
            }) {}

            // Stats panel B
            render_channel_stats(app, 1);
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
        // Sync status indicator
        bool synced = atomic_load(&app->stream_synced);
        Color sync_color = synced ? COLOR_SYNC_GREEN : COLOR_SYNC_RED;
        CLAY(CLAY_ID("SyncStatus"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Sync:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(synced ? CLAY_STRING("OK") : CLAY_STRING("--"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(sync_color) }));
        }

        // Frames count
        uint32_t frames = atomic_load(&app->frame_count);
        snprintf(temp_buf4, sizeof(temp_buf4), "%u", frames);
        CLAY(CLAY_ID("FrameStatus"), {
            .layout = {
                .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .childGap = 4
            }
        }) {
            CLAY_TEXT(CLAY_STRING("Frames:"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            CLAY_TEXT(make_string(temp_buf4),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT) }));
        }

        // Total errors
        uint32_t errors = atomic_load(&app->error_count);
        if (errors > 0) {
            snprintf(temp_buf5, sizeof(temp_buf5), "%u", errors);
            CLAY(CLAY_ID("ErrorStatus"), {
                .layout = {
                    .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_LEFT_TO_RIGHT,
                    .childGap = 4
                }
            }) {
                CLAY_TEXT(CLAY_STRING("Errors:"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY_TEXT(make_string(temp_buf5),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_CLIP_RED) }));
            }
        }

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

            // Get per-channel stats
            uint64_t raw_a = atomic_load(&app->recording_raw_a);
            uint64_t raw_b = atomic_load(&app->recording_raw_b);
            uint64_t comp_a = atomic_load(&app->recording_compressed_a);
            uint64_t comp_b = atomic_load(&app->recording_compressed_b);

            if (app->settings.use_flac && (raw_a > 0 || raw_b > 0)) {
                // FLAC mode: show per-channel raw/compressed/ratio
                double raw_a_mb = (double)raw_a / (1024.0 * 1024.0);
                double raw_b_mb = (double)raw_b / (1024.0 * 1024.0);
                double comp_a_mb = (double)comp_a / (1024.0 * 1024.0);
                double comp_b_mb = (double)comp_b / (1024.0 * 1024.0);
                double ratio_a = (raw_a > 0) ? (double)raw_a / (double)comp_a : 0;
                double ratio_b = (raw_b > 0) ? (double)raw_b / (double)comp_b : 0;

                snprintf(temp_buf2, sizeof(temp_buf2), "A: %.1f/%.1fMB (%.1fx)", raw_a_mb, comp_a_mb, ratio_a);
                CLAY_TEXT(make_string(temp_buf2),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_CHANNEL_A) }));

                snprintf(temp_buf3, sizeof(temp_buf3), "B: %.1f/%.1fMB (%.1fx)", raw_b_mb, comp_b_mb, ratio_b);
                CLAY_TEXT(make_string(temp_buf3),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_CHANNEL_B) }));
            } else {
                // RAW mode or no data yet: show total bytes
                uint64_t bytes = atomic_load(&app->recording_bytes);
                double mb = (double)bytes / (1024.0 * 1024.0);
                snprintf(temp_buf2, sizeof(temp_buf2), "%.1f MB", mb);
                CLAY_TEXT(make_string(temp_buf2),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATUS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            }
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
            snprintf(temp_buf6, sizeof(temp_buf6), "%u MSPS", srate / 1000000);
            CLAY_TEXT(make_string(temp_buf6),
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

        // Main content area - channels panel now includes per-channel stats with trigger controls
        render_channels_panel(app);

        // Status bar
        render_status_bar(app);
    }

    // Device dropdown overlay (if open)
    if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0) && app->device_count > 0) {
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
        // Check connect button
        if (Clay_PointerOver(CLAY_ID("ConnectButton"))) {
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

        // Track if any dropdown element was clicked
        bool dropdown_clicked = false;

        // Check device dropdown
        if (Clay_PointerOver(CLAY_ID("DeviceDropdown"))) {
            gui_dropdown_toggle(DROPDOWN_DEVICE, 0);
            dropdown_clicked = true;
        } else if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0)) {
            // Check device options
            for (int i = 0; i < app->device_count; i++) {
                if (Clay_PointerOver(CLAY_IDI("DeviceOption", i))) {
                    app->selected_device = i;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                    break;
                }
            }
        }

        // Per-channel controls
        for (int ch = 0; ch < 2; ch++) {
            channel_trigger_t *trig = (ch == 0) ? &app->trigger_a : &app->trigger_b;

            // Per-channel trigger mode dropdown (includes Off option)
            if (Clay_PointerOver(CLAY_IDI("TrigModeBtn", ch))) {
                gui_dropdown_toggle(DROPDOWN_TRIGGER_MODE, ch);
                dropdown_clicked = true;
            } else if (gui_dropdown_is_open(DROPDOWN_TRIGGER_MODE, ch)) {
                if (Clay_PointerOver(CLAY_IDI("TrigModeOptOff", ch))) {
                    trig->enabled = false;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                }
                if (Clay_PointerOver(CLAY_IDI("TrigModeOptRising", ch))) {
                    trig->enabled = true;
                    trig->trigger_mode = TRIGGER_MODE_RISING;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                }
                if (Clay_PointerOver(CLAY_IDI("TrigModeOptFalling", ch))) {
                    trig->enabled = true;
                    trig->trigger_mode = TRIGGER_MODE_FALLING;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                }
                if (Clay_PointerOver(CLAY_IDI("TrigModeOptCVBS", ch))) {
                    trig->enabled = true;
                    trig->trigger_mode = TRIGGER_MODE_CVBS_HSYNC;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                }
            }

            // Per-channel scope mode dropdown
            if (Clay_PointerOver(CLAY_IDI("ScopeModeBtn", ch))) {
                gui_dropdown_toggle(DROPDOWN_SCOPE_MODE, ch);
                dropdown_clicked = true;
            } else if (gui_dropdown_is_open(DROPDOWN_SCOPE_MODE, ch)) {
                if (Clay_PointerOver(CLAY_IDI("ScopeModeOptLine", ch))) {
                    trig->scope_mode = SCOPE_MODE_LINE;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                }
                if (Clay_PointerOver(CLAY_IDI("ScopeModeOptPhos", ch))) {
                    trig->scope_mode = SCOPE_MODE_PHOSPHOR;
                    gui_dropdown_close_all();
                    dropdown_clicked = true;
                }
            }
        }

        // Close all dropdowns if clicked elsewhere
        if (!dropdown_clicked) {
            gui_dropdown_close_all();
        }
    }
}

