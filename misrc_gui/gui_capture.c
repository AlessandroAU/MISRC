/*
 * MISRC GUI - Capture Integration
 *
 * Uses the same ringbuffer and extraction pattern as misrc_capture.c
 */

#include "gui_capture.h"
#include "gui_app.h"
#include "gui_render.h"
#include "gui_record.h"
#include "gui_extract.h"
#include "gui_oscilloscope.h"
#include "gui_phosphor.h"

#include <hsdaoh.h>
#include <hsdaoh_raw.h>
#include "../misrc_tools/extract.h"
#include "../misrc_common/ringbuffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#endif

// Get current time in milliseconds
static uint64_t get_time_ms(void) {
#ifdef _WIN32
    extern __declspec(dllimport) unsigned long __stdcall GetTickCount(void);
    return (uint64_t)GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
#endif
}

// Buffer sizes - match reference implementation
#define BUFFER_READ_SIZE 65536
#define BUFFER_TOTAL_SIZE (65536 * 1024)  // Same as reference: 64MB

// Ringbuffer for raw capture data (written by callback, read by main thread)
static ringbuffer_t s_capture_rb;
static bool s_rb_initialized = false;

// Frame tracking state (like reference implementation)
static uint16_t s_last_frame_cnt = 0;
static unsigned int s_in_order_cnt = 0;

// Message callback for hsdaoh
static void gui_message_callback(void *ctx, enum hsdaoh_msg_level level, const char *format, ...) {
    gui_app_t *app = (gui_app_t *)ctx;

    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Update status message
    if (level == HSDAOH_ERROR || level == HSDAOH_CRITICAL) {
        gui_app_set_status(app, buffer);
        atomic_fetch_add(&app->error_count, 1);
    }

    // Print to console for debugging
    const char *level_str = "INFO";
    if (level == HSDAOH_WARNING) level_str = "WARN";
    else if (level == HSDAOH_ERROR) level_str = "ERROR";
    else if (level == HSDAOH_CRITICAL) level_str = "CRITICAL";

    fprintf(stderr, "[%s] %s", level_str, buffer);
}

// Debug counter
static int s_callback_count = 0;

// Main capture callback - writes raw data to ringbuffer (like reference implementation)
void gui_capture_callback(void *data_info_ptr) {
    hsdaoh_data_info_t *data_info = (hsdaoh_data_info_t *)data_info_ptr;
    gui_app_t *app = (gui_app_t *)data_info->ctx;

    s_callback_count++;

    if (atomic_load(&do_exit)) return;
    if (!app) return;
    if (!data_info->buf) return;
    if (data_info->width == 0 || data_info->height == 0) return;

    if (data_info->device_error) {
        atomic_store(&app->stream_synced, false);
        return;
    }

    // Extract metadata from frame
    metadata_t meta;
    hsdaoh_extract_metadata(data_info->buf, &meta, data_info->width);

    bool was_synced = atomic_load(&app->stream_synced);

    // Validate magic number
    if (meta.magic != HSDAOH_MAGIC) {
        if (was_synced) {
            fprintf(stderr, "[CB] Lost sync to HDMI input stream\n");
        }
        atomic_store(&app->stream_synced, false);
        s_in_order_cnt = 0;
        return;
    }

    // Drop duplicate frames (like reference implementation)
    if (meta.framecounter == s_last_frame_cnt) {
        return;
    }

    // Check for missed frames
    if (meta.framecounter != ((s_last_frame_cnt + 1) & 0xffff)) {
        s_in_order_cnt = 0;
        if (was_synced) {
            fprintf(stderr, "[CB] Missed frame(s), fcnt %d, expected %d\n",
                    meta.framecounter, ((s_last_frame_cnt + 1) & 0xffff));
        }
    } else {
        s_in_order_cnt++;
    }

    s_last_frame_cnt = meta.framecounter;

    // Don't process until we have enough in-order frames (like reference: >4)
    if (!was_synced && s_in_order_cnt <= 4) {
        return;
    }

    // Now consider stream synced
    if (!was_synced) {
        fprintf(stderr, "[CB] Synchronized to HDMI input stream\n");
        atomic_store(&app->stream_synced, true);
    }

    // Update last callback time for disconnect detection
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    atomic_fetch_add(&app->frame_count, 1);

    // Update sample rate from metadata
    if (meta.stream_info[0].srate > 0) {
        atomic_store(&app->sample_rate, meta.stream_info[0].srate);
    }

    // First pass: calculate total payload size and validate
    size_t stream0_payload_bytes = 0;
    bool has_stream_id = (meta.flags & FLAG_STREAM_ID_PRESENT) != 0;
    bool frame_valid = true;

    for (unsigned int line = 0; line < data_info->height; line++) {
        uint8_t *line_dat = data_info->buf + (data_info->width * sizeof(uint16_t) * line);
        uint16_t payload_len = ((uint16_t *)line_dat)[data_info->width - 1] & 0x0FFF;
        uint16_t stream_id = has_stream_id ? (((uint16_t *)line_dat)[data_info->width - 3] & 0x0FFF) : 0;

        // Validate payload length
        if (payload_len > data_info->width - 1) {
            fprintf(stderr, "[CB] Invalid payload length: %d\n", payload_len);
            frame_valid = false;
            break;
        }

        if (payload_len == 0) {
            continue;
        }
        if (stream_id != 0) {
            continue;
        }
        stream0_payload_bytes += payload_len * sizeof(uint16_t);
    }

    // Don't process invalid frames
    if (!frame_valid || stream0_payload_bytes == 0) {
        return;
    }

    // Wait for ringbuffer space (like reference implementation)
    // This ensures we don't drop frames when the consumer is slow
    uint8_t *buf_out;
    while ((buf_out = rb_write_ptr(&s_capture_rb, stream0_payload_bytes)) == NULL) {
        if (atomic_load(&do_exit)) return;
        // Sleep briefly to allow consumer to drain buffer
        // Use Windows Sleep directly to avoid raylib conflicts
#ifdef _WIN32
        {
            extern __declspec(dllimport) void __stdcall Sleep(unsigned long);
            Sleep(4);
        }
#else
        usleep(4000);
#endif
    }

    // Second pass: copy payload data to ringbuffer
    size_t offset = 0;
    for (unsigned int line = 0; line < data_info->height; line++) {
        uint8_t *line_dat = data_info->buf + (data_info->width * sizeof(uint16_t) * line);
        uint16_t payload_len = ((uint16_t *)line_dat)[data_info->width - 1] & 0x0FFF;
        uint16_t stream_id = has_stream_id ? (((uint16_t *)line_dat)[data_info->width - 3] & 0x0FFF) : 0;

        if (payload_len == 0 || payload_len > data_info->width - 1) {
            continue;
        }
        if (stream_id != 0) {
            continue;
        }

        memcpy(buf_out + offset, line_dat, payload_len * sizeof(uint16_t));
        offset += payload_len * sizeof(uint16_t);
    }

    // Mark write complete with actual bytes written
    rb_write_finished(&s_capture_rb, stream0_payload_bytes);

    if (s_callback_count <= 3) {
        fprintf(stderr, "[CB] Wrote %zu bytes to ringbuffer\n", stream0_payload_bytes);
    }
}

// Initialize application
void gui_app_init(gui_app_t *app) {
    // Initialize per-channel display buffers
    memset(app->display_samples_a, 0, sizeof(app->display_samples_a));
    memset(app->display_samples_b, 0, sizeof(app->display_samples_b));
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    app->vu_a.level_pos = 0;
    app->vu_a.level_neg = 0;
    app->vu_a.peak_pos = 0;
    app->vu_a.peak_neg = 0;
    app->vu_a.peak_hold_time_pos = 0;
    app->vu_a.peak_hold_time_neg = 0;

    app->vu_b.level_pos = 0;
    app->vu_b.level_neg = 0;
    app->vu_b.peak_pos = 0;
    app->vu_b.peak_neg = 0;
    app->vu_b.peak_hold_time_pos = 0;
    app->vu_b.peak_hold_time_neg = 0;

    strcpy(app->status_message, "Initializing...");

    // Initialize trigger state for channel A
    app->trigger_a.enabled = false;
    app->trigger_a.level = 0;
    app->trigger_a.zoom_scale = ZOOM_SCALE_DEFAULT;
    app->trigger_a.trigger_display_pos = -1;
    atomic_store(&app->trigger_a.display_width, DISPLAY_BUFFER_SIZE);  // Will be updated by renderer
    app->trigger_a.scope_mode = SCOPE_MODE_PHOSPHOR;  // Phosphor mode by default
    app->trigger_a.trigger_mode = TRIGGER_MODE_RISING;  // Rising edge by default

    // Initialize trigger state for channel B
    app->trigger_b.enabled = false;
    app->trigger_b.level = 0;
    app->trigger_b.zoom_scale = ZOOM_SCALE_DEFAULT;
    app->trigger_b.trigger_display_pos = -1;
    atomic_store(&app->trigger_b.display_width, DISPLAY_BUFFER_SIZE);  // Will be updated by renderer
    app->trigger_b.scope_mode = SCOPE_MODE_PHOSPHOR;  // Phosphor mode by default
    app->trigger_b.trigger_mode = TRIGGER_MODE_RISING;  // Rising edge by default

    // Initialize phosphor display state
    app->phosphor_a = NULL;
    app->phosphor_b = NULL;
    app->phosphor_pixels_a = NULL;
    app->phosphor_pixels_b = NULL;
    memset(&app->phosphor_image_a, 0, sizeof(Image));
    memset(&app->phosphor_image_b, 0, sizeof(Image));
    memset(&app->phosphor_texture_a, 0, sizeof(Texture2D));
    memset(&app->phosphor_texture_b, 0, sizeof(Texture2D));
    app->phosphor_width = 0;
    app->phosphor_height = 0;
    app->phosphor_textures_valid = false;

    // Initialize capture ringbuffer
    if (!s_rb_initialized) {
        int r = rb_init(&s_capture_rb, "gui_capture", BUFFER_TOTAL_SIZE);
        if (r != 0) {
            fprintf(stderr, "Failed to initialize capture ringbuffer: %d\n", r);
        } else {
            s_rb_initialized = true;
            fprintf(stderr, "Capture ringbuffer initialized (%d bytes)\n", BUFFER_TOTAL_SIZE);
        }
    }

    // Set render app for custom rendering
    set_render_app(app);
}

// Cleanup application
void gui_app_cleanup(gui_app_t *app) {
    if (app->is_capturing) {
        gui_app_stop_capture(app);
    }

    // Close ringbuffer
    if (s_rb_initialized) {
        rb_close(&s_capture_rb);
        s_rb_initialized = false;
    }

    // Cleanup extraction subsystem
    gui_extract_cleanup();

    // Cleanup phosphor buffers and textures
    gui_phosphor_cleanup(app);

    // Cleanup oscilloscope resources (resamplers)
    gui_oscilloscope_cleanup();
}

// Enumerate available capture devices
void gui_app_enumerate_devices(gui_app_t *app) {
    app->device_count = 0;

    // Enumerate hsdaoh devices
    uint32_t hsdaoh_count = hsdaoh_get_device_count();

    for (uint32_t i = 0; i < hsdaoh_count && app->device_count < MAX_DEVICES; i++) {
        const char *name = hsdaoh_get_device_name(i);

        device_info_t *dev = &app->devices[app->device_count];
        snprintf(dev->name, sizeof(dev->name), "%s", name ? name : "Unknown");
        dev->serial[0] = '\0';  // Serial obtained after device open
        dev->is_simple_capture = false;
        dev->index = i;

        app->device_count++;
    }

    if (app->device_count == 0) {
        gui_app_set_status(app, "No capture devices found");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Found %d device(s)", app->device_count);
        gui_app_set_status(app, msg);
    }
}

// Start capture
int gui_app_start_capture(gui_app_t *app) {
    fprintf(stderr, "[GUI] gui_app_start_capture called\n");

    if (app->is_capturing) {
        fprintf(stderr, "[GUI] Already capturing\n");
        return 0;
    }

    if (app->device_count == 0) {
        fprintf(stderr, "[GUI] No devices available\n");
        gui_app_set_status(app, "No devices available");
        return -1;
    }

    if (!s_rb_initialized) {
        fprintf(stderr, "[GUI] Ringbuffer not initialized\n");
        gui_app_set_status(app, "Ringbuffer not initialized");
        return -1;
    }

    device_info_t *dev = &app->devices[app->selected_device];
    fprintf(stderr, "[GUI] Selected device: %s (index %d)\n", dev->name, dev->index);

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, 0);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers (per-channel)
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset callback counter and frame tracking
    s_callback_count = 0;
    s_last_frame_cnt = 0;
    s_in_order_cnt = 0;

    // Open device
    fprintf(stderr, "[GUI] Allocating device...\n");
    int r = hsdaoh_alloc(&app->hs_dev);
    if (r < 0) {
        fprintf(stderr, "[GUI] hsdaoh_alloc failed: %d\n", r);
        gui_app_set_status(app, "Failed to allocate device");
        return -1;
    }

    hsdaoh_set_msg_callback(app->hs_dev, gui_message_callback, app);
    hsdaoh_raw_callback(app->hs_dev, true);

    fprintf(stderr, "[GUI] Opening device index %d...\n", dev->index);
    r = hsdaoh_open2(app->hs_dev, dev->index);
    if (r < 0) {
        fprintf(stderr, "[GUI] hsdaoh_open2 failed: %d\n", r);
        gui_app_set_status(app, "Failed to open device");
        // Note: hsdaoh_open2 frees dev on failure, so DON'T call hsdaoh_close
        app->hs_dev = NULL;
        return -1;
    }

    fprintf(stderr, "[GUI] Starting stream...\n");
    r = hsdaoh_start_stream(app->hs_dev, (hsdaoh_read_cb_t)gui_capture_callback, app);
    if (r < 0) {
        fprintf(stderr, "[GUI] hsdaoh_start_stream failed: %d\n", r);
        gui_app_set_status(app, "Failed to start stream");
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
        return -1;
    }

    app->is_capturing = true;

    // Start the extraction thread - runs continuously from capture start
    r = gui_extract_start(app, &s_capture_rb);
    if (r < 0) {
        fprintf(stderr, "[GUI] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        hsdaoh_stop_stream(app->hs_dev);
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
        app->is_capturing = false;
        return -1;
    }

    gui_app_set_status(app, "Capturing...");

    return 0;
}

// Stop capture
void gui_app_stop_capture(gui_app_t *app) {
    if (!app->is_capturing) {
        return;
    }

    if (app->is_recording) {
        gui_app_stop_recording(app);
    }

    // Set is_capturing to false BEFORE stopping extraction thread
    // The extraction thread checks this flag to know when to exit
    app->is_capturing = false;

    // Stop extraction thread before closing device
    gui_extract_stop();

    if (app->hs_dev) {
        hsdaoh_stop_stream(app->hs_dev);
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
    }

    atomic_store(&app->stream_synced, false);

    // Clear display to show "No Signal"
    gui_app_clear_display(app);

    gui_app_set_status(app, "Capture stopped");
}

// Recording wrappers - delegate to gui_record module
int gui_app_start_recording(gui_app_t *app) {
    return gui_record_start(app);
}

void gui_app_stop_recording(gui_app_t *app) {
    gui_record_stop(app);
}

// Helper to update one direction of VU meter (pos or neg)
static void update_vu_direction(float *level, float *peak, float *peak_hold_time,
                                 float target, float dt) {
    // Fast attack, slow release
    if (target > *level) {
        *level = target;
    } else {
        *level += (target - *level) * dt / VU_RELEASE_TIME;
    }

    // Peak hold
    if (target > *peak) {
        *peak = target;
        *peak_hold_time = 0;
    } else {
        *peak_hold_time += dt;
        if (*peak_hold_time > PEAK_HOLD_DURATION) {
            *peak -= dt * PEAK_DECAY_RATE;
            if (*peak < 0) *peak = 0;
        }
    }
}

// Update VU meters - tracks positive and negative separately for AC signals
void gui_app_update_vu_meters(gui_app_t *app, float dt) {
    // Get current peak values (separate pos/neg)
    uint16_t peak_a_pos = atomic_load(&app->peak_a_pos);
    uint16_t peak_a_neg = atomic_load(&app->peak_a_neg);
    uint16_t peak_b_pos = atomic_load(&app->peak_b_pos);
    uint16_t peak_b_neg = atomic_load(&app->peak_b_neg);

    // Convert to normalized levels (0-1), using 2048 as full scale
    float level_a_pos = (float)peak_a_pos / 2048.0f;
    float level_a_neg = (float)peak_a_neg / 2048.0f;
    float level_b_pos = (float)peak_b_pos / 2048.0f;
    float level_b_neg = (float)peak_b_neg / 2048.0f;

    // Clamp to 1.0
    if (level_a_pos > 1.0f) level_a_pos = 1.0f;
    if (level_a_neg > 1.0f) level_a_neg = 1.0f;
    if (level_b_pos > 1.0f) level_b_pos = 1.0f;
    if (level_b_neg > 1.0f) level_b_neg = 1.0f;

    // Update channel A (positive and negative separately)
    update_vu_direction(&app->vu_a.level_pos, &app->vu_a.peak_pos,
                        &app->vu_a.peak_hold_time_pos, level_a_pos, dt);
    update_vu_direction(&app->vu_a.level_neg, &app->vu_a.peak_neg,
                        &app->vu_a.peak_hold_time_neg, level_a_neg, dt);

    // Update channel B (positive and negative separately)
    update_vu_direction(&app->vu_b.level_pos, &app->vu_b.peak_pos,
                        &app->vu_b.peak_hold_time_pos, level_b_pos, dt);
    update_vu_direction(&app->vu_b.level_neg, &app->vu_b.peak_neg,
                        &app->vu_b.peak_hold_time_neg, level_b_neg, dt);
}

// Update display buffer (called from main thread)
// Note: Display is now updated by the continuous extraction thread
void gui_app_update_display_buffer(gui_app_t *app) {
    (void)app;
    // No-op: extraction thread handles display updates continuously
}

// Clear display buffer and reset VU meters (called when device disconnects)
void gui_app_clear_display(gui_app_t *app) {
    // Clear display samples (per-channel)
    memset(app->display_samples_a, 0, sizeof(app->display_samples_a));
    memset(app->display_samples_b, 0, sizeof(app->display_samples_b));
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset VU meters
    app->vu_a.level_pos = 0;
    app->vu_a.level_neg = 0;
    app->vu_a.peak_pos = 0;
    app->vu_a.peak_neg = 0;
    app->vu_a.peak_hold_time_pos = 0;
    app->vu_a.peak_hold_time_neg = 0;

    app->vu_b.level_pos = 0;
    app->vu_b.level_neg = 0;
    app->vu_b.peak_pos = 0;
    app->vu_b.peak_neg = 0;
    app->vu_b.peak_hold_time_pos = 0;
    app->vu_b.peak_hold_time_neg = 0;

    // Reset peak values
    atomic_store(&app->peak_a_pos, 0);
    atomic_store(&app->peak_a_neg, 0);
    atomic_store(&app->peak_b_pos, 0);
    atomic_store(&app->peak_b_neg, 0);

    // Reset stream sync status
    atomic_store(&app->stream_synced, false);
}

// Set status message
void gui_app_set_status(gui_app_t *app, const char *message) {
    strncpy(app->status_message, message, sizeof(app->status_message) - 1);
    app->status_message[sizeof(app->status_message) - 1] = '\0';
    app->status_message_time = GetTime();
}

// Check if device has timed out (no callbacks for too long)
bool gui_capture_device_timeout(gui_app_t *app, uint32_t timeout_ms) {
    if (!app->is_capturing) return false;

    uint64_t last_cb = atomic_load(&app->last_callback_time_ms);
    uint64_t now = get_time_ms();

    // Handle wrap-around (GetTickCount wraps every ~49 days)
    uint64_t elapsed = now - last_cb;
    if (now < last_cb) {
        // Wrap-around occurred
        elapsed = (UINT64_MAX - last_cb) + now + 1;
    }

    return elapsed > timeout_ms;
}
