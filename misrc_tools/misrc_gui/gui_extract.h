/*
 * MISRC GUI - Sample Extraction and Display Processing
 *
 * Shared functions for extracting samples and updating display buffers.
 * Used by both the UI thread (when not recording) and the recording
 * extraction thread (when recording).
 */

#ifndef GUI_EXTRACT_H
#define GUI_EXTRACT_H

#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct gui_app gui_app_t;

// Initialize extraction subsystem (allocates buffers, gets extraction function)
void gui_extract_init(void);

// Cleanup extraction subsystem
void gui_extract_cleanup(void);

// Get the extraction function pointer (for direct use)
typedef void (*extract_fn_t)(uint32_t *buf, size_t num_samples, size_t *clip,
                             uint8_t *aux_buf, int16_t *buf_a, int16_t *buf_b,
                             uint16_t *peak);
extract_fn_t gui_extract_get_function(void);

// Get extraction output buffers (for direct use)
int16_t *gui_extract_get_buf_a(void);
int16_t *gui_extract_get_buf_b(void);
uint8_t *gui_extract_get_buf_aux(void);

// Update clip counts and peak values from extracted samples
// Processes sample buffers and updates app's atomic counters
void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples);

// Update display buffer with min/max decimation
// Decimates samples to fit display width while preserving peaks
void gui_extract_update_display(gui_app_t *app, const int16_t *buf_a,
                                const int16_t *buf_b, size_t num_samples);

#endif // GUI_EXTRACT_H
