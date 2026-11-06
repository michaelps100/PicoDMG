#include "pce_diag.h"

#if PCE_DIAGNOSTICS
#include <stddef.h>
#include <stdio.h>
#include "hardware/structs/scb.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "ili9341.h"

namespace {
constexpr uint32_t kMagic = 0x50434447u;
constexpr uint32_t kVersion = 3;
constexpr uint32_t kBackupTag = 0x50430000u;
constexpr uint32_t kBackupVersion = 0x20000000u;
constexpr uint32_t kWatchdogReset = 1u;
constexpr uint32_t kPending = 2u;
constexpr uint32_t kVramRange = 4u;
constexpr uint32_t kPartial = 8u;
constexpr uint32_t kInterrupted = 16u;
constexpr uint32_t kBadFrame = 32u;
constexpr uintptr_t kRamStart = 0x20000000u;
constexpr uintptr_t kRamEnd = 0x20082000u;
constexpr uint32_t kStackingErrors = (1u << 4) | (1u << 5) | (1u << 12) |
                                    (1u << 13) | (1u << 20);

struct record_t {
    uint32_t magic, version, checksum, stage, flags;
    uint32_t pc, lr, cfsr, hfsr, bfar, mmfar;
    uint32_t dma_source, dma_dest, dma_words, core1_stage;
    uint32_t frames, fault_core, frame_sp, exc_return;
    uint32_t fault_stage, stacked[8]; // R0-R3, R12, LR, PC, xPSR
};

// Not the SDK's generic low-SRAM .uninitialized_data: the loader overwrites
// that address. The linker reserves this NOLOAD bank below the main stack.
static record_t s_record __attribute__((section(".pce_diag_retained")));
static bool s_armed;
static bool s_faulted;
static_assert(sizeof(record_t) <= 256, "diagnostic SRAM reservation too small");

uint32_t checksum(const record_t &r) {
    const uint32_t *p = reinterpret_cast<const uint32_t *>(&r);
    uint32_t v = 0x9E3779B9u;
    for (unsigned i = 0; i < sizeof(r) / sizeof(uint32_t); ++i) {
        if (i == offsetof(record_t, checksum) / 4 ||
            i == offsetof(record_t, core1_stage) / 4) continue;
        v = (v << 5) | (v >> 27);
        v ^= p[i] + 0x7F4A7C15u;
    }
    return v;
}

bool valid() {
    return s_record.magic == kMagic && s_record.version == kVersion &&
           s_record.checksum == checksum(s_record);
}

void commit() {
    s_record.magic = kMagic;
    s_record.version = kVersion;
    s_record.checksum = checksum(s_record);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint32_t backup_signature(uint32_t stage_word, uint32_t pc, uint32_t lr) {
    uint32_t v = (stage_word ^ 0x9E3779B9u) * 16777619u;
    v = (v ^ pc) * 16777619u;
    v = (v ^ lr) * 16777619u;
    return kBackupTag | ((v ^ (v >> 16)) & 0xFFFFu);
}

bool backup_valid() {
    uint32_t stage_word = watchdog_hw->scratch[1];
    uint32_t stage = stage_word & 0xFFu;
    return (stage_word & 0xFFFFFC00u) == kBackupVersion &&
           stage >= PCE_DIAG_BOOT && stage <= PCE_DIAG_CORE1_DONE &&
           watchdog_hw->scratch[0] == backup_signature(stage_word,
               watchdog_hw->scratch[2], watchdog_hw->scratch[3]);
}

void retain_breadcrumb() {
    // The SDK/loader own scratch[4..7]; never alter their launch handshake.
    uint32_t stage_word = kBackupVersion | s_record.stage |
        ((s_record.flags & kBadFrame) ? 0x100u : 0u) |
        ((s_record.fault_core & 1u) << 9);
    watchdog_hw->scratch[0] = 0;
    watchdog_hw->scratch[1] = stage_word;
    watchdog_hw->scratch[2] = s_record.pc;
    watchdog_hw->scratch[3] = s_record.lr;
    watchdog_hw->scratch[0] = backup_signature(stage_word, s_record.pc, s_record.lr);
}

void clear_report() {
    s_record.magic = 0;
    s_record.checksum = 0;
    for (unsigned i = 0; i < 4; ++i) watchdog_hw->scratch[i] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

bool armed() {
    return __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&s_faulted, __ATOMIC_ACQUIRE);
}

const char *stage_name(uint32_t s) {
    switch (s) {
    case PCE_DIAG_BOOT: return "BOOT";
    case PCE_DIAG_GAME_START: return "GAME START";
    case PCE_DIAG_FRAME_BEGIN: return "FRAME BEGIN";
    case PCE_DIAG_CPU: return "PCE CPU";
    case PCE_DIAG_VRAM_DMA: return "VRAM DMA";
    case PCE_DIAG_ADPCM_DMA: return "ADPCM DMA";
    case PCE_DIAG_SECTOR_READ: return "CD SECTOR READ";
    case PCE_DIAG_AUDIO: return "CD AUDIO";
    case PCE_DIAG_VIDEO_SUBMIT: return "VIDEO SUBMIT";
    case PCE_DIAG_CORE1_FRAME: return "CORE1 DISPLAY";
    case PCE_DIAG_CORE1_DONE: return "CORE1 DONE";
    case PCE_DIAG_FRAME_DONE: return "FRAME COMPLETE";
    case PCE_DIAG_HARDFAULT: return "HARDFAULT";
    case PCE_DIAG_WATCHDOG: return "WATCHDOG RESET";
    default: return "UNAVAILABLE";
    }
}
}

extern "C" void pce_diag_boot(void) {
    s_armed = false;
    s_faulted = false;
    /* Wake from the power-switch standby (shared_menu/dmg_resume.h marks
     * scratch[5] before DORMANT): the run was stopped on purpose, never a
     * crash, so whatever breadcrumb is left must not become a report. */
    if ((watchdog_hw->scratch[5] & 0xFFFFFF00u) == 0x5E5A6A00u) {
        watchdog_disable();
        clear_report();
        return;
    }
    bool full_record = valid();
    bool backup = backup_valid();
    bool watchdog_reset = watchdog_enable_caused_reboot();
    watchdog_disable();
    if (backup || (full_record && watchdog_reset)) {
        if (!full_record) {
            s_record = {};
            s_record.stage = watchdog_hw->scratch[1] & 0xFFu;
            s_record.pc = watchdog_hw->scratch[2];
            s_record.lr = watchdog_hw->scratch[3];
            s_record.flags = kPartial;
            if (watchdog_hw->scratch[1] & 0x100u) s_record.flags |= kBadFrame;
            s_record.fault_core = (watchdog_hw->scratch[1] >> 9) & 1u;
        }
        s_record.flags |= kPending | (watchdog_reset ? kWatchdogReset : kInterrupted);
        commit();
    } else {
        clear_report();
    }
}

extern "C" void pce_diag_game_begin(void) {
    s_record = {};
    s_record.stage = PCE_DIAG_GAME_START;
    s_faulted = false;
    commit();
    __atomic_store_n(&s_armed, true, __ATOMIC_RELEASE);
    retain_breadcrumb();
    watchdog_enable(8000, false);
}

extern "C" void pce_diag_pause(void) {
    if (__atomic_load_n(&s_faulted, __ATOMIC_ACQUIRE)) return;
    watchdog_disable();
    __atomic_store_n(&s_armed, false, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 4; ++i) watchdog_hw->scratch[i] = 0;
}

extern "C" void pce_diag_resume(void) {
    if (__atomic_load_n(&s_faulted, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&s_armed, true, __ATOMIC_RELEASE);
    retain_breadcrumb();
    watchdog_enable(8000, false);
}

extern "C" void pce_diag_shutdown(void) {
    if (__atomic_load_n(&s_faulted, __ATOMIC_ACQUIRE)) return;
    pce_diag_pause();
    clear_report(); // intentional game exit: no diagnostic screen
}

extern "C" void pce_diag_stage(pce_diag_stage_t stage) {
    if (!armed()) return;
    if (stage == PCE_DIAG_CORE1_FRAME || stage == PCE_DIAG_CORE1_DONE) {
        __atomic_store_n(&s_record.core1_stage, (uint32_t)stage, __ATOMIC_RELEASE);
        return;
    }
    s_record.stage = stage;
    commit();
    retain_breadcrumb();
}

extern "C" void pce_diag_frame_done(void) {
    if (!armed()) return;
    ++s_record.frames;
    s_record.stage = PCE_DIAG_FRAME_DONE;
    commit();
    retain_breadcrumb();
    watchdog_update();
}

extern "C" void pce_diag_vram_dma(uint32_t source, uint32_t dest, uint32_t words) {
    if (!armed()) return;
    s_record.stage = PCE_DIAG_VRAM_DMA;
    s_record.dma_source = source;
    s_record.dma_dest = dest;
    s_record.dma_words = words;
    if (source >= 0x8000u || dest >= 0x8000u || words > 0x8000u - source ||
        words > 0x8000u - dest) s_record.flags |= kVramRange;
    commit();
    retain_breadcrumb();
}

extern "C" void pce_diag_render_report(void) {
    if (!valid() || !(s_record.flags & kPending)) return;
    char line[48];
    // x=48 also fits the GB_MODE viewport (x=45, width=266).
    constexpr uint16_t x = 48;
    ili9341_fill_screen(0x0000);
    ili9341_text("PCE DIAGNOSTIC R3", x, 10, 0x7FFF, 0);
    bool fault = s_record.stage == PCE_DIAG_HARDFAULT;
    bool partial = (s_record.flags & kPartial) != 0;
    const char *reason = s_record.stage == PCE_DIAG_HARDFAULT ? "HARDFAULT CAPTURED" :
                         (s_record.flags & kWatchdogReset) ? "WATCHDOG TIMEOUT" : "INTERRUPTED RUN";
    ili9341_text(reason, x, 30, 0x7FE0, 0);
    snprintf(line, sizeof(line), "C0: %s", stage_name(fault && !partial ? s_record.fault_stage : s_record.stage));
    ili9341_text(line, x, 50, 0x03FF, 0);
    snprintf(line, sizeof(line), "C1: %s", stage_name(s_record.core1_stage));
    ili9341_text(line, x, 68, 0x03E0, 0);
    if (fault) {
        if (s_record.flags & kBadFrame)
            snprintf(line, sizeof(line), "Exception frame unreadable");
        else
            snprintf(line, sizeof(line), "PC %08lX LR %08lX", (unsigned long)s_record.pc, (unsigned long)s_record.lr);
        ili9341_text(line, x, 88, 0x7FFF, 0);
        if (!partial) {
            snprintf(line, sizeof(line), "CFSR %08lX HFSR %08lX", (unsigned long)s_record.cfsr, (unsigned long)s_record.hfsr);
            ili9341_text(line, x, 108, 0x7FFF, 0);
            if ((s_record.cfsr & 0x8080u) == 0x8080u)
                snprintf(line, sizeof(line), "BFAR %08lX MM %08lX", (unsigned long)s_record.bfar, (unsigned long)s_record.mmfar);
            else if (s_record.cfsr & 0x8000u)
                snprintf(line, sizeof(line), "BFAR %08lX", (unsigned long)s_record.bfar);
            else if (s_record.cfsr & 0x80u)
                snprintf(line, sizeof(line), "MMFAR %08lX", (unsigned long)s_record.mmfar);
            else
                snprintf(line, sizeof(line), "Fault address unavailable");
            ili9341_text(line, x, 128, 0x7FFF, 0);
            snprintf(line, sizeof(line), "SP %08lX EX %08lX", (unsigned long)s_record.frame_sp, (unsigned long)s_record.exc_return);
            ili9341_text(line, x, 146, 0x7FFF, 0);
            if (!(s_record.flags & kBadFrame)) {
                snprintf(line, sizeof(line), "R0 %08lX R1 %08lX", (unsigned long)s_record.stacked[0], (unsigned long)s_record.stacked[1]);
                ili9341_text(line, x, 164, 0x7FFF, 0);
                snprintf(line, sizeof(line), "R2 %08lX R3 %08lX", (unsigned long)s_record.stacked[2], (unsigned long)s_record.stacked[3]);
                ili9341_text(line, x, 182, 0x7FFF, 0);
            }
            snprintf(line, sizeof(line), "R12 %08lX F %lu C%lu", (unsigned long)s_record.stacked[4], (unsigned long)s_record.frames, (unsigned long)s_record.fault_core);
            ili9341_text(line, x, 200, 0x7FFF, 0);
        }
    } else {
        ili9341_text("No CPU exception captured", x, 88, 0x7FFF, 0);
    }
    if (!partial && !fault) {
        snprintf(line, sizeof(line), "DMA %04lX>%04lX n=%lu", (unsigned long)s_record.dma_source, (unsigned long)s_record.dma_dest, (unsigned long)s_record.dma_words);
        ili9341_text(line, x, 152, 0x7C1F, 0);
        if (fault)
            snprintf(line, sizeof(line), "Frames %lu core %lu", (unsigned long)s_record.frames, (unsigned long)s_record.fault_core);
        else
            snprintf(line, sizeof(line), "Frames %lu", (unsigned long)s_record.frames);
        ili9341_text(line, x, 172, 0x7FFF, 0);
        if (s_record.flags & kVramRange)
            ili9341_text("VRAM RANGE EXCEEDED", x, 196, 0x7FE0, 0);
    } else if (partial) {
        ili9341_text("SRAM record lost (partial)", x, 172, 0x7FE0, 0);
        ili9341_text("Stage retained; other data lost", x, 196, 0x7FFF, 0);
    }
    ili9341_text("10 seconds -> game list", x, 220, 0x7FFF, 0);
    ili9341_flush(true);
    sleep_ms(10000);
    clear_report();
}

#if defined(__arm__) || defined(__thumb__)
extern "C" __attribute__((naked, used)) void isr_hardfault(void) {
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b pce_diag_fault_c\n");
}
#endif

extern "C" __attribute__((noreturn)) void pce_diag_fault_c(uint32_t *frame, uint32_t exc_return) {
    // A fault on core1 must also stop core0 feeding the watchdog.
    __atomic_store_n(&s_faulted, true, __ATOMIC_RELEASE);
    if (!valid()) s_record = {};
    s_record.fault_stage = s_record.stage;
    s_record.stage = PCE_DIAG_HARDFAULT;
    s_record.pc = s_record.lr = 0;
    s_record.cfsr = scb_hw->cfsr;
    s_record.hfsr = scb_hw->hfsr;
    s_record.bfar = scb_hw->bfar;
    s_record.mmfar = scb_hw->mmfar;
    s_record.fault_core = get_core_num();
    s_record.frame_sp = (uint32_t)reinterpret_cast<uintptr_t>(frame);
    s_record.exc_return = exc_return;
    uintptr_t address = reinterpret_cast<uintptr_t>(frame);
    // Arm Cortex-M33 DUG Figure 2-6: R0..xPSR remain the FIRST eight words.
    // FP storage adds 18 words AFTER xPSR; it never shifts PC/LR by 18 words.
    // This firmware runs Secure code/handlers. Reject additional security
    // context (DCRS=0) rather than misinterpreting an unsupported frame.
    size_t frame_bytes = ((exc_return & (1u << 4)) ? 8u : 26u) * sizeof(uint32_t);
    if ((address & 3u) == 0 && address >= kRamStart &&
        address <= kRamEnd - frame_bytes && !(s_record.cfsr & kStackingErrors) &&
        (exc_return & (1u << 5))) {
        for (unsigned i = 0; i < 8; ++i) s_record.stacked[i] = frame[i];
        s_record.pc = frame[6];
        s_record.lr = frame[5];
    } else {
        s_record.flags |= kBadFrame;
    }
    commit();
    retain_breadcrumb();
    // Keep the watchdog_enable marker used by the loader's resume decision.
    watchdog_enable(1000, false);
    while (true) tight_loop_contents();
}

#else
extern "C" void pce_diag_boot(void) {}
extern "C" void pce_diag_game_begin(void) {}
extern "C" void pce_diag_pause(void) {}
extern "C" void pce_diag_resume(void) {}
extern "C" void pce_diag_shutdown(void) {}
extern "C" void pce_diag_stage(pce_diag_stage_t) {}
extern "C" void pce_diag_frame_done(void) {}
extern "C" void pce_diag_vram_dma(uint32_t, uint32_t, uint32_t) {}
extern "C" void pce_diag_render_report(void) {}
#endif
