#ifndef GUI_CAPTURE_H
#define GUI_CAPTURE_H

#include "gui_app.h"

// Global exit flag (defined in misrc_gui.c)
extern volatile atomic_int do_exit;

// Capture callback function
void gui_capture_callback(void *data_info);

// Set the render app pointer for custom rendering
void set_render_app(gui_app_t *app);

#endif // GUI_CAPTURE_H
