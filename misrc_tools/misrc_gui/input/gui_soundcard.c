/*
 * MISRC GUI - Soundcard Capture (WASAPI)
 *
 * Captures audio from the computer's soundcard for VHS linear audio recording.
 * Uses Windows Audio Session API (WASAPI) for low-latency capture.
 *
 * Copyright (C) 2024-2025 MISRC Authors
 * License: GPL-3.0-or-later
 */

#ifdef _WIN32

/*
 * Header conflict resolution:
 *
 * WASAPI requires full Windows headers, but gui_app.h includes raylib which
 * conflicts with Windows headers (Rectangle, CloseWindow, etc.).
 *
 * Solution: Include Windows headers FIRST, then include gui_app.h.
 * We prevent threading.h from being included (it has conflicting minimal
 * Windows declarations) and use Windows threading API directly.
 */

#define COBJMACROS
#include <windows.h>
#include <process.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// Prevent threading.h from being included - it has declarations that
// conflict with the full Windows headers we already included
#define MISRC_THREADING_H

// Now we can safely include gui_app.h. The raylib types will conflict with
// Windows types, but since we don't CALL raylib functions here, we just
// need to suppress the redefinition errors.
#include "gui_soundcard.h"
#include "../../common/buffer_manager.h"
#include "../processing/gui_soundcard_display_thread.h"

// Forward declare gui_app struct and the fields we need to access
// We cannot include gui_app.h due to raylib/Windows conflicts
typedef struct gui_app gui_app_t;

// Static display thread instance
static soundcard_display_thread_t s_display_thread;

// Recording enabled flag - set by gui_soundcard_writer when recording starts/stops
static atomic_bool s_soundcard_recording_enabled = false;

// Accessor functions - defined in gui_capture.c which can include gui_app.h
extern buffer_manager_t* gui_soundcard_get_bufmgr(gui_app_t *app);
extern int gui_soundcard_get_device_index(gui_app_t *app);
extern void gui_soundcard_set_ctx(gui_app_t *app, void *ctx);
extern void* gui_soundcard_get_ctx(gui_app_t *app);
extern void gui_soundcard_set_running(gui_app_t *app, bool running);
extern bool gui_soundcard_get_running(gui_app_t *app);
extern void gui_soundcard_set_peaks(gui_app_t *app, uint16_t peak_l, uint16_t peak_r);
extern void gui_soundcard_reset_peaks(gui_app_t *app);

// Soundcard capture parameters
#define SOUNDCARD_SAMPLE_RATE     48000
#define SOUNDCARD_CHANNELS        2
#define SOUNDCARD_BITS_PER_SAMPLE 16
#define SOUNDCARD_BUFFER_DURATION_MS 20  // 20ms buffer for low latency

// Soundcard capture context
typedef struct {
    IMMDevice *device;
    IAudioClient *audio_client;
    IAudioCaptureClient *capture_client;
    HANDLE event;
    HANDLE thread;
    atomic_bool running;
    buffer_manager_t *bufmgr;
    gui_app_t *app;
    WAVEFORMATEX *wave_format;
} soundcard_ctx_t;

// Static storage for device enumeration
static soundcard_device_info_t s_soundcard_devices[MAX_SOUNDCARD_DEVICES];
static int s_soundcard_device_count = 0;

// Helper: Convert wide string to UTF-8
static void wide_to_utf8(const WCHAR *wide, char *utf8, size_t utf8_size) {
    if (!wide || !utf8 || utf8_size == 0) return;
    int result = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)utf8_size, NULL, NULL);
    if (result == 0) {
        utf8[0] = '\0';
    }
}

// Note: VU meter peak calculation has been moved to the soundcard display thread
// (gui_soundcard_display_thread.c) which reads from BUF_SOUNDCARD_AUDIO.

// Soundcard capture thread (Windows thread signature)
static unsigned __stdcall soundcard_capture_thread(void *ctx) {
    soundcard_ctx_t *sc = (soundcard_ctx_t *)ctx;
    HRESULT hr;
    uint64_t total_bytes_captured = 0;
    uint32_t debug_counter = 0;

    fprintf(stderr, "[SOUNDCARD] Capture thread started\n");
    fprintf(stderr, "[SOUNDCARD] Format: %d channels, %d Hz, %d bits\n",
            sc->wave_format->nChannels, (int)sc->wave_format->nSamplesPerSec,
            sc->wave_format->wBitsPerSample);

    // Start the audio client
    hr = IAudioClient_Start(sc->audio_client);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to start audio client: 0x%08lx\n", hr);
        return -1;
    }

    while (atomic_load(&sc->running)) {
        // Wait for audio data event (or timeout)
        DWORD wait_result = WaitForSingleObject(sc->event, 100);
        if (wait_result == WAIT_TIMEOUT) {
            continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "[SOUNDCARD] Wait failed: %lu\n", wait_result);
            break;
        }

        BYTE *data = NULL;
        UINT32 frames_available = 0;
        DWORD flags = 0;

        // Get captured audio buffer
        hr = IAudioCaptureClient_GetBuffer(sc->capture_client, &data, &frames_available, &flags, NULL, NULL);
        if (FAILED(hr)) {
            if (hr != AUDCLNT_S_BUFFER_EMPTY) {
                fprintf(stderr, "[SOUNDCARD] GetBuffer failed: 0x%08lx\n", hr);
            }
            continue;
        }

        if (frames_available > 0 && data) {
            size_t bytes = frames_available * sc->wave_format->nBlockAlign;
            const void *src_data = data;

            // Handle silence flag - data may be NULL or garbage
            // We use a static zero buffer for silence
            static uint8_t silence_buf[8192];  // Large enough for typical audio chunks
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                src_data = silence_buf;
                if (bytes > sizeof(silence_buf)) {
                    bytes = sizeof(silence_buf);  // Clamp to avoid overflow
                }
            }

            // Write to display buffer (lossy - drops silently if full)
            // The display thread will read from here and calculate VU meter peaks
            void *buf = bufmgr_write_begin(sc->bufmgr, BUF_SOUNDCARD_AUDIO, bytes, NULL);
            if (buf) {
                memcpy(buf, src_data, bytes);
                bufmgr_write_end(sc->bufmgr, BUF_SOUNDCARD_AUDIO, bytes);
            }

            // Write to record buffer only when recording is active
            // This prevents buffer filling up when not recording
            if (atomic_load(&s_soundcard_recording_enabled)) {
                buf = bufmgr_write_begin(sc->bufmgr, BUF_SOUNDCARD_RECORD, bytes, NULL);
                if (buf) {
                    memcpy(buf, src_data, bytes);
                    bufmgr_write_end(sc->bufmgr, BUF_SOUNDCARD_RECORD, bytes);
                    total_bytes_captured += bytes;
                    debug_counter++;
                    if (debug_counter <= 5 || (debug_counter % 500) == 0) {
                        fprintf(stderr, "[SOUNDCARD] Captured %u frames (%zu bytes), total: %llu bytes\n",
                                frames_available, bytes, (unsigned long long)total_bytes_captured);
                    }
                }
            }
        }

        // Release buffer
        hr = IAudioCaptureClient_ReleaseBuffer(sc->capture_client, frames_available);
        if (FAILED(hr)) {
            fprintf(stderr, "[SOUNDCARD] ReleaseBuffer failed: 0x%08lx\n", hr);
        }
    }

    // Stop the audio client
    IAudioClient_Stop(sc->audio_client);

    fprintf(stderr, "[SOUNDCARD] Capture thread exiting\n");
    return 0;
}

int gui_soundcard_enumerate_devices(soundcard_device_info_t *devices, int max) {
    if (!devices || max <= 0) return -1;

    HRESULT hr;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection *collection = NULL;
    IMMDevice *default_device = NULL;
    LPWSTR default_id = NULL;
    int count = 0;

    // Initialize COM for this thread
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[SOUNDCARD] Failed to initialize COM: 0x%08lx\n", hr);
        return -1;
    }

    // Create device enumerator
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to create device enumerator: 0x%08lx\n", hr);
        goto cleanup;
    }

    // Get default capture device ID
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eConsole, &default_device);
    if (SUCCEEDED(hr) && default_device) {
        IMMDevice_GetId(default_device, &default_id);
        IMMDevice_Release(default_device);
    }

    // Enumerate capture devices
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to enumerate devices: 0x%08lx\n", hr);
        goto cleanup;
    }

    UINT device_count = 0;
    IMMDeviceCollection_GetCount(collection, &device_count);

    for (UINT i = 0; i < device_count && count < max; i++) {
        IMMDevice *device = NULL;
        hr = IMMDeviceCollection_Item(collection, i, &device);
        if (FAILED(hr)) continue;

        LPWSTR device_id = NULL;
        hr = IMMDevice_GetId(device, &device_id);
        if (SUCCEEDED(hr) && device_id) {
            wide_to_utf8(device_id, devices[count].device_id, sizeof(devices[count].device_id));

            // Check if this is the default device
            devices[count].is_default = (default_id && wcscmp(device_id, default_id) == 0);

            // Get device name from properties
            IPropertyStore *props = NULL;
            hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &props);
            if (SUCCEEDED(hr)) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                hr = IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &varName);
                if (SUCCEEDED(hr)) {
                    wide_to_utf8(varName.pwszVal, devices[count].name, sizeof(devices[count].name));
                    PropVariantClear(&varName);
                }
                IPropertyStore_Release(props);
            }

            if (devices[count].name[0] == '\0') {
                snprintf(devices[count].name, sizeof(devices[count].name), "Audio Device %d", count + 1);
            }

            CoTaskMemFree(device_id);
            count++;
        }

        IMMDevice_Release(device);
    }

cleanup:
    if (default_id) CoTaskMemFree(default_id);
    if (collection) IMMDeviceCollection_Release(collection);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);

    // Store for later use
    s_soundcard_device_count = count;
    if (count > 0) {
        memcpy(s_soundcard_devices, devices, count * sizeof(soundcard_device_info_t));
    }

    return count;
}

int gui_soundcard_start(gui_app_t *app) {
    if (!app) return -1;

    if (gui_soundcard_get_running(app)) {
        fprintf(stderr, "[SOUNDCARD] Already running\n");
        return 0;
    }

    fprintf(stderr, "[SOUNDCARD] Starting soundcard capture\n");

    HRESULT hr;
    soundcard_ctx_t *ctx = calloc(1, sizeof(soundcard_ctx_t));
    if (!ctx) {
        fprintf(stderr, "[SOUNDCARD] Failed to allocate context\n");
        return -1;
    }

    ctx->app = app;
    ctx->bufmgr = gui_soundcard_get_bufmgr(app);

    // Initialize COM for this thread
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[SOUNDCARD] Failed to initialize COM: 0x%08lx\n", hr);
        free(ctx);
        return -1;
    }

    // Ensure both soundcard buffers are initialized
    // BUF_SOUNDCARD_AUDIO: for display thread (VU meter)
    // BUF_SOUNDCARD_RECORD: for recording thread (FLAC writer)
    if (bufmgr_ensure_init(ctx->bufmgr, BUF_SOUNDCARD_AUDIO) < 0) {
        fprintf(stderr, "[SOUNDCARD] Failed to initialize soundcard audio buffer\n");
        free(ctx);
        return -1;
    }
    if (bufmgr_ensure_init(ctx->bufmgr, BUF_SOUNDCARD_RECORD) < 0) {
        fprintf(stderr, "[SOUNDCARD] Failed to initialize soundcard record buffer\n");
        free(ctx);
        return -1;
    }

    // Create device enumerator
    IMMDeviceEnumerator *enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to create device enumerator: 0x%08lx\n", hr);
        free(ctx);
        return -1;
    }

    // Get the selected device (or default)
    int device_index = gui_soundcard_get_device_index(app);
    if (device_index >= 0 && device_index < s_soundcard_device_count) {
        // Use specific device
        WCHAR device_id_wide[256];
        MultiByteToWideChar(CP_UTF8, 0,
                           s_soundcard_devices[device_index].device_id,
                           -1, device_id_wide, 256);
        hr = IMMDeviceEnumerator_GetDevice(enumerator, device_id_wide, &ctx->device);
    } else {
        // Use default device
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eConsole, &ctx->device);
    }
    IMMDeviceEnumerator_Release(enumerator);

    if (FAILED(hr) || !ctx->device) {
        fprintf(stderr, "[SOUNDCARD] Failed to get capture device: 0x%08lx\n", hr);
        free(ctx);
        return -1;
    }

    // Activate audio client
    hr = IMMDevice_Activate(ctx->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ctx->audio_client);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to activate audio client: 0x%08lx\n", hr);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Get the mix format
    hr = IAudioClient_GetMixFormat(ctx->audio_client, &ctx->wave_format);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to get mix format: 0x%08lx\n", hr);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Try to use our preferred format (48kHz, 16-bit, stereo)
    WAVEFORMATEX desired_format = {0};
    desired_format.wFormatTag = WAVE_FORMAT_PCM;
    desired_format.nChannels = SOUNDCARD_CHANNELS;
    desired_format.nSamplesPerSec = SOUNDCARD_SAMPLE_RATE;
    desired_format.wBitsPerSample = SOUNDCARD_BITS_PER_SAMPLE;
    desired_format.nBlockAlign = desired_format.nChannels * desired_format.wBitsPerSample / 8;
    desired_format.nAvgBytesPerSec = desired_format.nSamplesPerSec * desired_format.nBlockAlign;
    desired_format.cbSize = 0;

    WAVEFORMATEX *closest_format = NULL;
    hr = IAudioClient_IsFormatSupported(ctx->audio_client, AUDCLNT_SHAREMODE_SHARED,
                                         &desired_format, &closest_format);

    WAVEFORMATEX *use_format = NULL;
    if (hr == S_OK) {
        // Our format is supported exactly
        CoTaskMemFree(ctx->wave_format);
        ctx->wave_format = malloc(sizeof(WAVEFORMATEX));
        if (ctx->wave_format) {
            memcpy(ctx->wave_format, &desired_format, sizeof(WAVEFORMATEX));
        }
        use_format = ctx->wave_format;
        fprintf(stderr, "[SOUNDCARD] Using preferred format: %u Hz, %u-bit, %u channels\n",
                SOUNDCARD_SAMPLE_RATE, SOUNDCARD_BITS_PER_SAMPLE, SOUNDCARD_CHANNELS);
    } else if (hr == S_FALSE && closest_format) {
        // Use closest supported format
        CoTaskMemFree(ctx->wave_format);
        ctx->wave_format = closest_format;
        use_format = closest_format;
        fprintf(stderr, "[SOUNDCARD] Using closest format: %lu Hz, %u-bit, %u channels\n",
                closest_format->nSamplesPerSec, closest_format->wBitsPerSample, closest_format->nChannels);
    } else {
        // Fall back to device's mix format
        use_format = ctx->wave_format;
        fprintf(stderr, "[SOUNDCARD] Using mix format: %lu Hz, %u-bit, %u channels\n",
                ctx->wave_format->nSamplesPerSec, ctx->wave_format->wBitsPerSample, ctx->wave_format->nChannels);
    }

    // Create event for buffer notifications
    ctx->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ctx->event) {
        fprintf(stderr, "[SOUNDCARD] Failed to create event\n");
        CoTaskMemFree(ctx->wave_format);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Initialize audio client in shared mode with event-driven buffering
    REFERENCE_TIME buffer_duration = SOUNDCARD_BUFFER_DURATION_MS * 10000;  // 100ns units
    hr = IAudioClient_Initialize(ctx->audio_client, AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  buffer_duration, 0, use_format, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to initialize audio client: 0x%08lx\n", hr);
        CloseHandle(ctx->event);
        CoTaskMemFree(ctx->wave_format);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Set event handle
    hr = IAudioClient_SetEventHandle(ctx->audio_client, ctx->event);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to set event handle: 0x%08lx\n", hr);
        CloseHandle(ctx->event);
        CoTaskMemFree(ctx->wave_format);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Get capture client
    hr = IAudioClient_GetService(ctx->audio_client, &IID_IAudioCaptureClient, (void **)&ctx->capture_client);
    if (FAILED(hr)) {
        fprintf(stderr, "[SOUNDCARD] Failed to get capture client: 0x%08lx\n", hr);
        CloseHandle(ctx->event);
        CoTaskMemFree(ctx->wave_format);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Reset buffers and peaks
    bufmgr_reset(ctx->bufmgr, BUF_SOUNDCARD_AUDIO);
    bufmgr_reset(ctx->bufmgr, BUF_SOUNDCARD_RECORD);
    gui_soundcard_reset_peaks(app);

    // Initialize and start display thread (null consumer for VU meter)
    gui_soundcard_display_thread_init(&s_display_thread);
    if (gui_soundcard_display_thread_start(&s_display_thread, app, ctx->bufmgr) < 0) {
        fprintf(stderr, "[SOUNDCARD] Failed to start display thread\n");
        gui_soundcard_display_thread_cleanup(&s_display_thread);
        IAudioCaptureClient_Release(ctx->capture_client);
        CloseHandle(ctx->event);
        CoTaskMemFree(ctx->wave_format);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    // Start capture thread
    atomic_store(&ctx->running, true);
    ctx->thread = (HANDLE)_beginthreadex(NULL, 0, soundcard_capture_thread, ctx, 0, NULL);
    if (!ctx->thread) {
        fprintf(stderr, "[SOUNDCARD] Failed to create capture thread\n");
        gui_soundcard_display_thread_stop(&s_display_thread);
        gui_soundcard_display_thread_cleanup(&s_display_thread);
        IAudioCaptureClient_Release(ctx->capture_client);
        CloseHandle(ctx->event);
        CoTaskMemFree(ctx->wave_format);
        IAudioClient_Release(ctx->audio_client);
        IMMDevice_Release(ctx->device);
        free(ctx);
        return -1;
    }

    gui_soundcard_set_ctx(app, ctx);
    gui_soundcard_set_running(app, true);

    fprintf(stderr, "[SOUNDCARD] Capture started successfully\n");
    return 0;
}

void gui_soundcard_stop(gui_app_t *app) {
    if (!app || !gui_soundcard_get_running(app)) return;

    fprintf(stderr, "[SOUNDCARD] Stopping soundcard capture\n");

    soundcard_ctx_t *ctx = (soundcard_ctx_t *)gui_soundcard_get_ctx(app);
    if (!ctx) return;

    // Signal capture thread to stop
    atomic_store(&ctx->running, false);

    // Wake up the capture thread if it's waiting
    if (ctx->event) {
        SetEvent(ctx->event);
    }

    // Wait for capture thread to finish
    WaitForSingleObject(ctx->thread, INFINITE);
    CloseHandle(ctx->thread);

    // Stop and cleanup display thread (must be after capture thread stops)
    gui_soundcard_display_thread_stop(&s_display_thread);
    gui_soundcard_display_thread_cleanup(&s_display_thread);

    // Cleanup WASAPI resources
    if (ctx->capture_client) IAudioCaptureClient_Release(ctx->capture_client);
    if (ctx->event) CloseHandle(ctx->event);
    if (ctx->wave_format) CoTaskMemFree(ctx->wave_format);
    if (ctx->audio_client) IAudioClient_Release(ctx->audio_client);
    if (ctx->device) IMMDevice_Release(ctx->device);
    free(ctx);

    gui_soundcard_set_ctx(app, NULL);
    gui_soundcard_set_running(app, false);
    gui_soundcard_reset_peaks(app);

    fprintf(stderr, "[SOUNDCARD] Capture stopped\n");
}

bool gui_soundcard_is_running(gui_app_t *app) {
    if (!app) return false;
    return gui_soundcard_get_running(app);
}

void gui_soundcard_set_recording(bool enabled) {
    atomic_store(&s_soundcard_recording_enabled, enabled);
    fprintf(stderr, "[SOUNDCARD] Recording %s\n", enabled ? "enabled" : "disabled");
}

#else // !_WIN32

// Stub implementations for non-Windows platforms

#include "gui_soundcard.h"
#include "../core/gui_app.h"
#include <stdio.h>

int gui_soundcard_enumerate_devices(soundcard_device_info_t *devices, int max) {
    (void)devices;
    (void)max;
    fprintf(stderr, "[SOUNDCARD] Not supported on this platform\n");
    return 0;
}

int gui_soundcard_start(gui_app_t *app) {
    (void)app;
    fprintf(stderr, "[SOUNDCARD] Not supported on this platform\n");
    return -1;
}

void gui_soundcard_stop(gui_app_t *app) {
    (void)app;
}

bool gui_soundcard_is_running(gui_app_t *app) {
    (void)app;
    return false;
}

void gui_soundcard_set_recording(bool enabled) {
    (void)enabled;
}

#endif // _WIN32
