/*
 * MISRC GUI - Simulated Device
 *
 * Provides a simulated capture device for development and testing without hardware.
 * Generates NTSC CVBS video on Channel A and VHS RF head signal on Channel B.
 */

//-----------------------------------------------------------------------------
// Simulation Feature Flags
//
// Set to 0 to disable non-ideal behaviors for cleaner test signals
//-----------------------------------------------------------------------------

// CVBS signal impairments
#define SIM_ENABLE_CVBS_NOISE           0   // Random noise on CVBS output

// VHS RF signal impairments
#define SIM_ENABLE_VHS_RF_NOISE         0   // Random noise on VHS RF output
#define SIM_ENABLE_VHS_HEAD_SWITCH      0   // Head switching noise at field boundaries
#define SIM_ENABLE_VHS_TRACKING_NOISE   0   // Low-level tracking jitter

// Analog circuit simulation
#define SIM_ENABLE_SOFT_CLIPPING        0   // Soft saturation near signal limits

//-----------------------------------------------------------------------------

#include "gui_simulated.h"
#include "gui_app.h"
#include "gui_oscilloscope.h"
#include "gui_extract.h"
#include "../misrc_common/ringbuffer.h"
#include "../misrc_common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

// Define M_PI if not available (Windows compatibility)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//-----------------------------------------------------------------------------
// NTSC Timing Constants (in samples at 40 MSPS)
//-----------------------------------------------------------------------------

#define NTSC_LINE_DURATION_US    63.555555  // One horizontal line (1/15734.264 Hz)
#define NTSC_LINE_SAMPLES        ((int)(NTSC_LINE_DURATION_US * 40.0))  // ~2542 samples per line
#define NTSC_HALF_LINE_SAMPLES   (NTSC_LINE_SAMPLES / 2)                // ~1271 samples

// Horizontal timing
#define NTSC_HSYNC_US            4.7       // H-sync pulse width
#define NTSC_HSYNC_SAMPLES       ((int)(NTSC_HSYNC_US * 40.0))          // ~188 samples
#define NTSC_BACK_PORCH_US       4.7       // Back porch (includes colorburst)
#define NTSC_BACK_PORCH_SAMPLES  ((int)(NTSC_BACK_PORCH_US * 40.0))
#define NTSC_FRONT_PORCH_US      1.5       // Front porch
#define NTSC_FRONT_PORCH_SAMPLES ((int)(NTSC_FRONT_PORCH_US * 40.0))
#define NTSC_COLORBURST_CYCLES   9         // Number of colorburst cycles
#define NTSC_COLORBURST_FREQ     3579545.0 // 3.579545 MHz color subcarrier

// Vertical timing (NTSC interlaced)
#define NTSC_LINES_PER_FRAME     525       // Total lines per frame (both fields)
#define NTSC_LINES_PER_FIELD     262       // Lines per field (262.5 lines, alternating)
#define NTSC_FRAME_SAMPLES       ((uint64_t)NTSC_LINE_SAMPLES * NTSC_LINES_PER_FRAME)

// Vertical blanking structure (per field)
#define NTSC_VBLANK_PRE_EQ_LINES      3     // Pre-equalizing (6 half-line pulses)
#define NTSC_VBLANK_VSYNC_LINES       3     // Vertical sync (6 serrated pulses)
#define NTSC_VBLANK_POST_EQ_LINES     3     // Post-equalizing (6 half-line pulses)
#define NTSC_VBLANK_BLANK_LINES       12    // Remaining VBI (black with normal H-sync)
#define NTSC_FIRST_ACTIVE_LINE        21    // First line with active video (0-indexed)

// Equalizing and serration pulse widths
#define NTSC_EQ_PULSE_US             2.3    // Equalizing pulse width
#define NTSC_EQ_PULSE_SAMPLES        ((int)(NTSC_EQ_PULSE_US * 40.0))
#define NTSC_SERR_PULSE_US           4.7    // Serration pulse width (same as H-sync)
#define NTSC_SERR_PULSE_SAMPLES      ((int)(NTSC_SERR_PULSE_US * 40.0))

//-----------------------------------------------------------------------------
// Video Signal Levels
//-----------------------------------------------------------------------------

#define SYNC_LEVEL      (-0.4)    // Sync tip
#define BLANKING_LEVEL  (0.0)     // Blanking/black level
#define BLACK_LEVEL     (0.075)   // Setup/pedestal (7.5 IRE)
#define WHITE_LEVEL     (1.0)     // Peak white (100 IRE)

//-----------------------------------------------------------------------------
// VHS RF Parameters
//-----------------------------------------------------------------------------

#define VHS_LUMA_CARRIER_SYNC    3400000.0   // 3.4 MHz at sync tip
#define VHS_LUMA_CARRIER_WHITE   4400000.0   // 4.4 MHz at white
#define VHS_CHROMA_CARRIER       629000.0    // 629 kHz down-converted chroma
#define VHS_FM_DEVIATION         (VHS_LUMA_CARRIER_WHITE - VHS_LUMA_CARRIER_SYNC)

//-----------------------------------------------------------------------------
// Signal Generation State
//-----------------------------------------------------------------------------

static uint64_t s_sim_sample_count = 0;
static double s_vhs_fm_phase = 0.0;  // FM phase must be accumulated (frequency varies with signal)

// Simple xorshift PRNG for noise
static uint32_t s_sim_rng_state = 12345;

static uint32_t sim_rand(void) {
    uint32_t x = s_sim_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_sim_rng_state = x;
    return x;
}

// Generate random float in range [-1, 1]
static float sim_noise(void) {
    return ((float)(sim_rand() & 0xFFFF) / 32768.0f) - 1.0f;
}

// Calculate continuous phase from absolute sample index
// This ensures phase continuity across line and field boundaries
static inline double phase_from_sample(uint64_t sample_index, double freq_hz) {
    return 2.0 * M_PI * freq_hz * ((double)sample_index / (double)SIM_SAMPLE_RATE);
}

//-----------------------------------------------------------------------------
// NTSC Line Type Classification
//-----------------------------------------------------------------------------

typedef enum {
    LINE_TYPE_PRE_EQ,       // Pre-equalizing pulses (half-line rate)
    LINE_TYPE_VSYNC,        // Vertical sync with serrations
    LINE_TYPE_POST_EQ,      // Post-equalizing pulses (half-line rate)
    LINE_TYPE_VBLANK,       // Vertical blanking (normal H-sync, black video)
    LINE_TYPE_ACTIVE        // Active video line
} line_type_t;

// Get the line type and field information for a given sample position
static line_type_t sim_get_line_type(uint64_t sample_index, int *out_field, int *out_line_in_field,
                                     int *out_sample_in_line, bool *out_is_half_line) {
    uint64_t sample_in_frame = sample_index % NTSC_FRAME_SAMPLES;
    int line_in_frame = (int)(sample_in_frame / NTSC_LINE_SAMPLES);
    int sample_in_line = (int)(sample_in_frame % NTSC_LINE_SAMPLES);

    int field, line_in_field;
    bool is_half_line = false;

    if (line_in_frame < NTSC_LINES_PER_FIELD) {
        field = 0;
        line_in_field = line_in_frame;
    } else if (line_in_frame == NTSC_LINES_PER_FIELD) {
        if (sample_in_line < NTSC_HALF_LINE_SAMPLES) {
            field = 0;
            line_in_field = 262;
            is_half_line = true;
        } else {
            field = 1;
            line_in_field = 0;
            sample_in_line -= NTSC_HALF_LINE_SAMPLES;
            is_half_line = true;
        }
    } else {
        field = 1;
        line_in_field = line_in_frame - NTSC_LINES_PER_FIELD;
    }

    *out_field = field;
    *out_line_in_field = line_in_field;
    *out_sample_in_line = sample_in_line;
    *out_is_half_line = is_half_line;

    if (line_in_field < NTSC_VBLANK_PRE_EQ_LINES) {
        return LINE_TYPE_PRE_EQ;
    } else if (line_in_field < NTSC_VBLANK_PRE_EQ_LINES + NTSC_VBLANK_VSYNC_LINES) {
        return LINE_TYPE_VSYNC;
    } else if (line_in_field < NTSC_VBLANK_PRE_EQ_LINES + NTSC_VBLANK_VSYNC_LINES + NTSC_VBLANK_POST_EQ_LINES) {
        return LINE_TYPE_POST_EQ;
    } else if (line_in_field < NTSC_FIRST_ACTIVE_LINE) {
        return LINE_TYPE_VBLANK;
    } else {
        return LINE_TYPE_ACTIVE;
    }
}

//-----------------------------------------------------------------------------
// Sync Pulse Generation
//-----------------------------------------------------------------------------

// Generate equalizing pulse signal (2 pulses per line at half-line rate)
static double sim_generate_eq_pulse(int sample_in_line, bool is_half_line) {
    int half_line = NTSC_HALF_LINE_SAMPLES;

    if (sample_in_line < NTSC_EQ_PULSE_SAMPLES) {
        return SYNC_LEVEL;
    }
    if (!is_half_line && sample_in_line >= half_line &&
        sample_in_line < half_line + NTSC_EQ_PULSE_SAMPLES) {
        return SYNC_LEVEL;
    }
    return BLANKING_LEVEL;
}

// Generate vertical sync with serrations
static double sim_generate_vsync_serration(int sample_in_line, bool is_half_line) {
    int serr_start1 = NTSC_HALF_LINE_SAMPLES - NTSC_SERR_PULSE_SAMPLES;
    if (sample_in_line >= serr_start1 && sample_in_line < NTSC_HALF_LINE_SAMPLES) {
        return BLANKING_LEVEL;
    }
    if (!is_half_line) {
        int serr_start2 = NTSC_LINE_SAMPLES - NTSC_SERR_PULSE_SAMPLES;
        if (sample_in_line >= serr_start2) {
            return BLANKING_LEVEL;
        }
    }
    return SYNC_LEVEL;
}

//-----------------------------------------------------------------------------
// Color Space Conversion
//-----------------------------------------------------------------------------

// RGB to YIQ conversion (NTSC color space)
static inline void rgb_to_yiq(double r, double g, double b,
                              double *y, double *i, double *q) {
    // NTSC YIQ matrix
    *y = 0.299 * r + 0.587 * g + 0.114 * b;
    *i = 0.596 * r - 0.274 * g - 0.322 * b;
    *q = 0.211 * r - 0.523 * g + 0.312 * b;
}

// 75% SMPTE color bars as RGB
// Order: white, yellow, cyan, green, magenta, red, blue, black
static const double bar_rgb[8][3] = {
    {0.75, 0.75, 0.75},  // white
    {0.75, 0.75, 0.00},  // yellow
    {0.00, 0.75, 0.75},  // cyan
    {0.00, 0.75, 0.00},  // green
    {0.75, 0.00, 0.75},  // magenta
    {0.75, 0.00, 0.00},  // red
    {0.00, 0.00, 0.75},  // blue
    {0.00, 0.00, 0.00},  // black
};

//-----------------------------------------------------------------------------
// Test Pattern Generation
//-----------------------------------------------------------------------------

// Get Y, I, Q values for current bar
static void sim_get_bar_yiq(int bar, double *y, double *i, double *q) {
    bar = bar % 8;
    rgb_to_yiq(bar_rgb[bar][0], bar_rgb[bar][1], bar_rgb[bar][2], y, i, q);
}

static double sim_generate_test_pattern(int pixel_in_line, int field, int active_width) {
    (void)field;
    int bar = (pixel_in_line * 8) / active_width;

    double y, i, q;
    sim_get_bar_yiq(bar, &y, &i, &q);
    (void)i; (void)q;  // Luma only for this function

    return y;
}

//-----------------------------------------------------------------------------
// CVBS Signal Generation
//-----------------------------------------------------------------------------

static double sim_generate_cvbs(uint64_t sample_index, int *out_line_number, int *out_field,
                                 double *out_chroma_i, double *out_chroma_q) {
    int field, line_in_field, sample_in_line;
    bool is_half_line;

    line_type_t line_type = sim_get_line_type(sample_index, &field, &line_in_field,
                                               &sample_in_line, &is_half_line);

    int line_in_frame = (field == 0) ? line_in_field : (NTSC_LINES_PER_FIELD + line_in_field);
    if (out_line_number) *out_line_number = line_in_frame;
    if (out_field) *out_field = field;

    // For CVBS output, we need the traditional per-line phase calculation that
    // NTSC decoders expect. The 227.5 cycles per line means 180° phase shift each line.
    // Phase within current line (based on sample position within line)
    double cycles_in_line = NTSC_COLORBURST_FREQ * (double)sample_in_line / (double)SIM_SAMPLE_RATE;
    double phase_in_line = 2.0 * M_PI * cycles_in_line;

    // Line-based phase offset: 180° per line (227.5 cycles = 227 full + 0.5)
    double line_phase_offset = (line_in_frame % 2) * M_PI;

    double subcarrier_phase = phase_in_line + line_phase_offset;

    double signal = 0.0;
    double chroma_i = 0.0;  // I component for quadrature modulation
    double chroma_q = 0.0;  // Q component for quadrature modulation

    switch (line_type) {
        case LINE_TYPE_PRE_EQ:
        case LINE_TYPE_POST_EQ:
            signal = sim_generate_eq_pulse(sample_in_line, is_half_line);
            break;

        case LINE_TYPE_VSYNC:
            signal = sim_generate_vsync_serration(sample_in_line, is_half_line);
            break;

        case LINE_TYPE_VBLANK:
            if (sample_in_line < NTSC_HSYNC_SAMPLES) {
                signal = SYNC_LEVEL;
            } else {
                signal = BLANKING_LEVEL;
            }
            break;

        case LINE_TYPE_ACTIVE: {
            int active_start = NTSC_HSYNC_SAMPLES + NTSC_BACK_PORCH_SAMPLES;
            int active_end = NTSC_LINE_SAMPLES - NTSC_FRONT_PORCH_SAMPLES;
            int active_width = active_end - active_start;

            if (sample_in_line < NTSC_HSYNC_SAMPLES) {
                signal = SYNC_LEVEL;
            }
            else if (sample_in_line < active_start) {
                int back_porch_pos = sample_in_line - NTSC_HSYNC_SAMPLES;
                int burst_start = (int)(0.6 * 40);
                int burst_duration = (int)(NTSC_COLORBURST_CYCLES * 40.0 / (NTSC_COLORBURST_FREQ / 1000000.0));

                if (back_porch_pos >= burst_start && back_porch_pos < burst_start + burst_duration) {
                    // Colorburst - reference phase for decoder
                    // NTSC burst is along the -U axis (180° from +U), which is approximately -I
                    // For simplicity, use a sine burst (reference phase 0)
                    double burst = 0.15 * sin(subcarrier_phase);
                    signal = BLANKING_LEVEL + burst;
                } else {
                    signal = BLANKING_LEVEL;
                }
            }
            else if (sample_in_line < active_end) {
                int pixel = sample_in_line - active_start;
                int bar = (pixel * 8) / active_width;

                // Get Y, I, Q from RGB color bars
                double y, i, q;
                sim_get_bar_yiq(bar, &y, &i, &q);

                // Store I/Q for VHS generation
                chroma_i = i;
                chroma_q = q;

                // Generate NTSC chroma using quadrature modulation: I*cos(wt) + Q*sin(wt)
                // This is the standard NTSC modulation formula
                double chroma = i * cos(subcarrier_phase) + q * sin(subcarrier_phase);

                signal = BLACK_LEVEL + y * (WHITE_LEVEL - BLACK_LEVEL) + chroma;
            }
            else {
                signal = BLANKING_LEVEL;
            }
            break;
        }
    }

    // Output I/Q components for VHS quadrature generation
    if (out_chroma_i) {
        *out_chroma_i = chroma_i;
    }
    if (out_chroma_q) {
        *out_chroma_q = chroma_q;
    }

    return signal;
}

//-----------------------------------------------------------------------------
// VHS RF Signal Generation
//-----------------------------------------------------------------------------

// VHS color-under: generate 629 kHz QAM chroma signal from I/Q components
// VHS color-under is a quadrature signal, not single-axis amplitude+phase

static double sim_generate_vhs_rf(double cvbs_luma, double chroma_i, double chroma_q,
                                   int line_in_frame, int field, int sample_in_line) {
    (void)field;

    // === FM Luminance ===
    // VHS FM-encodes the luminance signal
    double normalized = (cvbs_luma - SYNC_LEVEL) / (WHITE_LEVEL - SYNC_LEVEL);
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;

    double fm_freq = VHS_LUMA_CARRIER_SYNC + normalized * VHS_FM_DEVIATION;

    // FM phase must be accumulated since frequency varies with signal
    double fm_signal = sin(s_vhs_fm_phase) * 0.7;
    s_vhs_fm_phase += 2.0 * M_PI * fm_freq / SIM_SAMPLE_RATE;
    if (s_vhs_fm_phase > 2.0 * M_PI) s_vhs_fm_phase -= 2.0 * M_PI;

    // === VHS Color-Under (629 kHz QAM) ===
    // VHS color-under is a quadrature amplitude modulated signal at 629 kHz.
    // The decoder expects the carrier to have a consistent phase relationship
    // to the line timing (like NTSC burst provides for 3.58 MHz).
    //
    // Use per-line phase calculation so the decoder can lock properly.
    // VHS also applies a 90° phase rotation on odd lines for crosstalk cancellation.

    // Per-line phase for 629 kHz carrier (same approach as CVBS uses for 3.58 MHz)
    double vhs_cycles_in_line = VHS_CHROMA_CARRIER * (double)sample_in_line / (double)SIM_SAMPLE_RATE;
    double vhs_phase = 2.0 * M_PI * vhs_cycles_in_line;

    // VHS 90° phase rotation on odd lines for crosstalk cancellation
    double vhs_line_rotation = (line_in_frame % 2) ? (M_PI / 2.0) : 0.0;
    vhs_phase += vhs_line_rotation;

    // Generate 629 kHz QAM chroma: I*cos(wt) + Q*sin(wt)
    // This is the same quadrature formula used for NTSC, just at 629 kHz
    double chroma_under = chroma_i * cos(vhs_phase) + chroma_q * sin(vhs_phase);

    // Scale down VHS chroma (color-under level is lower than NTSC broadcast)
    chroma_under *= 0.5;

    double head_noise = 0.0;
#if SIM_ENABLE_VHS_HEAD_SWITCH
    int line_in_field = (field == 0) ? line_in_frame : (line_in_frame - NTSC_LINES_PER_FIELD);
    if (line_in_field <= 6) {
        double switch_intensity = 1.0 - (line_in_field / 6.0);
        head_noise = sim_noise() * 0.4 * switch_intensity;

        if (line_in_field <= 2 && (sim_rand() % 100) < 5) {
            head_noise += (sim_noise() > 0 ? 0.5 : -0.5);
        }
    }
#endif

    double tracking_noise = 0.0;
#if SIM_ENABLE_VHS_TRACKING_NOISE
    tracking_noise = sim_noise() * 0.02 * (field == 0 ? 1.0 : 1.1);
#endif

    return fm_signal + chroma_under + head_noise + tracking_noise;
}

//-----------------------------------------------------------------------------
// Simulated Capture Thread
//-----------------------------------------------------------------------------

static int simulated_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;

    int16_t *buf_a = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));
    int16_t *buf_b = (int16_t *)malloc(SIM_BUFFER_SIZE * sizeof(int16_t));

    if (!buf_a || !buf_b) {
        fprintf(stderr, "[SIM] Failed to allocate buffers\n");
        free(buf_a);
        free(buf_b);
        return -1;
    }

    fprintf(stderr, "[SIM] Simulated capture thread started at %d MSPS\n", SIM_SAMPLE_RATE / 1000000);
    fprintf(stderr, "[SIM] Channel A: CVBS composite video (NTSC color bars)\n");
    fprintf(stderr, "[SIM] Channel B: VHS RF head signal (FM luma + 629kHz chroma)\n");

    gui_extract_init_record_rbs();

#if SIM_ENABLE_CVBS_NOISE
    double cvbs_noise = 0.02;
#endif
#if SIM_ENABLE_VHS_RF_NOISE
    double rf_noise = 0.03;
#endif

    s_sim_rng_state = (uint32_t)get_time_ms();

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);

    uint64_t batch_count = 0;

    while (atomic_load(&app->sim_running)) {
        for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
            int line_in_frame = 0;
            int field = 0;
            double chroma_i = 0.0;
            double chroma_q = 0.0;

            // Generate CVBS signal and get I/Q chroma components for VHS
            double cvbs = sim_generate_cvbs(s_sim_sample_count, &line_in_frame, &field, &chroma_i, &chroma_q);
#if SIM_ENABLE_CVBS_NOISE
            cvbs += sim_noise() * cvbs_noise;
#endif

            // For VHS, compute luma from CVBS (subtract the chroma we added)
            // Must use the same phase calculation as sim_generate_cvbs() to correctly extract luma
            // Get the correct sample_in_line from sim_get_line_type (handles half-lines properly)
            int field_tmp, line_in_field_tmp, sample_in_line;
            bool is_half_line_tmp;
            sim_get_line_type(s_sim_sample_count, &field_tmp, &line_in_field_tmp, &sample_in_line, &is_half_line_tmp);

            double cycles_in_line = NTSC_COLORBURST_FREQ * (double)sample_in_line / (double)SIM_SAMPLE_RATE;
            double phase_in_line = 2.0 * M_PI * cycles_in_line;
            double line_phase_offset = (line_in_frame % 2) * M_PI;
            double subcarrier_phase = phase_in_line + line_phase_offset;
            // Reconstruct the chroma we added using quadrature: I*cos(wt) + Q*sin(wt)
            double chroma_at_sample = chroma_i * cos(subcarrier_phase) + chroma_q * sin(subcarrier_phase);
            double cvbs_luma = cvbs - chroma_at_sample;

            // Generate VHS RF with I/Q quadrature chroma at 629 kHz
            double vhs_rf = sim_generate_vhs_rf(cvbs_luma, chroma_i, chroma_q, line_in_frame, field, sample_in_line);
#if SIM_ENABLE_VHS_RF_NOISE
            vhs_rf += sim_noise() * rf_noise;
#endif

#if SIM_ENABLE_SOFT_CLIPPING
            // Soft clipping - analog-style saturation near limits
            if (cvbs > 0.95) cvbs = 0.95 + 0.05 * tanh((cvbs - 0.95) * 10.0);
            if (cvbs < -0.95) cvbs = -0.95 + 0.05 * tanh((cvbs + 0.95) * 10.0);
            if (vhs_rf > 0.95) vhs_rf = 0.95 + 0.05 * tanh((vhs_rf - 0.95) * 10.0);
            if (vhs_rf < -0.95) vhs_rf = -0.95 + 0.05 * tanh((vhs_rf + 0.95) * 10.0);
#endif

            buf_a[i] = (int16_t)(cvbs * 1400.0);
            buf_b[i] = (int16_t)(vhs_rf * 1024.0);  // 50% of full scale

            s_sim_sample_count++;
        }

        gui_oscilloscope_update_display(app, buf_a, buf_b, SIM_BUFFER_SIZE);

        atomic_fetch_add(&app->total_samples, SIM_BUFFER_SIZE);
        atomic_fetch_add(&app->samples_a, SIM_BUFFER_SIZE);
        atomic_fetch_add(&app->samples_b, SIM_BUFFER_SIZE);
        atomic_fetch_add(&app->frame_count, 1);
        atomic_store(&app->last_callback_time_ms, get_time_ms());

        // Update peak values for VU meters
        int16_t peak_a_pos = 0, peak_a_neg = 0;
        int16_t peak_b_pos = 0, peak_b_neg = 0;
        for (int i = 0; i < SIM_BUFFER_SIZE; i += 16) {
            if (buf_a[i] > peak_a_pos) peak_a_pos = buf_a[i];
            if (buf_a[i] < peak_a_neg) peak_a_neg = buf_a[i];
            if (buf_b[i] > peak_b_pos) peak_b_pos = buf_b[i];
            if (buf_b[i] < peak_b_neg) peak_b_neg = buf_b[i];
        }
        atomic_store(&app->peak_a_pos, (uint16_t)peak_a_pos);
        atomic_store(&app->peak_a_neg, (uint16_t)(-peak_a_neg));
        atomic_store(&app->peak_b_pos, (uint16_t)peak_b_pos);
        atomic_store(&app->peak_b_neg, (uint16_t)(-peak_b_neg));

        // Write to record ringbuffers if recording is enabled
        bool use_flac = false;
        if (gui_extract_is_recording(&use_flac)) {
            ringbuffer_t *rb_a = gui_extract_get_record_rb_a();
            ringbuffer_t *rb_b = gui_extract_get_record_rb_b();

            if (rb_a && rb_b) {
                if (use_flac) {
                    size_t sample_bytes = SIM_BUFFER_SIZE * sizeof(int32_t);

                    int32_t *write_a = (int32_t *)rb_write_ptr(rb_a, sample_bytes);
                    int32_t *write_b = (int32_t *)rb_write_ptr(rb_b, sample_bytes);

                    if (write_a && write_b) {
                        for (int i = 0; i < SIM_BUFFER_SIZE; i++) {
                            write_a[i] = (int32_t)buf_a[i] << 4;
                            write_b[i] = (int32_t)buf_b[i] << 4;
                        }
                        rb_write_finished(rb_a, sample_bytes);
                        rb_write_finished(rb_b, sample_bytes);
                    }
                } else {
                    size_t sample_bytes = SIM_BUFFER_SIZE * sizeof(int16_t);

                    void *write_a = rb_write_ptr(rb_a, sample_bytes);
                    void *write_b = rb_write_ptr(rb_b, sample_bytes);

                    if (write_a && write_b) {
                        memcpy(write_a, buf_a, sample_bytes);
                        memcpy(write_b, buf_b, sample_bytes);
                        rb_write_finished(rb_a, sample_bytes);
                        rb_write_finished(rb_b, sample_bytes);
                    }
                }
            }
        }

        batch_count++;
        thrd_sleep_ms(SIM_UPDATE_INTERVAL_MS);
    }

    fprintf(stderr, "[SIM] Simulated capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);

    free(buf_a);
    free(buf_b);

    return 0;
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int gui_simulated_start(gui_app_t *app) {
    fprintf(stderr, "[SIM] Starting simulated capture\n");

    // Reset statistics
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, SIM_SAMPLE_RATE);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    // Reset display buffers
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    // Reset signal generation state
    s_sim_sample_count = 0;
    s_vhs_fm_phase = 0.0;

    // Start thread
    atomic_store(&app->sim_running, true);
    thrd_t thread;
    if (thrd_create(&thread, simulated_capture_thread, app) != thrd_success) {
        fprintf(stderr, "[SIM] Failed to create simulated capture thread\n");
        atomic_store(&app->sim_running, false);
        return -1;
    }
    app->sim_thread = (void *)(uintptr_t)thread;

    app->is_capturing = true;
    gui_app_set_status(app, "Simulated capture running");

    return 0;
}

void gui_simulated_stop(gui_app_t *app) {
    if (!atomic_load(&app->sim_running)) return;

    fprintf(stderr, "[SIM] Stopping simulated capture\n");

    atomic_store(&app->sim_running, false);

    if (app->sim_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)app->sim_thread;
        thrd_join(thread, NULL);
        app->sim_thread = NULL;
    }

    atomic_store(&app->stream_synced, false);
    app->is_capturing = false;

    gui_app_set_status(app, "Simulated capture stopped");
}

bool gui_simulated_is_running(gui_app_t *app) {
    return atomic_load(&app->sim_running);
}
