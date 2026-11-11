/**
 * ws_audio.c - WonderSwan Sound Emulation for PicoWS
 *
 * Implements 4-channel audio generation for Bandai WonderSwan / WonderSwan Color:
 *   - Channel 0: 32-sample wavetable (4-bit samples)
 *   - Channel 1: 32-sample wavetable OR Voice / Direct PCM
 *   - Channel 2: 32-sample wavetable with hardware Frequency Sweep
 *   - Channel 3: 32-sample wavetable OR 15-bit LFSR Noise
 *   - Output mixed at 44.1 kHz stereo to MAX98357A I2S
 */

#include "ws_audio.h"
#include <string.h>

#include "types.h"
#include "memory.h"
#include "io.h"
#include "external_audio.h"

extern uint8_t cpu_readmem20(uint32_t addr);

#define WS_CLOCK_HZ            3072000ULL
#define AUDIO_SAMPLE_RATE       44100ULL
#define SAMPLES_PER_FRAME       735        /* 44100 Hz / 60 FPS */
#define SCANLINES_PER_FRAME     159

/* Cycles of 3.072MHz per 44.1kHz audio sample in 16.16 fixed-point */
#define CYCLES_PER_SAMPLE_FP16  ((uint32_t)((WS_CLOCK_HZ << 16) / AUDIO_SAMPLE_RATE))

/* Register state */
static uint16_t s_period[4];
static uint8_t  s_volume[4];
static uint8_t  s_control;
static uint8_t  s_output_control;
static uint8_t  s_sample_ram_pos;
static uint8_t  s_voice_volume;
static uint8_t  s_sweep_value;
static uint8_t  s_sweep_step;
static uint8_t  s_noise_control;
static uint16_t s_nreg = 0;

static int32_t  s_period_counter[4];
static uint8_t  s_sample_pos[4];
static int32_t  s_sweep_divider;
static int32_t  s_sweep_counter;
                sample -= 8;
                ch_l[3] = sample * ((s_volume[3] >> 4) & 0x0F);
                ch_r[3] = sample * (s_volume[3] & 0x0F);
            }
        }
    }

    int32_t mix_l = ch_l[0] + ch_l[1] + ch_l[2] + ch_l[3];
    int32_t mix_r = ch_r[0] + ch_r[1] + ch_r[2] + ch_r[3];

    if (s_hvoice_ctrl & 0x80) {
        int32_t hv_sample = (int32_t)s_hyper_voice - 128;
        mix_l += hv_sample;
        mix_r += hv_sample;
    }

    /* Master volume scaling (Clean Light UI level 1..5) */
    static const int32_t vol_table[6] = { 0, 51, 102, 154, 205, 256 };
    int32_t cur_vol = vol_table[s_cfg_volume];

    int32_t out_l = (mix_l * 54 * cur_vol) >> 8;
    int32_t out_r = (mix_r * 54 * cur_vol) >> 8;

    if (out_l > 32767)  out_l = 32767;
    else if (out_l < -32768) out_l = -32768;
    if (out_r > 32767)  out_r = 32767;
    else if (out_r < -32768) out_r = -32768;

    return ((uint32_t)(uint16_t)(int16_t)out_l << 16) | (uint16_t)(int16_t)out_r;
}

/* -------------------------------------------------------------------------- */
void ws_audio_process_line(int line) {
    if (line < 0 || line >= SCANLINES_PER_FRAME) return;
    if (line == 0) {
        s_samples_rendered_in_frame = 0;
    }

    int target = (line + 1) * SAMPLES_PER_FRAME / SCANLINES_PER_FRAME;
    int count = target - s_samples_rendered_in_frame;
    if (count <= 0) return;

    uint32_t line_samples[8];
    if (count > 8) count = 8;

    for (int i = 0; i < count; i++) {
        line_samples[i] = ws_audio_render_sample();
    }
    s_samples_rendered_in_frame += count;

    EXT_AUDIO_ENQUEUE_SAMPLES(line_samples, count);
}

