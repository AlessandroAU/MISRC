/*
 * MISRC GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses a dedicated extraction thread to ensure recording is not affected
 * by UI thread blocking (e.g., window dragging).
 */

#include "gui_record.h"
#include "gui_app.h"
#include "gui_extract.h"

#include "../ringbuffer.h"
#include "../extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <malloc.h>
#define aligned_alloc(align, size) _aligned_malloc(size, align)
#define aligned_free(ptr) _aligned_free(ptr)
#else
#define aligned_free(ptr) free(ptr)
#endif

// FLAC support
#if LIBFLAC_ENABLED == 1
#include "FLAC/stream_encoder.h"
#endif

// Buffer sizes
#define BUFFER_READ_SIZE 65536
#define BUFFER_RECORD_SIZE (65536 * 1024)  // 64MB per channel

// Threading primitives (avoid windows.h conflicts with raylib)
#ifdef _WIN32
  #include <process.h>
  typedef void* thrd_t;
  typedef unsigned (__stdcall *thrd_start_t)(void*);
  #define thrd_success 0
  #define thrd_create(a,b,c) (((*(a)=(thrd_t)_beginthreadex(NULL,0,(thrd_start_t)b,c,0,NULL))==0)?-1:thrd_success)
  #ifndef INFINITE
    #define INFINITE 0xFFFFFFFF
  #endif
  #ifndef WAIT_OBJECT_0
    #define WAIT_OBJECT_0 0
  #endif
  static inline int thrd_join_impl(thrd_t t, int *res) {
    extern __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
    extern __declspec(dllimport) int __stdcall GetExitCodeThread(void*, unsigned long*);
    extern __declspec(dllimport) int __stdcall CloseHandle(void*);
    unsigned long exitcode = 0;
    if (WaitForSingleObject(t, INFINITE) != WAIT_OBJECT_0) return -1;
    GetExitCodeThread(t, &exitcode);
    if (res) *res = (int)exitcode;
    CloseHandle(t);
    return thrd_success;
  }
  #define thrd_join(a,b) thrd_join_impl(a,b)
  static inline void thrd_sleep_ms(int ms) {
    extern __declspec(dllimport) void __stdcall Sleep(unsigned long);
    Sleep((unsigned long)ms);
  }
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t thrd_t;
  #define thrd_success 0
  #define thrd_create(a,b,c) pthread_create(a,NULL,(void* (*)(void *))b,c)
  #define thrd_join(a,b) pthread_join(a,b)
  static inline void thrd_sleep_ms(int ms) {
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
  }
#endif

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Recording ringbuffers (extracted samples -> file writers)
static ringbuffer_t s_record_rb_a;
static ringbuffer_t s_record_rb_b;
static bool s_record_rb_initialized = false;

// Extraction thread (reads from capture ringbuffer, writes to record ringbuffers)
static thrd_t s_extract_thread;
static bool s_extract_thread_running = false;
static ringbuffer_t *s_capture_rb = NULL;  // Pointer to capture ringbuffer

// Writer threads
static thrd_t s_writer_thread_a;
static thrd_t s_writer_thread_b;
static bool s_writer_threads_running = false;
static FILE *s_file_a = NULL;
static FILE *s_file_b = NULL;

#if LIBFLAC_ENABLED == 1
static FLAC__StreamEncoder *s_encoder_a = NULL;
static FLAC__StreamEncoder *s_encoder_b = NULL;
#endif

// Global app pointer for threads
static gui_app_t *s_recording_app = NULL;

// File writer context
typedef struct {
    ringbuffer_t *rb;
    FILE *file;
    int channel;  // 0 = A, 1 = B
#if LIBFLAC_ENABLED == 1
    FLAC__StreamEncoder *encoder;
    atomic_uint_fast64_t *compressed_bytes;
#endif
} writer_ctx_t;

static writer_ctx_t s_ctx_a;
static writer_ctx_t s_ctx_b;

// Note: Extraction buffers are provided by gui_extract module

#if LIBFLAC_ENABLED == 1
// FLAC write callback to track compressed output size
static FLAC__StreamEncoderWriteStatus flac_write_callback(
    const FLAC__StreamEncoder *encoder,
    const FLAC__byte buffer[],
    size_t bytes,
    uint32_t samples,
    uint32_t current_frame,
    void *client_data)
{
    (void)encoder; (void)samples; (void)current_frame;
    writer_ctx_t *wctx = (writer_ctx_t *)client_data;

    size_t written = fwrite(buffer, 1, bytes, wctx->file);
    if (written != bytes) {
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }

    // Track compressed bytes
    if (wctx->compressed_bytes) {
        atomic_fetch_add(wctx->compressed_bytes, bytes);
    }

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// FLAC file writer thread
static int flac_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = BUFFER_READ_SIZE * sizeof(int32_t);
    size_t raw_bytes_per_block = BUFFER_READ_SIZE * sizeof(int16_t);
    void *buf;

    fprintf(stderr, "[FLAC] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (!atomic_load(&do_exit) && s_recording_app && s_recording_app->is_recording) {
        buf = rb_read_ptr(wctx->rb, len);
        if (!buf) {
            thrd_sleep_ms(1);
            continue;
        }

        FLAC__bool ok = FLAC__stream_encoder_process(
            wctx->encoder, (const FLAC__int32 **)&buf, BUFFER_READ_SIZE);
        if (!ok) {
            fprintf(stderr, "FLAC encoder error on channel %c\n", wctx->channel == 0 ? 'A' : 'B');
        }

        rb_read_finished(wctx->rb, len);

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, raw_bytes_per_block);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, raw_bytes_per_block);
            }
        }
    }

    fprintf(stderr, "[FLAC] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}
#endif

// RAW file writer thread
static int raw_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = BUFFER_READ_SIZE * sizeof(int16_t);

    fprintf(stderr, "[RAW] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    while (!atomic_load(&do_exit) && s_recording_app && s_recording_app->is_recording) {
        void *buf = rb_read_ptr(wctx->rb, len);
        if (!buf) {
            thrd_sleep_ms(1);
            continue;
        }

        size_t written = fwrite(buf, 1, len, wctx->file);
        rb_read_finished(wctx->rb, len);

        if (s_recording_app) {
            atomic_fetch_add(&s_recording_app->recording_bytes, written);
            if (wctx->channel == 0) {
                atomic_fetch_add(&s_recording_app->recording_raw_a, written);
            } else {
                atomic_fetch_add(&s_recording_app->recording_raw_b, written);
            }
        }
    }

    fprintf(stderr, "[RAW] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}

// Extraction thread - reads from capture ringbuffer, extracts, writes to record ringbuffers
// This thread runs independently of the UI and won't be blocked by window dragging
// Also updates display buffer and peak/clip values for UI
static int extraction_thread(void *ctx) {
    (void)ctx;
    size_t read_size = BUFFER_READ_SIZE * 4;  // 4 bytes per sample pair
    size_t clip[2] = {0, 0};
    uint16_t peak[2] = {0, 0};

    // Get extraction resources from shared module
    extract_fn_t extract_fn = gui_extract_get_function();
    int16_t *buf_a = gui_extract_get_buf_a();
    int16_t *buf_b = gui_extract_get_buf_b();
    uint8_t *buf_aux = gui_extract_get_buf_aux();

    fprintf(stderr, "[EXTRACT] Extraction thread started\n");

    while (!atomic_load(&do_exit) && s_recording_app && s_recording_app->is_recording) {
        void *buf = rb_read_ptr(s_capture_rb, read_size);
        if (!buf) {
            thrd_sleep_ms(1);
            continue;
        }

        // Extract samples
        extract_fn((uint32_t*)buf, BUFFER_READ_SIZE, clip, buf_aux, buf_a, buf_b, peak);

        // Mark capture buffer as consumed
        rb_read_finished(s_capture_rb, read_size);

        // Update stats and display using shared functions
        gui_extract_update_stats(s_recording_app, buf_a, buf_b, BUFFER_READ_SIZE);
        gui_extract_update_display(s_recording_app, buf_a, buf_b, BUFFER_READ_SIZE);

        atomic_fetch_add(&s_recording_app->total_samples, BUFFER_READ_SIZE);

#if LIBFLAC_ENABLED == 1
        if (s_recording_app->settings.use_flac) {
            // FLAC needs 32-bit samples with 12-bit to 16-bit extension
            size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int32_t);
            int32_t *write_a = (int32_t *)rb_write_ptr(&s_record_rb_a, sample_bytes);
            int32_t *write_b = (int32_t *)rb_write_ptr(&s_record_rb_b, sample_bytes);

            if (write_a && write_b) {
                // Convert 12-bit samples to 16-bit by left-shifting 4 bits
                for (size_t i = 0; i < BUFFER_READ_SIZE; i++) {
                    write_a[i] = (int32_t)buf_a[i] << 4;
                    write_b[i] = (int32_t)buf_b[i] << 4;
                }
                rb_write_finished(&s_record_rb_a, sample_bytes);
                rb_write_finished(&s_record_rb_b, sample_bytes);
            } else {
                static int record_drop_count = 0;
                record_drop_count++;
                if (record_drop_count <= 5 || record_drop_count % 100 == 0) {
                    fprintf(stderr, "[EXTRACT] Record ringbuffer full, dropped %d blocks\n", record_drop_count);
                }
            }
        } else
#endif
        {
            // RAW uses 16-bit samples directly
            size_t sample_bytes = BUFFER_READ_SIZE * sizeof(int16_t);
            void *write_a = rb_write_ptr(&s_record_rb_a, sample_bytes);
            void *write_b = rb_write_ptr(&s_record_rb_b, sample_bytes);

            if (write_a && write_b) {
                memcpy(write_a, buf_a, sample_bytes);
                memcpy(write_b, buf_b, sample_bytes);
                rb_write_finished(&s_record_rb_a, sample_bytes);
                rb_write_finished(&s_record_rb_b, sample_bytes);
            } else {
                static int record_drop_count = 0;
                record_drop_count++;
                if (record_drop_count <= 5 || record_drop_count % 100 == 0) {
                    fprintf(stderr, "[EXTRACT] Record ringbuffer full, dropped %d blocks\n", record_drop_count);
                }
            }
        }
    }

    fprintf(stderr, "[EXTRACT] Extraction thread exiting\n");
    return 0;
}

// Initialize recording subsystem
void gui_record_init(void) {
    if (!s_record_rb_initialized) {
        rb_init(&s_record_rb_a, "record_a_rb", BUFFER_RECORD_SIZE);
        rb_init(&s_record_rb_b, "record_b_rb", BUFFER_RECORD_SIZE);
        s_record_rb_initialized = true;
    }

    // Initialize shared extraction module
    gui_extract_init();
}

// Cleanup recording subsystem
void gui_record_cleanup(void) {
    if (s_record_rb_initialized) {
        rb_close(&s_record_rb_a);
        rb_close(&s_record_rb_b);
        s_record_rb_initialized = false;
    }

    // Note: gui_extract_cleanup is called by gui_capture.c
}

// Check if recording is active
bool gui_record_is_active(void) {
    return s_recording_app != NULL && s_recording_app->is_recording;
}

// Start recording
int gui_record_start(gui_app_t *app, ringbuffer_t *capture_rb) {
    if (!app->is_capturing) {
        gui_app_set_status(app, "Start capture first");
        return -1;
    }

    if (app->is_recording) {
        return 0;
    }

    // Initialize subsystem
    gui_record_init();

    // Store capture ringbuffer pointer
    s_capture_rb = capture_rb;

    // Reset record ringbuffers
    atomic_store(&s_record_rb_a.head, 0);
    atomic_store(&s_record_rb_a.tail, 0);
    atomic_store(&s_record_rb_b.head, 0);
    atomic_store(&s_record_rb_b.tail, 0);

    s_recording_app = app;
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac) {
        // Open FLAC files
        s_file_a = fopen(app->settings.output_filename_a, "wb");
        s_file_b = fopen(app->settings.output_filename_b, "wb");

        if (!s_file_a || !s_file_b) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }

        // Create FLAC encoders
        s_encoder_a = FLAC__stream_encoder_new();
        s_encoder_b = FLAC__stream_encoder_new();

        if (!s_encoder_a || !s_encoder_b) {
            gui_app_set_status(app, "Failed to create FLAC encoders");
            fclose(s_file_a); fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }

        // Setup writer contexts
        s_ctx_a.rb = &s_record_rb_a;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;
        s_ctx_a.encoder = s_encoder_a;
        s_ctx_a.compressed_bytes = &app->recording_compressed_a;

        s_ctx_b.rb = &s_record_rb_b;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;
        s_ctx_b.encoder = s_encoder_b;
        s_ctx_b.compressed_bytes = &app->recording_compressed_b;

        // Sample rate for FLAC - 40kHz
        uint32_t srate = 40000;

        // Configure and initialize encoders
        writer_ctx_t *contexts[2] = { &s_ctx_a, &s_ctx_b };
        FLAC__StreamEncoder *encoders[2] = { s_encoder_a, s_encoder_b };

        for (int i = 0; i < 2; i++) {
            FLAC__StreamEncoder *enc = encoders[i];
            writer_ctx_t *wctx = contexts[i];

            FLAC__stream_encoder_set_verify(enc, false);
            FLAC__stream_encoder_set_compression_level(enc, app->settings.flac_level);
            FLAC__stream_encoder_set_channels(enc, 1);
            FLAC__stream_encoder_set_bits_per_sample(enc, 16);
            FLAC__stream_encoder_set_sample_rate(enc, srate);
            FLAC__stream_encoder_set_total_samples_estimate(enc, 0);

            FLAC__StreamEncoderInitStatus status =
                FLAC__stream_encoder_init_stream(enc, flac_write_callback, NULL, NULL, NULL, wctx);
            if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
                fprintf(stderr, "FLAC encoder init failed: %d\n", status);
                gui_app_set_status(app, "FLAC encoder init failed");
                FLAC__stream_encoder_delete(s_encoder_a);
                FLAC__stream_encoder_delete(s_encoder_b);
                s_encoder_a = s_encoder_b = NULL;
                fclose(s_file_a); fclose(s_file_b);
                s_file_a = s_file_b = NULL;
                return -1;
            }
        }

        // Start threads
        app->is_recording = true;
        app->recording_start_time = GetTime();

        thrd_create(&s_extract_thread, extraction_thread, NULL);
        s_extract_thread_running = true;

        thrd_create(&s_writer_thread_a, flac_writer_thread, &s_ctx_a);
        thrd_create(&s_writer_thread_b, flac_writer_thread, &s_ctx_b);
        s_writer_threads_running = true;

        gui_app_set_status(app, "Recording (FLAC)...");
    } else
#endif
    {
        // RAW recording
        s_file_a = fopen(app->settings.output_filename_a, "wb");
        s_file_b = fopen(app->settings.output_filename_b, "wb");

        if (!s_file_a || !s_file_b) {
            gui_app_set_status(app, "Failed to open output files");
            if (s_file_a) fclose(s_file_a);
            if (s_file_b) fclose(s_file_b);
            s_file_a = s_file_b = NULL;
            return -1;
        }

        s_ctx_a.rb = &s_record_rb_a;
        s_ctx_a.file = s_file_a;
        s_ctx_a.channel = 0;

        s_ctx_b.rb = &s_record_rb_b;
        s_ctx_b.file = s_file_b;
        s_ctx_b.channel = 1;

        app->is_recording = true;
        app->recording_start_time = GetTime();

        thrd_create(&s_extract_thread, extraction_thread, NULL);
        s_extract_thread_running = true;

        thrd_create(&s_writer_thread_a, raw_writer_thread, &s_ctx_a);
        thrd_create(&s_writer_thread_b, raw_writer_thread, &s_ctx_b);
        s_writer_threads_running = true;

        gui_app_set_status(app, "Recording (RAW)...");
    }

    return 0;
}

// Stop recording
void gui_record_stop(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }

    // Signal threads to stop
    app->is_recording = false;

    // Wait for extraction thread
    if (s_extract_thread_running) {
        thrd_join(s_extract_thread, NULL);
        s_extract_thread_running = false;
    }

    // Wait for writer threads
    if (s_writer_threads_running) {
        thrd_join(s_writer_thread_a, NULL);
        thrd_join(s_writer_thread_b, NULL);
        s_writer_threads_running = false;
    }

#if LIBFLAC_ENABLED == 1
    // Finalize FLAC encoders
    if (s_encoder_a) {
        FLAC__stream_encoder_finish(s_encoder_a);
        FLAC__stream_encoder_delete(s_encoder_a);
        s_encoder_a = NULL;
    }
    if (s_encoder_b) {
        FLAC__stream_encoder_finish(s_encoder_b);
        FLAC__stream_encoder_delete(s_encoder_b);
        s_encoder_b = NULL;
    }
#endif

    // Close files
    if (s_file_a) {
        fclose(s_file_a);
        s_file_a = NULL;
    }
    if (s_file_b) {
        fclose(s_file_b);
        s_file_b = NULL;
    }

    s_recording_app = NULL;
    s_capture_rb = NULL;
    gui_app_set_status(app, "Recording stopped");
}
