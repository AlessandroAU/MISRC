/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#ifndef GUI_RECORD_H
#define GUI_RECORD_H

#include <stdbool.h>

// Forward declarations
typedef struct gui_app gui_app_t;

// Initialize recording subsystem
void gui_record_init(void);

// Cleanup recording subsystem
void gui_record_cleanup(void);

// Start recording to files
// Returns 0 on success, -1 on error
int gui_record_start(gui_app_t *app);

// Stop recording
void gui_record_stop(gui_app_t *app);

// Check if recording is active
bool gui_record_is_active(void);

#endif // GUI_RECORD_H
