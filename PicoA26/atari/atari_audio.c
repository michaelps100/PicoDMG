/* PCM synthesis is Ron Fries' TiaSound from MCUME_pico2/picovcs.
 * See third_party/mcume_picovcs/UPSTREAM.txt and Tiasound.c for provenance/license.
 * The reference runs at its native rate. 44.1 kHz I2S sampling remains separate. */
#include "atari_audio.h"
#include "core2600/platform.h"
#include <string.h>
extern void Tia_sound_init(unsigned short native_hz,unsigned short output_hz);
extern void Update_tia_sound(unsigned short reg,unsigned char value);
extern void Tia_process_2(unsigned short *samples,unsigned short count);
extern void a26_reference_sound_save(uint8_t *dest);
extern bool a26_reference_sound_valid(const uint8_t *src);
extern void a26_reference_sound_load(const uint8_t *src);
int nOptions_SoundOn=1;
void emu_sndInit(void) {}
void emu_sndPlaySound(int channel,int volume,int frequency) { (void)channel; (void)volume; (void)frequency; }
static atari_audio_state_t state;
void atari_audio_reset(void) {
    memset(&state,0,sizeof(state));
    // Native-to-native avoids the old fixed-point resampler's unsigned
    // underflow for playback rates above the TIA clock. Core schedules /114.
    Tia_sound_init(31399,31399);
    Tia_process_2(&state.native_sample,1); // prime reference sample divider
    state.dc_input=-30720;
}
void atari_audio_init(void) { atari_audio_reset(); }
void A26_RAMFUNC(atari_audio_set_reg)(uint8_t reg,uint8_t value) {
    if(reg<0x15 || reg>0x1a) return;
    Update_tia_sound(reg,value);
    atari_audio_channel_t *c=&state.channels[(reg-0x15)&1];
    if(reg<0x17) c->audc=value&15;
    else if(reg<0x19) c->audf=value&31;
    else c->audv=value&15;
}
void __not_in_flash_func(atari_audio_step)(void) { Tia_process_2(&state.native_sample,1); }
int16_t __not_in_flash_func(atari_audio_get_sample)(uint8_t volume) {
    static const int gain[6]={0,51,102,154,205,256};
    // Reference PCM is unsigned. Filter DC before applying user volume.
    int32_t sample=((int32_t)state.native_sample-30720);
    int32_t out=sample-state.dc_input+state.dc_output-state.dc_output/256;
    state.dc_input=sample; state.dc_output=out;
    out=out*gain[volume>5?5:volume]/256;
    if(out>32767) out=32767;
    if(out< -32768) out= -32768;
    return (int16_t)out;
}
void atari_audio_export(atari_audio_state_t *s) {
    memset(state.reference,0,sizeof(state.reference));
    a26_reference_sound_save(state.reference); *s=state;
}
bool atari_audio_validate(const atari_audio_state_t *s) {
    return a26_reference_sound_valid(s->reference) && s->dc_input>=-30720 &&
        s->dc_input<=30720 && s->dc_output>=-100000 && s->dc_output<=100000;
}
void atari_audio_import(const atari_audio_state_t *s) {
    state=*s; a26_reference_sound_load(s->reference);
}
