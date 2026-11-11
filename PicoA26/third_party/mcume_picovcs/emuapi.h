/* PicoA26 only uses the original PCM synthesizer, not MCUME's tone mixer. */
#ifndef PICOA26_MCUME_EMUAPI_H
#define PICOA26_MCUME_EMUAPI_H
#include <stdlib.h>
#include <string.h>
void emu_sndInit(void);
void emu_sndPlaySound(int channel, int volume, int frequency);
#endif
