/*
 * MISRC GUI - Soundcard Capture (WASAPI)
 *
 * Captures audio from the computer's soundcard for VHS linear audio recording.
 * Uses Windows Audio Session API (WASAPI) for low-latency capture.
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#ifndef GUI_SOUNDCARD_H
#define GUI_SOUNDCARD_H

#include <stdbool.h>
#include <stdint.h>

// Forward declaration
typedef struct gui_app gui_app_t;

// Maximum number of soundcard devices to enumerate
#define MAX_SOUNDCARD_DEVICES 16

// Soundcard device info for enumeration
typedef struct {
    char name[128];         // Human-readable device name
    char device_id[256];    // WASAPI device ID (for reconnect)
    bool is_default;        // True if this is the default capture device
} soundcard_device_info_t;

/*
 * Enumerate available soundcard capture devices.
 *
 * @param devices   Array to fill with device info
 * @param max       Maximum number of devices to enumerate
 * @return Number of devices found, or -1 on error
 */
int gui_soundcard_enumerate_devices(soundcard_device_info_t *devices, int max);

/*
 * Start soundcard capture.
 *
 * Initializes WASAPI and starts capturing from the selected device.
 * Captured audio is written to BUF_SOUNDCARD_AUDIO ringbuffer.
 *
 * @param app   Application state
 * @return 0 on success, -1 on error
 */
int gui_soundcard_start(gui_app_t *app);

/*
 * Stop soundcard capture.
 *
 * Signals the capture thread to stop and waits for it to finish.
 *
 * @param app   Application state
 */
void gui_soundcard_stop(gui_app_t *app);

/*
 * Check if soundcard capture is running.
 *
 * @param app   Application state
 * @return true if capture is active
 */
bool gui_soundcard_is_running(gui_app_t *app);

/*
 * Enable/disable soundcard recording.
 *
 * When enabled, captured audio is written to BUF_SOUNDCARD_RECORD
 * for the FLAC writer to consume. When disabled, audio is only
 * written to BUF_SOUNDCARD_AUDIO for VU meter display.
 *
 * @param enabled   true to enable recording, false to disable
 */
void gui_soundcard_set_recording(bool enabled);

#endif // GUI_SOUNDCARD_H
