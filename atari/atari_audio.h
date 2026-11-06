#ifndef ATARI_AUDIO_H
#define ATARI_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t audc, audf, audv, divider, poly4, poly5, count, out;
    uint16_t poly9;
} atari_audio_channel_t;
typedef struct {
    atari_audio_channel_t channels[2]; /* register summary, not synthesis state */
    uint8_t reference[544];
    int32_t dc_input, dc_output;
    uint16_t native_sample;
} atari_audio_state_t;
void atari_audio_export(atari_audio_state_t *state);
bool atari_audio_validate(const atari_audio_state_t *state);
void atari_audio_import(const atari_audio_state_t *state);
void atari_audio_init(void);
void atari_audio_reset(void);
void atari_audio_set_reg(uint8_t reg, uint8_t value);
void atari_audio_step(void);
int16_t atari_audio_get_sample(uint8_t volume_level);

#ifdef __cplusplus
}
#endif

#endif /* ATARI_AUDIO_H */
