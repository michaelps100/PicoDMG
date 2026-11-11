#pragma once

#include <stdint.h>

typedef enum {
    PCE_DIAG_BOOT = 1,
    PCE_DIAG_GAME_START,
    PCE_DIAG_FRAME_BEGIN,
    PCE_DIAG_CPU,
    PCE_DIAG_VRAM_DMA,
    PCE_DIAG_ADPCM_DMA,
    PCE_DIAG_SECTOR_READ,
    PCE_DIAG_AUDIO,
    PCE_DIAG_VIDEO_SUBMIT,
    PCE_DIAG_CORE1_FRAME,
    PCE_DIAG_FRAME_DONE,
    PCE_DIAG_HARDFAULT,
    PCE_DIAG_WATCHDOG,
    PCE_DIAG_CORE1_DONE,
} pce_diag_stage_t;

#ifdef __cplusplus
extern "C" {
#endif
void pce_diag_boot(void);
void pce_diag_game_begin(void);
void pce_diag_pause(void);
void pce_diag_resume(void);
void pce_diag_shutdown(void);
void pce_diag_stage(pce_diag_stage_t stage);
void pce_diag_frame_done(void);
void pce_diag_vram_dma(uint32_t source, uint32_t dest, uint32_t words);
void pce_diag_render_report(void);
void pce_diag_fault_c(uint32_t *exception_frame, uint32_t exc_return);
#ifdef __cplusplus
}
#endif
