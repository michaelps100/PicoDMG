#include "p8_audio.h"
#include "i2s.h"
#include "pico/stdlib.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define P8_SAMPLE_RATE 22050
#define BUFFER_SAMPLES 735 /* 22050 / 30 fps */
#define MAX_AUDIO_FRAME_SAMPLES 1470 /* Max dynamic refill headroom */

static const uint8_t *s_sfx_mem = NULL;
static const uint8_t *s_music_mem = NULL;

static i2s_config_t s_i2s_cfg;
/* Stereo interleaved at 44.1 kHz (2x oversampling of 22050 Hz synthesis) */
static int16_t s_dma_stream[MAX_AUDIO_FRAME_SAMPLES * 4];
static bool s_i2s_ready = false;
static uint8_t s_master_volume = 4; /* 0..5 */
static bool s_audio_paused = false;

/* Precomputed 32-bit phase increments for the 64 PICO-8 pitches (DDS accumulator: 2^32 = 1 cycle) */
static uint32_t s_pitch_phase_inc[64];

/* Channel state */
typedef struct {
    int sfx_id;
    int note_idx;
    uint32_t tick_counter;
    uint32_t ticks_per_note;
    uint32_t phase;           /* 32-bit phase accumulator: [0, 2^32-1] wraps naturally */
    uint32_t phase_inc;       /* 32-bit phase step per sample */
    uint32_t start_phase_inc; /* phase step at note start */
    uint32_t target_phase_inc;/* target phase step (for slide) */
    uint32_t phase_detuned;   /* for phaser instrument 7 */
    int pitch;                /* current note pitch (0..63) */
    int prev_pitch;           /* previous note pitch for slide */
    int prev_vol;             /* previous note volume for slide */
    int volume;               /* 0..7 */
    int current_volume;       /* 0..7 modulated */
    int waveform;             /* 0..7 */
    int effect;               /* 0..7 */
    uint32_t noise_seed;      /* RNG seed for brownian noise */
    float noise_sample;       /* low-pass filtered noise state */
    bool active;
    bool is_music;
    bool can_loop;
    int offset;
    int length;
} p8_voice_t;

static p8_voice_t s_voices[P8_AUDIO_CHANNELS];

/* Music state */
typedef struct {
    int pattern;
    int loop_start_pattern;
    bool playing;
    int channel_mask;
    uint32_t sample_counter;
    uint32_t total_samples;
} p8_music_state_t;

static p8_music_state_t s_music_state = { -1, 0, false, 0, 0, 0 };

/* Initialize pitch table using 32-bit phase accumulator (2^32 = 1 full wave cycle) */
static void init_pitch_table(void) {
    for (int i = 0; i < 64; i++) {
        /* C-0 is pitch 0 = ~65.406 Hz (C2) in PICO-8, pitch 33 is A4 (440 Hz) */
        float freq = 440.0f * powf(2.0f, (float)(i - 33) / 12.0f);
        s_pitch_phase_inc[i] = (uint32_t)(((double)freq * 4294967296.0) / (double)P8_SAMPLE_RATE);
    }
}

void p8_audio_init(void) {
    init_pitch_table();
    memset(s_voices, 0, sizeof(s_voices));
    for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
        s_voices[i].noise_seed = 0x12345678u + i * 0x9ABCDEF1u;
        s_voices[i].noise_sample = 0.0f;
        s_voices[i].prev_pitch = 24;
        s_voices[i].prev_vol = 7;
        s_voices[i].can_loop = true;
    }

    s_i2s_cfg = i2s_get_default_config();
    s_i2s_cfg.sample_freq = 44100; /* Standard MAX98357A supported rate */
    s_i2s_cfg.channel_count = 2;
    s_i2s_cfg.dma_trans_count = BUFFER_SAMPLES * 2;
    i2s_volume(&s_i2s_cfg, s_master_volume);
    i2s_init(&s_i2s_cfg);
    i2s_audio_start(&s_i2s_cfg);
    s_i2s_ready = true;
}

void p8_audio_set_memory(const uint8_t *sfx_mem, const uint8_t *music_mem) {
    s_sfx_mem = sfx_mem;
    s_music_mem = music_mem;
}

static void trigger_voice_note(p8_voice_t *v, int note_idx) {
    if (v->sfx_id < 0 || v->sfx_id >= 64 || !s_sfx_mem) {
        v->active = false;
        return;
    }

    const uint8_t *sfx = s_sfx_mem + (v->sfx_id * 68);
    int speed = sfx[65];
    if (speed < 1) speed = 1;
    v->ticks_per_note = (uint32_t)speed * 183; /* 1 tick = 183 samples @ 22050 Hz */
    v->tick_counter = 0;

    const uint8_t *note_bytes = sfx + (note_idx * 2);
    uint16_t note = (uint16_t)note_bytes[0] | ((uint16_t)note_bytes[1] << 8);

    int pitch = note & 0x3F;
    v->pitch = pitch;
    v->waveform = (note >> 6) & 0x07;
    v->volume = (note >> 9) & 0x07;
    v->effect = (note >> 12) & 0x07;
    v->current_volume = v->volume;

    uint32_t current_inc = s_pitch_phase_inc[pitch < 64 ? pitch : 63];

    if (v->effect == 1) {
        /* Effect 1 (Slide): slides from previous note pitch and volume to current note */
        uint32_t prev_inc = s_pitch_phase_inc[v->prev_pitch < 64 ? v->prev_pitch : 63];
        v->start_phase_inc = prev_inc;
        v->target_phase_inc = current_inc;
        v->phase_inc = prev_inc;
    } else {
        v->start_phase_inc = current_inc;
        v->target_phase_inc = current_inc;
        v->phase_inc = current_inc;
        v->prev_pitch = pitch;
        v->prev_vol = v->volume;
    }
}

void p8_audio_sfx(int sfx_id, int channel, int offset, int length) {
    if (sfx_id == -1) {
        /* Stop sfx on channel */
        if (channel >= 0 && channel < P8_AUDIO_CHANNELS) {
            s_voices[channel].active = false;
        } else if (channel == -1) {
            for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
                if (!s_voices[i].is_music) s_voices[i].active = false;
            }
        }
        return;
    }

    if (sfx_id == -2) {
        /* Release loop on channel */
        if (channel >= 0 && channel < P8_AUDIO_CHANNELS) {
            s_voices[channel].can_loop = false;
        } else if (channel == -1) {
            for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
                s_voices[i].can_loop = false;
            }
        }
        return;
    }

    if (sfx_id < 0 || sfx_id >= 64) return;

    /* Select channel if not specified (-1) */
    if (channel < 0 || channel >= P8_AUDIO_CHANNELS) {
        channel = -1;
        uint8_t mask = s_music_state.playing ? (s_music_state.channel_mask & 0x0F) : 0;

        /* Priority 1: channel not playing anything and NOT reserved by music mask */
        for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
            if ((mask & (1 << i)) != 0) continue;
            if (!s_voices[i].active || s_voices[i].sfx_id == sfx_id) {
                channel = i;
                break;
            }
        }

        /* Priority 2: any channel not playing anything (even if reserved) */
        if (channel == -1) {
            for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
                if (!s_voices[i].active) {
                    channel = i;
                    break;
                }
            }
        }

        /* Priority 3: an active SFX channel not playing music and not reserved */
        if (channel == -1) {
            for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
                if ((mask & (1 << i)) != 0) continue;
                if (!s_voices[i].is_music) {
                    channel = i;
                    break;
                }
            }
        }

        /* Priority 4: any non-music channel */
        if (channel == -1) {
            for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
                if (!s_voices[i].is_music) {
                    channel = i;
                    break;
                }
            }
        }

        /* Fallback: channel 3 */
        if (channel == -1) channel = 3;
    }

    p8_voice_t *v = &s_voices[channel];
    v->sfx_id = sfx_id;
    v->note_idx = (offset >= 0 && offset < 32) ? offset : 0;
    v->offset = v->note_idx;
    v->length = (length > 0 && length <= 32) ? length : 32;
    v->active = true;
    v->is_music = false;
    v->can_loop = true;
    trigger_voice_note(v, v->note_idx);
}

void p8_audio_music(int pattern, int fade_len, int channel_mask) {
    (void)fade_len;
    if (pattern < 0 || pattern >= 64 || !s_music_mem) {
        /* Stop music */
        s_music_state.playing = false;
        s_music_state.pattern = -1;
        for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
            if (s_voices[i].is_music) s_voices[i].active = false;
        }
        return;
    }

    if (channel_mask != 0) {
        s_music_state.channel_mask = channel_mask;
    }

    const uint8_t *pat = s_music_mem + (pattern * 4);
    bool start_flag = (pat[0] & 0x80) != 0;
    if (start_flag || s_music_state.loop_start_pattern < 0) {
        s_music_state.loop_start_pattern = pattern;
    }

    /* Compute duration of this pattern following PICO-8 / Zepto8 rules:
     * - Duration of the first non-looping channel determines pattern length.
     * - If all channels loop, take the maximum duration among looping channels.
     */
    int duration_looping = -1;
    int duration_no_loop = -1;

    for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
        uint8_t n = pat[i];
        if (n & 0x40) continue; /* bit 6 set = channel empty/disabled */
        int sfx_id = n & 0x3F;
        if (sfx_id >= 64 || !s_sfx_mem) continue;

        const uint8_t *sfx = s_sfx_mem + (sfx_id * 68);
        int speed = sfx[65] < 1 ? 1 : sfx[65];
        bool has_loop = (sfx[67] > 0 && sfx[67] > sfx[66]);

        if (has_loop) {
            int sfx_dur = 32 * speed;
            if (sfx_dur > duration_looping) duration_looping = sfx_dur;
        } else {
            int end_time = 32;
            if (sfx[67] == 0 && sfx[66] > 0 && sfx[66] < 32) {
                end_time = sfx[66];
            }
            duration_no_loop = end_time * speed;
            break; /* First non-looping channel determines pattern length! */
        }
    }

    int duration = (duration_no_loop > 0) ? duration_no_loop : duration_looping;
    if (duration <= 0) duration = 32 * 16; /* Default fallback */

    s_music_state.pattern = pattern;
    s_music_state.playing = true;
    s_music_state.sample_counter = 0;
    s_music_state.total_samples = (uint32_t)duration * 183;

    /* Start music SFX on all active channels in this pattern */
    for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
        uint8_t n = pat[i];
        if ((n & 0x40) == 0 && (n & 0x3F) < 64) {
            p8_voice_t *v = &s_voices[i];
            v->sfx_id = n & 0x3F;
            v->note_idx = 0;
            v->offset = 0;
            v->length = 32;
            v->active = true;
            v->is_music = true;
            v->can_loop = true;
            trigger_voice_note(v, 0);
        } else {
            if (s_voices[i].is_music) {
                s_voices[i].active = false;
            }
        }
    }
}

/* Generate balanced wave sample for voice (aligned with Zepto8 reference acoustics) */
static inline int16_t generate_voice_sample(p8_voice_t *v) {
    if (!v->active || v->current_volume == 0) return 0;

    /* High 16 bits of 32-bit phase accumulator gives 0..65535 position within wave cycle */
    uint16_t phase16 = (uint16_t)(v->phase >> 16);
    int32_t raw_sample = 0;

    switch (v->waveform) {
    case 0: /* 0: Triangle ([-14000, +14000]) */
        if (phase16 < 32768) {
            raw_sample = (((int32_t)phase16 * 28000) >> 15) - 14000;
        } else {
            raw_sample = 14000 - (((int32_t)(phase16 - 32768) * 28000) >> 15);
        }
        break;

    case 1: /* 1: Tilted Saw (rises 87.5%, drops 12.5%, [-14000, +14000]) */
        if (phase16 < 57344) {
            raw_sample = ((int32_t)phase16 * 28000 / 57344) - 14000;
        } else {
            raw_sample = 14000 - (((int32_t)(phase16 - 57344) * 28000) >> 13);
        }
        break;

    case 2: /* 2: Sawtooth ([-11000, +11000]) */
        raw_sample = (((int32_t)phase16 * 22000) >> 16) - 11000;
        break;

    case 3: /* 3: Square (50% duty cycle, [-9500, +9500]) */
        raw_sample = (phase16 < 32768) ? 9500 : -9500;
        break;

    case 4: /* 4: Pulse (31.6% duty cycle, [-9500, +9500]) */
        raw_sample = (phase16 < 20709) ? 9500 : -9500;
        break;

    case 5: /* 5: Organ (Fundamental triangle + harmonic, ~[-8500, +8500]) */
        {
            float t = (float)phase16 / 65536.0f;
            float ret = (t < 0.5f) ? (3.0f - fabsf(24.0f * t - 6.0f)) : (1.0f - fabsf(16.0f * t - 12.0f));
            raw_sample = (int32_t)(ret * 2600.0f);
        }
        break;

    case 6: /* 6: Noise (Brownian low-pass noise, [-9500, +9500]) */
        {
            /* 32-bit XorShift PRNG */
            v->noise_seed ^= v->noise_seed << 13;
            v->noise_seed ^= v->noise_seed >> 17;
            v->noise_seed ^= v->noise_seed << 5;
            float r = (float)(int32_t)v->noise_seed / 2147483648.0f; /* [-1.0, +1.0] */

            /* Cutoff frequency scaled with note pitch */
            float advance = (float)v->phase_inc / 4294967296.0f;
            float scale = advance * 8.858923f;
            v->noise_sample = (v->noise_sample + scale * r) / (1.0f + scale);

            float factor = 1.0f - (float)(v->pitch < 64 ? v->pitch : 63) / 63.0f;
            float ret = v->noise_sample * 1.5f * (1.0f + factor * factor);
            raw_sample = (int32_t)(ret * 9500.0f);
            if (raw_sample > 9500) raw_sample = 9500;
            else if (raw_sample < -9500) raw_sample = -9500;
        }
        break;

    case 7: /* 7: Phaser (Two detuned triangle waves, ~[-12500, +12500]) */
        {
            float t = (float)phase16 / 65536.0f;
            float ret = 2.0f - fabsf(8.0f * t - 4.0f);
            float t2 = (float)(v->phase_detuned >> 16) / 65536.0f;
            ret += 1.0f - fabsf(4.0f * t2 - 2.0f);
            raw_sample = (int32_t)(ret * 4200.0f);
        }
        break;

    default:
        raw_sample = 0;
        break;
    }

    /* Scale by voice volume (0..7) */
    return (int16_t)((raw_sample * v->current_volume) / 7);
}

static inline void advance_voice_tick(p8_voice_t *v) {
    if (!v->active) return;

    v->tick_counter++;

    /* Apply effects modulation */
    switch (v->effect) {
    case 1: /* Slide (smooth linear transition from start_phase_inc/prev_vol to target_phase_inc/vol) */
        if (v->ticks_per_note > 0) {
            float t = (float)v->tick_counter / (float)v->ticks_per_note;
            int64_t diff = (int64_t)v->target_phase_inc - (int64_t)v->start_phase_inc;
            v->phase_inc = (uint32_t)((int64_t)v->start_phase_inc + (int64_t)(diff * t));
            int vol_diff = v->volume - v->prev_vol;
            v->current_volume = v->prev_vol + (int)(vol_diff * t);
        }
        break;

    case 2: /* Vibrato (~7.5 Hz subtle vibrato) */
        {
            /* 22050 Hz / 7.5 Hz = 2940 samples cycle; avoids per-sample fmodf/fabsf */
            int32_t cycle = (int32_t)(v->tick_counter % 2940);
            int32_t tri = (cycle < 1470) ? (735 - cycle) : (cycle - 2205); /* [-735, +735] */
            int32_t delta = (int32_t)(((int64_t)v->target_phase_inc * tri) / 49461);
            v->phase_inc = (uint32_t)((int32_t)v->target_phase_inc + delta);
        }
        break;

    case 3: /* Drop (frequency drops to 0 over the note duration) */
        if (v->ticks_per_note > 0) {
            float t = 1.0f - ((float)v->tick_counter / (float)v->ticks_per_note);
            v->phase_inc = (uint32_t)((float)v->target_phase_inc * t);
        }
        break;

    case 4: /* Fade in */
        if (v->ticks_per_note > 0) {
            v->current_volume = (int)((v->volume * v->tick_counter) / v->ticks_per_note);
        }
        break;

    case 5: /* Fade out */
        if (v->ticks_per_note > 0) {
            v->current_volume = (int)(v->volume - (v->volume * v->tick_counter) / v->ticks_per_note);
        }
        break;

    case 6: /* Fast Arpeggio (cycle 4 notes at speed 4) */
        if (v->ticks_per_note > 0 && s_sfx_mem) {
            int step = (v->tick_counter * 4) / v->ticks_per_note;
            int arp_idx = (v->note_idx & ~3) | (step & 3);
            const uint8_t *sfx = s_sfx_mem + (v->sfx_id * 68);
            const uint8_t *nb = sfx + (arp_idx * 2);
            int arp_pitch = ((uint16_t)nb[0] | ((uint16_t)nb[1] << 8)) & 0x3F;
            v->phase_inc = s_pitch_phase_inc[arp_pitch < 64 ? arp_pitch : 63];
        }
        break;

    case 7: /* Slow Arpeggio (cycle 4 notes at speed 8) */
        if (v->ticks_per_note > 0 && s_sfx_mem) {
            int step = (v->tick_counter * 2) / v->ticks_per_note;
            int arp_idx = (v->note_idx & ~3) | (step & 3);
            const uint8_t *sfx = s_sfx_mem + (v->sfx_id * 68);
            const uint8_t *nb = sfx + (arp_idx * 2);
            int arp_pitch = ((uint16_t)nb[0] | ((uint16_t)nb[1] << 8)) & 0x3F;
            v->phase_inc = s_pitch_phase_inc[arp_pitch < 64 ? arp_pitch : 63];
        }
        break;

    default:
        break;
    }

    v->phase += v->phase_inc;
    if (v->waveform == 7) {
        v->phase_detuned += (uint32_t)((uint64_t)v->phase_inc * 109 / 110);
    }

    if (v->tick_counter >= v->ticks_per_note) {
        v->prev_pitch = v->pitch;
        v->prev_vol = v->volume;
        v->tick_counter = 0;
        v->note_idx++;
        if (v->note_idx >= v->offset + v->length || v->note_idx >= 32) {
            /* Check SFX loop */
            const uint8_t *sfx = s_sfx_mem ? s_sfx_mem + (v->sfx_id * 68) : NULL;
            if (sfx && v->can_loop && sfx[66] < sfx[67] && sfx[67] <= 32) {
                v->note_idx = sfx[66];
                trigger_voice_note(v, v->note_idx);
            } else {
                v->active = false;
            }
        } else {
            trigger_voice_note(v, v->note_idx);
        }
    }
}

void p8_audio_update_samples(int num_samples) {
    if (!s_i2s_ready) return;
    if (num_samples <= 0) return;
    if (num_samples > MAX_AUDIO_FRAME_SAMPLES) num_samples = MAX_AUDIO_FRAME_SAMPLES;

    /* 44.1 kHz I2S output: 2x oversampling of 22050 Hz synthesis */
    uint16_t dma_stereo_count = (uint16_t)(num_samples * 2);
    s_i2s_cfg.dma_trans_count = dma_stereo_count;

    if (s_audio_paused || s_master_volume == 0) {
        memset(s_dma_stream, 0, dma_stereo_count * 2 * sizeof(int16_t));
        i2s_dma_write(&s_i2s_cfg, s_dma_stream);
        return;
    }

    for (int s = 0; s < num_samples; s++) {
        /* Advance music pattern sequencer if playing */
        if (s_music_state.playing && s_music_mem) {
            s_music_state.sample_counter++;
            if (s_music_state.sample_counter >= s_music_state.total_samples) {
                s_music_state.sample_counter = 0;
                const uint8_t *pat = s_music_mem + (s_music_state.pattern * 4);
                bool stop = (pat[2] & 0x80) != 0;
                bool loop = (pat[1] & 0x80) != 0;

                if (stop) {
                    p8_audio_music(-1, 0, 0);
                } else if (loop) {
                    /* Jump back to loop start pattern */
                    int next_pat = s_music_state.pattern;
                    while (next_pat > 0) {
                        next_pat--;
                        const uint8_t *p = s_music_mem + (next_pat * 4);
                        if (p[0] & 0x80) break;
                    }
                    p8_audio_music(next_pat, 0, s_music_state.channel_mask);
                } else {
                    int next_pat = s_music_state.pattern + 1;
                    if (next_pat < 64) {
                        p8_audio_music(next_pat, 0, s_music_state.channel_mask);
                    } else {
                        p8_audio_music(s_music_state.loop_start_pattern, 0, s_music_state.channel_mask);
                    }
                }
            }
        }

        int32_t mixed = 0;
        for (int ch = 0; ch < P8_AUDIO_CHANNELS; ch++) {
            p8_voice_t *v = &s_voices[ch];
            if (v->active) {
                mixed += generate_voice_sample(v);
                advance_voice_tick(v);
            }
        }

        /* Soft-knee saturation: smooth musical compression avoiding harsh digital clipping */
        if (mixed > 24000) {
            mixed = 24000 + ((mixed - 24000) * 8767) / (8767 + (mixed - 24000));
        } else if (mixed < -24000) {
            int32_t neg = -mixed;
            mixed = -(24000 + ((neg - 24000) * 8767) / (8767 + (neg - 24000)));
        }
        if (mixed > 32767) mixed = 32767;
        else if (mixed < -32768) mixed = -32768;

        int16_t sample = (int16_t)mixed;

        /* Write 2x samples for 44.1 kHz playback from 22.05 kHz synthesis */
        int out_idx = s * 4;
        s_dma_stream[out_idx + 0] = sample; /* Left (sample 1) */
        s_dma_stream[out_idx + 1] = sample; /* Right (sample 1) */
        s_dma_stream[out_idx + 2] = sample; /* Left (sample 2) */
        s_dma_stream[out_idx + 3] = sample; /* Right (sample 2) */
    }

    i2s_dma_write(&s_i2s_cfg, s_dma_stream);
}

/* Resume intro chime (dmg_boot_intro.c, core 1). i2s_dma_write() does not
 * block: it drops what does not fit in the ring, so the intro gets the real
 * free space and pushes one sample at a time (a fixed answer here lost most of
 * the chime and changed its tone). Only used before a cart runs. */
int p8_audio_intro_free(void) {
    return s_i2s_ready ? (int)i2s_ring_free() : 0;
}
void p8_audio_intro_push(int16_t l, int16_t r) {
    if (s_i2s_ready) i2s_ring_push(&s_i2s_cfg, l, r);
}
/* After the intro: ring and DMA as p8_audio_init() leaves them. */
void p8_audio_intro_restore(void) {
    if (s_i2s_ready) i2s_audio_start(&s_i2s_cfg);
}

void p8_audio_update(void) {
    p8_audio_update_samples(BUFFER_SAMPLES);
}

void p8_audio_set_volume(uint8_t volume) {
    if (volume > 5) volume = 5;
    s_master_volume = volume;
    if (s_i2s_ready) {
        i2s_volume(&s_i2s_cfg, volume);
    }
}

uint8_t p8_audio_get_volume(void) {
    return s_master_volume;
}

void p8_audio_pause(void) {
    s_audio_paused = true;
}

void p8_audio_resume(void) {
    s_audio_paused = false;
}

void p8_audio_reset(void) {
    for (int i = 0; i < P8_AUDIO_CHANNELS; i++) {
        s_voices[i].active = false;
        s_voices[i].is_music = false;
        s_voices[i].can_loop = true;
    }
    s_music_state.playing = false;
    s_music_state.pattern = -1;
    s_music_state.loop_start_pattern = 0;
    s_music_state.channel_mask = 0;
    s_music_state.sample_counter = 0;
}

size_t p8_audio_get_state_size(void) {
    return sizeof(s_voices) + sizeof(s_music_state);
}

void p8_audio_get_state(void *dest) {
    if (!dest) return;
    uint8_t *p = (uint8_t *)dest;
    memcpy(p, s_voices, sizeof(s_voices));
    p += sizeof(s_voices);
    memcpy(p, &s_music_state, sizeof(s_music_state));
}

void p8_audio_set_state(const void *src) {
    if (!src) return;
    const uint8_t *p = (const uint8_t *)src;
    memcpy(s_voices, p, sizeof(s_voices));
    p += sizeof(s_voices);
    memcpy(&s_music_state, p, sizeof(s_music_state));
}
