#ifndef GUI_CAPTURE_H
#define GUI_CAPTURE_H

#include "gui_app.h"

// Global exit flag (defined in misrc_gui.c)
extern volatile atomic_int do_exit;

// Capture callback function
void gui_capture_callback(void *data_info);

// Set the render app pointer for custom rendering
void set_render_app(gui_app_t *app);

// Check if device has timed out (no callbacks for too long)
// Returns true if device appears disconnected
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms);

#endif // GUI_CAPTURE_H
