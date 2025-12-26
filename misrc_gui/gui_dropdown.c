/*
 * MISRC GUI - Generalized Dropdown System Implementation
 *
 * Provides centralized state management, rendering helpers, and
 * interaction handling for dropdowns.
 * Only one dropdown can be open at a time across the entire UI.
 */

#include "gui_dropdown.h"
#include "gui_app.h"
#include "gui_panel.h"
#include "gui_ui.h"
#include <string.h>

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

// Currently open dropdown (only one can be open at a time)
static struct {
    bool is_open;
    char id[64];
    uint32_t index;
} s_open_dropdown = {0};

//-----------------------------------------------------------------------------
// State Management
//-----------------------------------------------------------------------------

void gui_dropdown_open(const char *id, uint32_t index) {
    s_open_dropdown.is_open = true;
    strncpy(s_open_dropdown.id, id, sizeof(s_open_dropdown.id) - 1);
    s_open_dropdown.id[sizeof(s_open_dropdown.id) - 1] = '\0';
    s_open_dropdown.index = index;
}

void gui_dropdown_close_all(void) {
    s_open_dropdown.is_open = false;
    s_open_dropdown.id[0] = '\0';
    s_open_dropdown.index = 0;
}

bool gui_dropdown_is_open(const char *id, uint32_t index) {
    if (!s_open_dropdown.is_open) return false;
    if (!id) return false;
    return (strcmp(s_open_dropdown.id, id) == 0 && s_open_dropdown.index == index);
}

void gui_dropdown_toggle(const char *id, uint32_t index) {
    if (gui_dropdown_is_open(id, index)) {
        gui_dropdown_close_all();
    } else {
        gui_dropdown_open(id, index);
    }
}

//-----------------------------------------------------------------------------
// Rendering Helpers
//-----------------------------------------------------------------------------

Color gui_dropdown_option_color(bool is_selected, bool is_hovered) {
    if (is_selected) return COLOR_BUTTON_ACTIVE;
    if (is_hovered) return COLOR_BUTTON_HOVER;
    return COLOR_BUTTON;
}

//-----------------------------------------------------------------------------
// Selection Handlers (internal)
//-----------------------------------------------------------------------------

// Handle device selection
static bool handle_device_dropdown(gui_app_t *app) {
    bool clicked = false;

    if (Clay_PointerOver(CLAY_ID("DeviceDropdown"))) {
        gui_dropdown_toggle(DROPDOWN_DEVICE, 0);
        clicked = true;
    } else if (gui_dropdown_is_open(DROPDOWN_DEVICE, 0)) {
        for (int i = 0; i < app->device_count; i++) {
            if (Clay_PointerOver(CLAY_IDI("DeviceOption", i))) {
                if (i != app->selected_device) {
                    // Switch to a different device - stop current and start new
                    bool was_capturing = app->is_capturing;
                    if (was_capturing) {
                        gui_app_stop_capture(app);
                    }
                    app->selected_device = i;
                    if (was_capturing) {
                        gui_app_start_capture(app);
                    }
                }
                gui_dropdown_close_all();
                clicked = true;
                break;
            }
        }
    }

    return clicked;
}

// Handle trigger mode selection for a channel
static bool handle_trigger_mode_dropdown(gui_app_t *app, int ch) {
    bool clicked = false;
    channel_trigger_t *trig = (ch == 0) ? &app->trigger_a : &app->trigger_b;

    if (Clay_PointerOver(CLAY_IDI("TrigModeBtn", ch))) {
        gui_dropdown_toggle(DROPDOWN_TRIGGER_MODE, ch);
        clicked = true;
    } else if (gui_dropdown_is_open(DROPDOWN_TRIGGER_MODE, ch)) {
        if (Clay_PointerOver(CLAY_IDI("TrigModeOptOff", ch))) {
            trig->enabled = false;
            gui_dropdown_close_all();
            clicked = true;
        }
        if (Clay_PointerOver(CLAY_IDI("TrigModeOptRising", ch))) {
            trig->enabled = true;
            trig->trigger_mode = TRIGGER_MODE_RISING;
            gui_dropdown_close_all();
            clicked = true;
        }
        if (Clay_PointerOver(CLAY_IDI("TrigModeOptFalling", ch))) {
            trig->enabled = true;
            trig->trigger_mode = TRIGGER_MODE_FALLING;
            gui_dropdown_close_all();
            clicked = true;
        }
        if (Clay_PointerOver(CLAY_IDI("TrigModeOptCVBS", ch))) {
            trig->enabled = true;
            trig->trigger_mode = TRIGGER_MODE_CVBS_HSYNC;
            gui_dropdown_close_all();
            clicked = true;
        }
    }

    return clicked;
}

// Handle layout selection for a channel
static bool handle_layout_dropdown(gui_app_t *app, int ch) {
    bool clicked = false;

    // Get panel config pointers for this channel
    bool *cfg_split = (ch == 0) ? &app->panel_config_a.split : &app->panel_config_b.split;
    int *cfg_right_view = (ch == 0) ? &app->panel_config_a.right_view : &app->panel_config_b.right_view;
    void **cfg_right_state = (ch == 0) ? &app->panel_config_a.right_state : &app->panel_config_b.right_state;

    if (Clay_PointerOver(CLAY_IDI("LayoutBtn", ch))) {
        gui_dropdown_toggle(DROPDOWN_LAYOUT, ch);
        clicked = true;
    } else if (gui_dropdown_is_open(DROPDOWN_LAYOUT, ch)) {
        if (Clay_PointerOver(CLAY_IDI("LayoutOptSingle", ch))) {
            if (*cfg_split) {
                *cfg_split = false;
                // Destroy right panel state if it exists
                if (*cfg_right_state) {
                    panel_destroy_view_state((panel_view_type_t)*cfg_right_view, *cfg_right_state);
                    *cfg_right_state = NULL;
                }
            }
            gui_dropdown_close_all();
            clicked = true;
        }
        if (Clay_PointerOver(CLAY_IDI("LayoutOptSplit", ch))) {
            if (!*cfg_split) {
                *cfg_split = true;
                // Create right panel state if needed
                *cfg_right_state = panel_create_view_state((panel_view_type_t)*cfg_right_view);
            }
            gui_dropdown_close_all();
            clicked = true;
        }
    }

    return clicked;
}

// Handle left view selection for a channel
static bool handle_left_view_dropdown(gui_app_t *app, int ch) {
    bool clicked = false;

    int *cfg_left_view = (ch == 0) ? &app->panel_config_a.left_view : &app->panel_config_b.left_view;
    void **cfg_left_state = (ch == 0) ? &app->panel_config_a.left_state : &app->panel_config_b.left_state;

    if (Clay_PointerOver(CLAY_IDI("LeftViewBtn", ch))) {
        gui_dropdown_toggle(DROPDOWN_LEFT_VIEW, ch);
        clicked = true;
    } else if (gui_dropdown_is_open(DROPDOWN_LEFT_VIEW, ch)) {
        for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
            if (!panel_view_type_available((panel_view_type_t)vt)) continue;
            // Use ch * 10 + vt to match the ID used in rendering
            if (Clay_PointerOver(CLAY_IDI("LeftViewOpt", ch * 10 + vt))) {
                if (*cfg_left_view != vt) {
                    // Destroy old state
                    if (*cfg_left_state) {
                        panel_destroy_view_state((panel_view_type_t)*cfg_left_view, *cfg_left_state);
                        *cfg_left_state = NULL;
                    }
                    *cfg_left_view = vt;
                    *cfg_left_state = panel_create_view_state((panel_view_type_t)vt);
                }
                gui_dropdown_close_all();
                clicked = true;
                break;
            }
        }
    }

    return clicked;
}

// Handle right view selection for a channel
static bool handle_right_view_dropdown(gui_app_t *app, int ch) {
    bool clicked = false;

    bool cfg_split = (ch == 0) ? app->panel_config_a.split : app->panel_config_b.split;
    if (!cfg_split) return false;  // Right view only visible when split

    int *cfg_right_view = (ch == 0) ? &app->panel_config_a.right_view : &app->panel_config_b.right_view;
    void **cfg_right_state = (ch == 0) ? &app->panel_config_a.right_state : &app->panel_config_b.right_state;

    if (Clay_PointerOver(CLAY_IDI("RightViewBtn", ch))) {
        gui_dropdown_toggle(DROPDOWN_RIGHT_VIEW, ch);
        clicked = true;
    } else if (gui_dropdown_is_open(DROPDOWN_RIGHT_VIEW, ch)) {
        for (int vt = 0; vt < PANEL_VIEW_COUNT; vt++) {
            if (!panel_view_type_available((panel_view_type_t)vt)) continue;
            // Use ch * 10 + vt to match the ID used in rendering
            if (Clay_PointerOver(CLAY_IDI("RightViewOpt", ch * 10 + vt))) {
                if (*cfg_right_view != vt) {
                    // Destroy old state
                    if (*cfg_right_state) {
                        panel_destroy_view_state((panel_view_type_t)*cfg_right_view, *cfg_right_state);
                        *cfg_right_state = NULL;
                    }
                    *cfg_right_view = vt;
                    *cfg_right_state = panel_create_view_state((panel_view_type_t)vt);
                }
                gui_dropdown_close_all();
                clicked = true;
                break;
            }
        }
    }

    return clicked;
}

//-----------------------------------------------------------------------------
// Centralized Interaction Handler
//-----------------------------------------------------------------------------

bool gui_dropdown_handle_click(gui_app_t *app) {
    bool dropdown_clicked = false;

    // Device dropdown (global)
    if (handle_device_dropdown(app)) {
        dropdown_clicked = true;
    }

    // Per-channel dropdowns
    for (int ch = 0; ch < 2; ch++) {
        if (handle_trigger_mode_dropdown(app, ch)) {
            dropdown_clicked = true;
        }
        if (handle_layout_dropdown(app, ch)) {
            dropdown_clicked = true;
        }
        if (handle_left_view_dropdown(app, ch)) {
            dropdown_clicked = true;
        }
        if (handle_right_view_dropdown(app, ch)) {
            dropdown_clicked = true;
        }
    }

    // Close all dropdowns if clicked elsewhere
    if (!dropdown_clicked) {
        gui_dropdown_close_all();
    }

    return dropdown_clicked;
}
