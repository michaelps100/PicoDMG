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

static uint8_t  s_hyper_voice;
static uint8_t  s_hvoice_ctrl;
static uint8_t  s_hvoice_chan_ctrl;
static uint32_t s_dma_index = 0;
static uint32_t s_dma_subsample = 0;

static uint32_t s_cycle_accum = 0;
static int      s_samples_rendered_in_frame = 0;
static uint8_t  s_cfg_volume = 5;

/* -------------------------------------------------------------------------- */
void ws_audio_init(void) {
    ws_audio_reset();
}

void ws_audio_done(void) {
    /* Nothing to deallocate */
}

void ws_audio_reset(void) {
    memset(s_period, 0, sizeof(s_period));
    memset(s_volume, 0, sizeof(s_volume));
    s_volume[1] = 128; /* Silence level for Voice PCM */
    s_control = 0;
    s_output_control = 0;
    s_sample_ram_pos = 0;
    s_voice_volume = 0;
    s_sweep_value = 0;
    s_sweep_step = 0;
    s_noise_control = 0;
    s_nreg = 0;

    for (int ch = 0; ch < 4; ch++) {
        s_period_counter[ch] = 0;
        s_sample_pos[ch] = 0;
    }
    s_sweep_divider = 8192;
    s_sweep_counter = 0;
    s_hyper_voice = 0x80;
    s_hvoice_ctrl = 0;
    s_hvoice_chan_ctrl = 0;
    s_dma_index = 0;
    s_dma_subsample = 0;
    s_cycle_accum = 0;
    s_samples_rendered_in_frame = 0;
}

void ws_audio_set_volume(uint8_t volume_level) {
    if (volume_level > 5) volume_level = 5;
    s_cfg_volume = volume_level;
}

void ws_audio_sync_from_ioram(void) {
    for (int p = 0x80; p <= 0x95; p++) {
        ws_audio_port_write(p, ws_ioRam[p]);
    }
}

/* -------------------------------------------------------------------------- */
void ws_audio_port_write(uint32_t port, uint8_t value) {
    if (port >= 0x80 && port <= 0x87) {
        int ch = (port - 0x80) >> 1;
        if (port & 1) {
            s_period[ch] = (s_period[ch] & 0x00FF) | (((uint16_t)value & 0x07) << 8);
        } else {
            s_period[ch] = (s_period[ch] & 0x0700) | ((uint16_t)value & 0xFF);
        }
    } else if (port >= 0x88 && port <= 0x8B) {
        s_volume[port - 0x88] = value;
    } else {
        switch (port) {
            case 0x52:
                if (value & 0x80) {
                    s_dma_index = 0;
                    s_dma_subsample = 0;
                }
                break;
            case 0x6A: s_hvoice_ctrl = value; break;
            case 0x6B: s_hvoice_chan_ctrl = value & 0x6F; break;
            case 0x8C: s_sweep_value = value; break;
            case 0x8D:
                s_sweep_step = value;
                s_sweep_counter = s_sweep_step + 1;
                s_sweep_divider = 8192;
                break;
            case 0x8E:
                if (value & 0x08) s_nreg = 0;
                s_noise_control = value & 0x17;
                break;
            case 0x8F: s_sample_ram_pos = value; break;
            case 0x90:
                for (int ch = 0; ch < 4; ch++) {
                    if (!(s_control & (1 << ch)) && (value & (1 << ch))) {
                        s_period_counter[ch] = 1;
                        s_sample_pos[ch] = 0x1F;
                    }
                }
                if (!(s_control & 0x20) && (value & 0x20)) {
                    s_volume[1] = 128;
                }
                s_control = value;
                break;
            case 0x91: s_output_control = value & 0x0F; break;
            case 0x92: s_nreg = (s_nreg & 0xFF00) | (uint16_t)value; break;
            case 0x93: s_nreg = (s_nreg & 0x00FF) | (((uint16_t)value & 0x7F) << 8); break;
            case 0x94: s_voice_volume = value & 0x0F; break;
            case 0x95: s_hyper_voice = value; break;
            default: break;
        }
    }
}

uint8_t ws_audio_port_read(uint32_t port) {
    if (port >= 0x80 && port <= 0x87) {
        int ch = (port - 0x80) >> 1;
        if (port & 1) return (uint8_t)(s_period[ch] >> 8);
        return (uint8_t)(s_period[ch] & 0xFF);
    } else if (port >= 0x88 && port <= 0x8B) {
        return s_volume[port - 0x88];
    } else {
        switch (port) {
            case 0x6A: return s_hvoice_ctrl;
            case 0x6B: return s_hvoice_chan_ctrl;
            case 0x8C: return s_sweep_value;
            case 0x8D: return s_sweep_step;
            case 0x8E: return s_noise_control;
            case 0x8F: return s_sample_ram_pos;
            case 0x90: return s_control;
            case 0x91: return s_output_control | 0x80; /* Headphone always detected */
            case 0x92: return (uint8_t)(s_nreg & 0xFF);
            case 0x93: return (uint8_t)((s_nreg >> 8) & 0xFF);
            case 0x94: return s_voice_volume;
            case 0x95: return s_hyper_voice;
            default: return 0xFF;
        }
    }
}

/* -------------------------------------------------------------------------- */
static inline uint32_t ws_audio_render_sample(void) {
    s_cycle_accum += CYCLES_PER_SAMPLE_FP16;
    int cycles_step = (int)(s_cycle_accum >> 16);
    s_cycle_accum &= 0xFFFF;

    /* Sound DMA processing at hardware rate if active */
    if (ws_ioRam[0x52] & 0x80) {
        /* DMA frequencies: 0: 4000Hz, 1: 6000Hz, 2: 12000Hz, 3: 24000Hz */
        static const uint32_t dma_freq_step[4] = { 5944, 8916, 17833, 35665 };
        s_dma_subsample += dma_freq_step[ws_ioRam[0x52] & 3];
        while (s_dma_subsample >= 65536) {
            s_dma_subsample -= 65536;
            if (ws_ioRam[0x52] & 0x80) {
                uint32_t src_base = (((uint32_t)ws_ioRam[0x4C] & 0x0F) << 16) |
                                    (((uint32_t)ws_ioRam[0x4B]) << 8) |
                                    ((uint32_t)ws_ioRam[0x4A]);
                uint32_t cnt = (((uint32_t)ws_ioRam[0x50] & 0x0F) << 16) |
                               (((uint32_t)ws_ioRam[0x4F]) << 8) |
                               ((uint32_t)ws_ioRam[0x4E]);
                if (cnt > 0) {
                    uint32_t addr = (ws_ioRam[0x52] & 0x40) ? (src_base - s_dma_index) : (src_base + s_dma_index);
                    uint8_t val = cpu_readmem20(addr & 0xFFFFF);
                    s_dma_index++;
                    if (s_dma_index >= cnt) {
                        s_dma_index = 0;
                        if (!(ws_ioRam[0x52] & 0x08)) {
                            ws_ioRam[0x52] &= ~0x80;
                        }
                    }
                    if (ws_ioRam[0x52] & 0x10) {
                        s_hyper_voice = val;
                    } else {
                        s_volume[1] = val;
                    }
                }
            }
        }
    }

    int32_t ch_l[4] = {0, 0, 0, 0};
    int32_t ch_r[4] = {0, 0, 0, 0};

    /* Channel 0: Wavetable */
    if (s_control & 0x01) {
        uint32_t pt = 2048 - s_period[0];
        if (pt > 4) {
            s_period_counter[0] -= cycles_step;
            while (s_period_counter[0] <= 0) {
                s_sample_pos[0] = (s_sample_pos[0] + 1) & 0x1F;
                s_period_counter[0] += (int32_t)pt;
            }
            uint32_t addr = (((uint32_t)s_sample_ram_pos << 6) + 0 + (s_sample_pos[0] >> 1)) & 0xFFFF;
            uint8_t b = internalRam[addr];
            int sample = (s_sample_pos[0] & 1) ? ((b >> 4) & 0x0F) : (b & 0x0F);
            sample -= 8;
            ch_l[0] = sample * ((s_volume[0] >> 4) & 0x0F);
            ch_r[0] = sample * (s_volume[0] & 0x0F);
        }
    }

    /* Channel 1: Wavetable OR Voice PCM */
    if (s_control & 0x02) {
        if (s_control & 0x20) {
            /* Voice Mode: 8-bit unsigned PCM sample (0..255, silence = 128) */
            int32_t v_sample = (int32_t)s_volume[1] - 128; /* Range: -128 .. +127 */
            int32_t vl = 0;
            int32_t vr = 0;
            if (s_voice_volume & 0x0F) {
                if (s_voice_volume & 0x08)      vl = v_sample;
                else if (s_voice_volume & 0x04) vl = v_sample >> 1;

                if (s_voice_volume & 0x02)      vr = v_sample;
                else if (s_voice_volume & 0x01) vr = v_sample >> 1;
            } else {
                /* If port 0x94 volume not configured, play at full volume */
                vl = v_sample;
                vr = v_sample;
            }
            ch_l[1] = vl;
            ch_r[1] = vr;
        } else {
            uint32_t pt = 2048 - s_period[1];
            if (pt > 4) {
                s_period_counter[1] -= cycles_step;
                while (s_period_counter[1] <= 0) {
                    s_sample_pos[1] = (s_sample_pos[1] + 1) & 0x1F;
                    s_period_counter[1] += (int32_t)pt;
                }
                uint32_t addr = (((uint32_t)s_sample_ram_pos << 6) + 16 + (s_sample_pos[1] >> 1)) & 0xFFFF;
                uint8_t b = internalRam[addr];
                int sample = (s_sample_pos[1] & 1) ? ((b >> 4) & 0x0F) : (b & 0x0F);
                sample -= 8;
                ch_l[1] = sample * ((s_volume[1] >> 4) & 0x0F);
                ch_r[1] = sample * (s_volume[1] & 0x0F);
            }
        }
    }

    /* Channel 2: Wavetable with hardware Frequency Sweep */
    if (s_control & 0x04) {
        if ((s_control & 0x40) && s_sweep_value) {
            s_sweep_divider -= cycles_step;
            if (s_sweep_divider <= 0) {
                s_sweep_divider += 8192;
                s_sweep_counter--;
                if (s_sweep_counter <= 0) {
                    s_sweep_counter = s_sweep_step + 1;
                    s_period[2] = (s_period[2] + (int8_t)s_sweep_value) & 0x7FF;
                }
            }
        }
        uint32_t pt = 2048 - s_period[2];
        if (pt > 4) {
            s_period_counter[2] -= cycles_step;
            while (s_period_counter[2] <= 0) {
                s_sample_pos[2] = (s_sample_pos[2] + 1) & 0x1F;
                s_period_counter[2] += (int32_t)pt;
            }
            uint32_t addr = (((uint32_t)s_sample_ram_pos << 6) + 32 + (s_sample_pos[2] >> 1)) & 0xFFFF;
            uint8_t b = internalRam[addr];
            int sample = (s_sample_pos[2] & 1) ? ((b >> 4) & 0x0F) : (b & 0x0F);
            sample -= 8;
            ch_l[2] = sample * ((s_volume[2] >> 4) & 0x0F);
            ch_r[2] = sample * (s_volume[2] & 0x0F);
        }
    }

    /* Channel 3: Wavetable OR LFSR Noise */
    if (s_control & 0x08) {
        if ((s_control & 0x80) && (s_noise_control & 0x10)) {
            /* Noise Mode: 15-bit Galois LFSR */
            uint32_t pt = 2048 - s_period[3];
            if (pt < 4) pt = 4;
            s_period_counter[3] -= cycles_step;
            while (s_period_counter[3] <= 0) {
                static const uint8_t stab[8] = { 14, 10, 13, 4, 8, 6, 9, 11 };
                s_nreg = ((s_nreg << 1) | ((1 ^ (s_nreg >> 7) ^ (s_nreg >> stab[s_noise_control & 7])) & 1)) & 0x7FFF;
                s_period_counter[3] += (int32_t)pt;
            }
            int sample = (s_nreg & 1) ? 7 : -8;
            ch_l[3] = sample * ((s_volume[3] >> 4) & 0x0F);
            ch_r[3] = sample * (s_volume[3] & 0x0F);
        } else {
            uint32_t pt = 2048 - s_period[3];
            if (pt > 4) {
                s_period_counter[3] -= cycles_step;
                while (s_period_counter[3] <= 0) {
                    s_sample_pos[3] = (s_sample_pos[3] + 1) & 0x1F;
                    s_period_counter[3] += (int32_t)pt;
                }
                uint32_t addr = (((uint32_t)s_sample_ram_pos << 6) + 48 + (s_sample_pos[3] >> 1)) & 0xFFFF;
                uint8_t b = internalRam[addr];
                int sample = (s_sample_pos[3] & 1) ? ((b >> 4) & 0x0F) : (b & 0x0F);
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
