/*
 * MISRC GUI - Capture Integration
 *
 * Uses the same ringbuffer and extraction pattern as misrc_capture.c
 */

#include "gui_capture.h"
#include "gui_app.h"
#include "gui_render.h"

#include <hsdaoh.h>
#include <hsdaoh_raw.h>
#include "../extract.h"
#include "../ringbuffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(align, size) _aligned_malloc(size, align)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

// Buffer sizes - match reference implementation
// Reference uses 65536*32 but we use smaller for GUI responsiveness
#define BUFFER_READ_SIZE 65536
#define BUFFER_TOTAL_SIZE (65536 * 1024)  // Same as reference: 64MB

// Ringbuffer for raw capture data (written by callback, read by main thread)
static ringbuffer_t s_capture_rb;
static bool s_rb_initialized = false;

// Extraction output buffers (allocated from ringbuffer-aligned memory)
static uint8_t *s_buf_aux = NULL;

// Cached extraction function
static conv_function_t s_extract_fn = NULL;

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

    // Validate magic number
    if (meta.magic != HSDAOH_MAGIC) {
        atomic_store(&app->stream_synced, false);
        return;
    }

    // Check if this is the first synced frame
    bool was_synced = atomic_load(&app->stream_synced);
    if (!was_synced) {
        if (s_callback_count <= 5) {
            fprintf(stderr, "[CB] Stream synced!\n");
        }
    }

    atomic_store(&app->stream_synced, true);
    atomic_fetch_add(&app->frame_count, 1);

    // Update sample rate from metadata
    if (meta.stream_info[0].srate > 0) {
        atomic_store(&app->sample_rate, meta.stream_info[0].srate);
    }

    // First pass: calculate total payload size
    size_t stream0_payload_bytes = 0;
    bool has_stream_id = (meta.flags & FLAG_STREAM_ID_PRESENT) != 0;

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
        stream0_payload_bytes += payload_len * sizeof(uint16_t);
    }

    if (stream0_payload_bytes == 0) {
        return;
    }

    // Get write pointer for exact payload size
    uint8_t *buf_out = rb_write_ptr(&s_capture_rb, stream0_payload_bytes);
    if (!buf_out) {
        // Ringbuffer full - skip this frame
        static int drop_count = 0;
        drop_count++;
        if (drop_count <= 10 || drop_count % 100 == 0) {
            fprintf(stderr, "[CB] Ringbuffer full, dropping frame (%d dropped)\n", drop_count);
        }
        return;
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

// Static extraction output buffers (page-aligned for SSE)
static int16_t *s_buf_out_a = NULL;
static int16_t *s_buf_out_b = NULL;
static bool s_extract_bufs_init = false;

// Process captured data on main thread (called each frame)
// Process multiple blocks to keep up with incoming data
void gui_process_capture_data(gui_app_t *app) {
    static int call_count = 0;
    call_count++;

    if (!s_rb_initialized) {
        if (call_count <= 3) fprintf(stderr, "[MAIN] gui_process_capture_data: rb not initialized\n");
        return;
    }
    if (!atomic_load(&app->stream_synced)) {
        if (call_count <= 3 || call_count % 300 == 0) {
            fprintf(stderr, "[MAIN] gui_process_capture_data: stream not synced (call %d)\n", call_count);
        }
        return;
    }

    // Get extraction function if not cached
    if (!s_extract_fn) {
        // Use dummy non-NULL pointers to get AB extraction
        // Parameters: single=0, pad=0, dword=0, peak_level=0 (avoid STARTP bug)
        // Note: peak_level=1 causes crash due to stack alignment issue in STARTP macro
        s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);
        fprintf(stderr, "[MAIN] Extraction function: %p\n", (void*)s_extract_fn);
    }

    // Initialize extraction output buffers once (page-aligned for SSE)
    if (!s_extract_bufs_init) {
        // Allocate 32-byte aligned buffers for SSE/AVX
        s_buf_out_a = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
        s_buf_out_b = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
        s_buf_aux = aligned_alloc(16, BUFFER_READ_SIZE);
        if (!s_buf_out_a || !s_buf_out_b || !s_buf_aux) {
            fprintf(stderr, "[MAIN] Failed to alloc extraction buffers!\n");
            return;
        }
        s_extract_bufs_init = true;
        fprintf(stderr, "[MAIN] Extraction buffers initialized (a=%p, b=%p, aux=%p)\n",
                (void*)s_buf_out_a, (void*)s_buf_out_b, (void*)s_buf_aux);
    }

    // Process multiple blocks per frame to keep up with incoming data
    int blocks_processed = 0;
    const int max_blocks_per_frame = 32;  // Limit to prevent frame stutter

    while (blocks_processed < max_blocks_per_frame) {
        // Try to read a block from ringbuffer
        size_t read_size = BUFFER_READ_SIZE * 4;  // 4 bytes per sample pair (two 16-bit samples)
        void *buf = rb_read_ptr(&s_capture_rb, read_size);
        if (!buf) {
            break;  // No more data available
        }

        // Extract samples using static buffers
        size_t clip[2] = {0, 0};
        uint16_t peak[2] = {0, 0};  // Not used since peak_level=0

        s_extract_fn((uint32_t*)buf, BUFFER_READ_SIZE, clip, s_buf_aux, s_buf_out_a, s_buf_out_b, peak);

        // Mark input buffer as consumed
        rb_read_finished(&s_capture_rb, read_size);

        // Detect positive and negative clipping separately
        // 12-bit ADC: +2047 is positive clip, -2048 is negative clip
        size_t clip_a_pos = 0, clip_a_neg = 0;
        size_t clip_b_pos = 0, clip_b_neg = 0;
        for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
            if (s_buf_out_a[i] >= 2047) clip_a_pos++;
            else if (s_buf_out_a[i] <= -2048) clip_a_neg++;
            if (s_buf_out_b[i] >= 2047) clip_b_pos++;
            else if (s_buf_out_b[i] <= -2048) clip_b_neg++;
        }
        atomic_fetch_add(&app->clip_count_a_pos, clip_a_pos);
        atomic_fetch_add(&app->clip_count_a_neg, clip_a_neg);
        atomic_fetch_add(&app->clip_count_b_pos, clip_b_pos);
        atomic_fetch_add(&app->clip_count_b_neg, clip_b_neg);

        // Calculate separate positive and negative peaks from first 1000 samples
        // For AC signals, positive and negative excursions may differ
        uint16_t peak_a_pos = 0, peak_a_neg = 0;
        uint16_t peak_b_pos = 0, peak_b_neg = 0;
        size_t peak_samples = 1000;
        for (size_t i = 0; i < peak_samples; i++) {
            int16_t sa = s_buf_out_a[i];
            int16_t sb = s_buf_out_b[i];
            // Track positive peaks (how far above zero)
            if (sa > 0 && (uint16_t)sa > peak_a_pos) peak_a_pos = (uint16_t)sa;
            if (sb > 0 && (uint16_t)sb > peak_b_pos) peak_b_pos = (uint16_t)sb;
            // Track negative peaks (how far below zero, stored as positive value)
            if (sa < 0 && (uint16_t)(-sa) > peak_a_neg) peak_a_neg = (uint16_t)(-sa);
            if (sb < 0 && (uint16_t)(-sb) > peak_b_neg) peak_b_neg = (uint16_t)(-sb);
        }
        atomic_store(&app->peak_a_pos, peak_a_pos);
        atomic_store(&app->peak_a_neg, peak_a_neg);
        atomic_store(&app->peak_b_pos, peak_b_pos);
        atomic_store(&app->peak_b_neg, peak_b_neg);

        // Update display buffer with min/max decimation
        // Decimate to fit display width while preserving peaks
        const float scale = 1.0f / 2048.0f;
        const size_t target_samples = DISPLAY_BUFFER_SIZE;  // Target display width
        const size_t decimation = (BUFFER_READ_SIZE + target_samples - 1) / target_samples;
        size_t display_samples = BUFFER_READ_SIZE / decimation;
        if (display_samples > target_samples) {
            display_samples = target_samples;
        }

        for (size_t i = 0; i < display_samples; i++) {
            size_t start = i * decimation;
            size_t end = start + decimation;
            if (end > BUFFER_READ_SIZE) end = BUFFER_READ_SIZE;

            // Find min/max within this decimation window
            int16_t min_a = s_buf_out_a[start], max_a = s_buf_out_a[start];
            int16_t min_b = s_buf_out_b[start], max_b = s_buf_out_b[start];
            for (size_t j = start + 1; j < end; j++) {
                if (s_buf_out_a[j] < min_a) min_a = s_buf_out_a[j];
                if (s_buf_out_a[j] > max_a) max_a = s_buf_out_a[j];
                if (s_buf_out_b[j] < min_b) min_b = s_buf_out_b[j];
                if (s_buf_out_b[j] > max_b) max_b = s_buf_out_b[j];
            }

            app->display_samples[i].min_a = (float)min_a * scale;
            app->display_samples[i].max_a = (float)max_a * scale;
            app->display_samples[i].min_b = (float)min_b * scale;
            app->display_samples[i].max_b = (float)max_b * scale;
        }
        app->display_samples_available = display_samples;


        atomic_fetch_add(&app->total_samples, BUFFER_READ_SIZE);
        blocks_processed++;
    }
}

// Initialize application
void gui_app_init(gui_app_t *app) {
    memset(app->display_samples, 0, sizeof(app->display_samples));
    app->display_write_pos = 0;
    app->display_samples_available = 0;

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

    if (s_buf_aux) {
        aligned_free(s_buf_aux);
        s_buf_aux = NULL;
    }
    if (s_buf_out_a) {
        aligned_free(s_buf_out_a);
        s_buf_out_a = NULL;
    }
    if (s_buf_out_b) {
        aligned_free(s_buf_out_b);
        s_buf_out_b = NULL;
    }
    s_extract_bufs_init = false;
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
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, 0);

    // Reset display buffer
    app->display_write_pos = 0;
    app->display_samples_available = 0;

    // Reset callback counter
    s_callback_count = 0;

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
        hsdaoh_close(app->hs_dev);
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

    if (app->hs_dev) {
        hsdaoh_stop_stream(app->hs_dev);
        hsdaoh_close(app->hs_dev);
        app->hs_dev = NULL;
    }

    app->is_capturing = false;
    atomic_store(&app->stream_synced, false);
    gui_app_set_status(app, "Capture stopped");
}

// Start recording
int gui_app_start_recording(gui_app_t *app) {
    if (!app->is_capturing) {
        gui_app_set_status(app, "Start capture first");
        return -1;
    }

    if (app->is_recording) {
        return 0;
    }

    // TODO: Implement file recording
    app->is_recording = true;
    app->recording_start_time = GetTime();
    atomic_store(&app->recording_bytes, 0);

    gui_app_set_status(app, "Recording...");
    return 0;
}

// Stop recording
void gui_app_stop_recording(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }

    // TODO: Close file writers
    app->is_recording = false;
    gui_app_set_status(app, "Recording stopped");
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
void gui_app_update_display_buffer(gui_app_t *app) {
    // Process any available capture data
    gui_process_capture_data(app);
}

// Set status message
void gui_app_set_status(gui_app_t *app, const char *message) {
    strncpy(app->status_message, message, sizeof(app->status_message) - 1);
    app->status_message[sizeof(app->status_message) - 1] = '\0';
    app->status_message_time = GetTime();
}
