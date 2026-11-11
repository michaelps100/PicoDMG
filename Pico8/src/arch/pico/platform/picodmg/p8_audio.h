#ifndef P8_AUDIO_H
#define P8_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P8_AUDIO_SAMPLE_RATE 22050u
#define P8_AUDIO_SAMPLES_PER_FRAME (P8_AUDIO_SAMPLE_RATE / 30u) /* 735 samples @ 30 FPS */
#define P8_AUDIO_CHANNELS 4u

/* Initialize I2S hardware and audio synthesizer */
void p8_audio_init(void);

/* Feed MMIO pointers for SFX (0x3200) and Music (0x3100) from PICO-8 RAM */
void p8_audio_set_memory(const uint8_t *sfx_mem, const uint8_t *music_mem);

/* PICO-8 API triggers */
void p8_audio_sfx(int sfx_id, int channel, int offset, int length);
void p8_audio_music(int pattern, int fade_len, int channel_mask);

/* Process one frame of audio (synthesizes samples and sends to I2S DMA) */
void p8_audio_update(void);
void p8_audio_update_samples(int num_samples);

/* Master Volume control (0..5: 0=Mute, 5=Max) */
void p8_audio_set_volume(uint8_t volume);
uint8_t p8_audio_get_volume(void);

/* Pause / Resume audio playback */
void p8_audio_pause(void);
void p8_audio_resume(void);

/* Reset all audio channels */
void p8_audio_reset(void);

/* Standby-resume intro chime (shared_menu/dmg_boot_intro.c). */
int  p8_audio_intro_free(void);
void p8_audio_intro_push(int16_t l, int16_t r);
void p8_audio_intro_restore(void);

/* Audio state serialization for savestates */
size_t p8_audio_get_state_size(void);
void p8_audio_get_state(void *dest);
void p8_audio_set_state(const void *src);

#ifdef __cplusplus
}
#endif

#endif /* P8_AUDIO_H */
