#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "cpuexec.h"
#include "fxemu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SuperFX Stubs */
void S9xResetSuperFX(void) {}
void S9xSuperFXExec(void) {}
void FxFlushCache(void) {}
void FxCacheWriteAccess(uint16 Address) { (void)Address; }
void FxReset(FxInit_s *psFxInfo) { (void)psFxInfo; }
int FxEmulate(uint32 nInstructions) { (void)nInstructions; return 0; }

/* Cx4 Stubs */
void S9xInitC4(void) {}
void S9xSetC4(uint8 Byte, uint16 Address) { (void)Byte; (void)Address; }
uint8 S9xGetC4(uint16 Address) { (void)Address; return 0; }
void S9xSetC4RAM(uint8 Byte, uint16 Address) { (void)Byte; (void)Address; }
uint8 S9xGetC4RAM(uint16 Address) { (void)Address; return 0; }

/* DSP1/2/3/4 Stubs */
void S9xResetDSP1(void) {}
void DSP1SetByte(uint8 byte, uint16 address) { (void)byte; (void)address; }
uint8 DSP1GetByte(uint16 address) { (void)address; return 0; }
void DSP2SetByte(uint8 byte, uint16 address) { (void)byte; (void)address; }
uint8 DSP2GetByte(uint16 address) { (void)address; return 0; }

uint8 S9xGetDSP(uint16 Address) { (void)Address; return 0; }
void S9xSetDSP(uint8 Byte, uint16 Address) { (void)Byte; (void)Address; }

void (*SetDSP)(uint8, uint16) = S9xSetDSP;
uint8 (*GetDSP)(uint16) = S9xGetDSP;

/* S-DD1 Stubs */
void S9xSetSDD1MemoryMap(uint32 bank, uint32 value) { (void)bank; (void)value; }
void S9xResetSDD1(void) {}
void S9xSDD1PostLoadState(void) {}

/* S-RTC Stubs */
void S9xSetSRTC(uint8 data, uint16 Address) { (void)data; (void)Address; }
uint8 S9xGetSRTC(uint16 Address) { (void)Address; return 0; }
void S9xSRTCPreSaveState(void) {}
void S9xSRTCPostLoadState(void) {}
void S9xResetSRTC(void) {}
void S9xHardResetSRTC(void) {}

#ifdef __cplusplus
}
#endif
