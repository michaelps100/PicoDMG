/**
 * ws_audio.h - WonderSwan Sound Emulation for PicoWS
 *
 * Emulates the 4-channel WonderSwan / WonderSwan Color audio hardware:
 *   - Channel 0: 32-sample wavetable
 *   - Channel 1: 32-sample wavetable OR Voice / Direct PCM
 *   - Channel 2: 32-sample wavetable with hardware Frequency Sweep
 *   - Channel 3: 32-sample wavetable OR 15-bit LFSR Noise
 *   - Sound DMA support (PCM streaming to Ch1 or HyperVoice)
 *   - Clean 44.1 kHz stereo output to MAX98357A via I2S
 */

#ifndef __WS_AUDIO_H__
#define __WS_AUDIO_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle */
void ws_audio_init(void);
void ws_audio_reset(void);
void ws_audio_done(void);

/* I/O port handlers (ports 0x80 .. 0x95, 0x6A, 0x6B) */
void    ws_audio_port_write(uint32_t port, uint8_t value);
uint8_t ws_audio_port_read(uint32_t port);

/* Scanline audio generation (called for each scanline 0..158) */
void ws_audio_process_line(int line);

/* Output samples per emulated frame: 735 (44100 / 60) normally, more to stretch the
 * audio of frames that run late. Takes effect at the next line 0. */
void ws_audio_set_samples_per_frame(int n);

/* Volume & state sync */
void ws_audio_set_volume(uint8_t volume_level);
void ws_audio_sync_from_ioram(void);

#ifdef __cplusplus
}
#endif

#endif /* __WS_AUDIO_H__ */
