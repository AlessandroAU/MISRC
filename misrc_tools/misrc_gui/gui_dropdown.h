/*
 * MISRC GUI - Generalized Dropdown System
 *
 * Centralized dropdown management for consistent behavior across the UI.
 * Only one dropdown can be open at a time.
 */

#ifndef GUI_DROPDOWN_H
#define GUI_DROPDOWN_H

#include <stdbool.h>
#include <stdint.h>

//-----------------------------------------------------------------------------
// State Management
//-----------------------------------------------------------------------------

// Open a specific dropdown (closes any other open dropdown)
// id: unique string identifier, index: numeric index for indexed dropdowns (0 if not used)
void gui_dropdown_open(const char *id, uint32_t index);

// Close all dropdowns
void gui_dropdown_close_all(void);

// Check if a specific dropdown is open
bool gui_dropdown_is_open(const char *id, uint32_t index);

// Toggle a dropdown (open if closed, close if open)
// Also closes all other dropdowns
void gui_dropdown_toggle(const char *id, uint32_t index);

#endif // GUI_DROPDOWN_H
