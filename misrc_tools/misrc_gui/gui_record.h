/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses a dedicated extraction thread to ensure recording is not affected
 * by UI thread blocking (e.g., window dragging).
 */

#ifndef GUI_RECORD_H
#define GUI_RECORD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../ringbuffer.h"

// Forward declarations
typedef struct gui_app gui_app_t;

// Initialize recording subsystem
void gui_record_init(void);

// Cleanup recording subsystem
void gui_record_cleanup(void);

// Start recording to files
// Returns 0 on success, -1 on error
int gui_record_start(gui_app_t *app, ringbuffer_t *capture_rb);

// Stop recording
void gui_record_stop(gui_app_t *app);

// Check if recording is active
bool gui_record_is_active(void);

#endif // GUI_RECORD_H
