// Execute the actual diagnostic code; only the hardware/display are replaced.
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <sys/mman.h>
#define PCE_DIAGNOSTICS 1
#include "../../pce_diag.cpp"

static test_watchdog_t fake_watchdog;
static test_scb_t fake_scb;
test_watchdog_t *watchdog_hw = &fake_watchdog;
test_scb_t *scb_hw = &fake_scb;
static bool reset_by_watchdog, enabled, throw_on_fault;
static unsigned core, feeds, screens, waits, delay_ms;
static std::vector<std::string> lines;
struct simulated_reset {};

bool watchdog_enable_caused_reboot() { return reset_by_watchdog; }
void watchdog_enable(uint32_t ms, bool) {
    enabled = true;
    delay_ms = ms;
    watchdog_hw->scratch[4] = 0x6ab73121u;
    if (throw_on_fault) throw simulated_reset{};
}
void watchdog_disable() { enabled = false; }
void watchdog_update() { ++feeds; }
unsigned get_core_num() { return core; }
void tight_loop_contents() { assert(false && "fault capture failed to request reset"); }
void sleep_ms(uint32_t ms) { assert(ms == 10000); ++waits; }
void ili9341_fill_screen(uint16_t) { ++screens; lines.clear(); }
void ili9341_flush(bool) {}
void ili9341_text(const char *s, uint16_t x, uint16_t y, uint16_t, uint16_t) {
    assert(x >= 45 && x + std::strlen(s) * 8 <= 311);
    assert(y + 8 <= 240);
    lines.emplace_back(s);
}
static bool contains(const char *s) {
    for (const auto &line : lines) if (line.find(s) != std::string::npos) return true;
    return false;
}
static void fresh() {
    std::memset(&s_record, 0xa5, sizeof(s_record));
    std::memset(&fake_watchdog, 0, sizeof(fake_watchdog));
    std::memset(&fake_scb, 0, sizeof(fake_scb));
    fake_watchdog.scratch[5] = 0x55443322;
    fake_watchdog.scratch[6] = 0xB007ED01;
    fake_watchdog.scratch[7] = 0;
    reset_by_watchdog = enabled = throw_on_fault = false;
    core = feeds = screens = waits = delay_ms = 0;
    lines.clear();
    s_armed = s_faulted = false;
    pce_diag_boot();
}
static void handshake_untouched() {
    assert(fake_watchdog.scratch[5] == 0x55443322);
    assert(fake_watchdog.scratch[6] == 0xB007ED01);
    assert(fake_watchdog.scratch[7] == 0);
}
static void fault(uint32_t *frame, uint32_t exc_return = 0xfffffff9u) {
    throw_on_fault = true;
    try { pce_diag_fault_c(frame, exc_return); }
    catch (const simulated_reset &) {}
    throw_on_fault = false;
    assert(delay_ms == 1000 && backup_valid() && valid());
}

int main() {
    fresh();
    pce_diag_render_report();
    assert(screens == 0 && waits == 0 && !backup_valid());
    puts("PASS cold boot: no report");

    pce_diag_game_begin();
    assert(enabled && delay_ms == 8000);
    pce_diag_stage(PCE_DIAG_CPU);
    pce_diag_frame_done();
    pce_diag_shutdown();
    pce_diag_render_report();
    assert(screens == 0 && waits == 0 && !enabled && !backup_valid());
    reset_by_watchdog = true; // stale hardware reason must not resurrect exit
    pce_diag_boot();
    pce_diag_render_report();
    assert(screens == 0);
    handshake_untouched();
    puts("PASS intentional exit: no report, retained state cleared");

    fresh();
    pce_diag_game_begin();
    pce_diag_frame_done();
    pce_diag_pause();
    pce_diag_stage(PCE_DIAG_CPU);
    pce_diag_frame_done();
    assert(!enabled && !backup_valid() && feeds == 1);
    pce_diag_resume();
    assert(enabled && backup_valid());
    pce_diag_stage(PCE_DIAG_CORE1_FRAME);
    pce_diag_stage(PCE_DIAG_SECTOR_READ);
    reset_by_watchdog = true;
    pce_diag_boot();
    assert(valid() && s_record.stage == PCE_DIAG_SECTOR_READ && s_record.frames == 1);
    pce_diag_render_report();
    assert(screens == 1 && waits == 1 && contains("WATCHDOG TIMEOUT"));
    assert(contains("CD SECTOR READ") && contains("CORE1 DISPLAY"));
    assert(!contains("CFSR") && !contains("PC 00000000"));
    pce_diag_boot();
    pce_diag_render_report();
    assert(screens == 1 && !backup_valid());
    handshake_untouched();
    puts("PASS pause/resume and watchdog recovery: one report with actual stages");

    fresh();
    pce_diag_game_begin();
    pce_diag_stage(PCE_DIAG_VIDEO_SUBMIT);
    std::memset(&s_record, 0xcc, sizeof(s_record)); // loader/corruption erases SRAM
    reset_by_watchdog = true;
    pce_diag_boot();
    assert(s_record.stage == PCE_DIAG_VIDEO_SUBMIT && (s_record.flags & kPartial));
    pce_diag_render_report();
    assert(contains("VIDEO SUBMIT") && contains("SRAM record lost (partial)"));
    assert(!contains("CFSR"));
    puts("PASS overwritten SRAM: watchdog registers recover stage; missing data labelled");

    fresh();
    pce_diag_game_begin();
    ++fake_watchdog.scratch[2]; // broken checksum, even if tag/stage are intact
    std::memset(&s_record, 0, sizeof(s_record));
    reset_by_watchdog = true;
    pce_diag_boot();
    pce_diag_render_report();
    assert(screens == 0 && !backup_valid());
    puts("PASS corrupt retained data: rejected");

    void *ram = mmap(reinterpret_cast<void *>(kRamStart), kRamEnd - kRamStart,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(ram == reinterpret_cast<void *>(kRamStart));
    auto *frame = reinterpret_cast<uint32_t *>(kRamStart + 0x1000);
    frame[5] = 0x100a2021;
    frame[6] = 0x100a1040;
    fresh();
    pce_diag_game_begin();
    core = 1;
    pce_diag_stage(PCE_DIAG_SECTOR_READ);
    fake_scb = {0x8200, 0x40000000, 0x20090000, 0};
    fault(frame);
    assert(s_record.pc == frame[6] && s_record.lr == frame[5] && s_record.fault_core == 1);
    assert(s_record.fault_stage == PCE_DIAG_SECTOR_READ);
    unsigned before = feeds;
    pce_diag_frame_done();
    pce_diag_stage(PCE_DIAG_CPU);
    assert(feeds == before && s_record.stage == PCE_DIAG_HARDFAULT);
    pce_diag_pause();
    pce_diag_shutdown();
    assert(enabled && delay_ms == 1000 && backup_valid());
    reset_by_watchdog = true;
    pce_diag_boot();
    pce_diag_render_report();
    assert(contains("HARDFAULT CAPTURED") && contains("PC 100A1040 LR 100A2021"));
    assert(contains("CFSR 00008200 HFSR 40000000"));
    assert(contains("C0: CD SECTOR READ"));
    handshake_untouched();
    puts("PASS core1 HardFault: real frame retained; other core cannot feed watchdog");

    fresh();
    pce_diag_game_begin();
    fault(frame);
    std::memset(&s_record, 0, sizeof(s_record));
    reset_by_watchdog = true;
    pce_diag_boot();
    pce_diag_render_report();
    assert(contains("PC 100A1040 LR 100A2021") && contains("SRAM record lost (partial)"));
    assert(!contains("CFSR"));
    puts("PASS HardFault with overwritten SRAM: PC/LR fallback, no fabricated registers");

    fresh();
    pce_diag_game_begin();
    // Arm Cortex-M33 DUG Figure 2-6: the integer frame starts at SP even
    // with FP storage; S0..S15 follow xPSR, not the other way around.
    frame[5] = 0x100a3001;
    frame[6] = 0x100a3020;
    frame[18 + 5] = 0x112a4898; // FP slots that R2 wrongly interpreted as LR/PC
    frame[18 + 6] = 0;
    fault(frame, 0xffffffe9u);
    assert(s_record.pc == frame[6] && s_record.lr == frame[5]);
    assert(s_record.stacked[6] == frame[6] && s_record.stacked[7] == frame[7]);
    puts("PASS floating-point extended exception frame");

    fresh();
    pce_diag_game_begin();
    fault(frame, 0xffffffc9u); // unsupported additional security context
    assert(s_record.flags & kBadFrame);
    puts("PASS unsupported security context: rejected rather than decoded incorrectly");

    const uintptr_t bad[] = {0, kRamStart - 4, kRamEnd - 4, kRamEnd + 16, kRamStart + 1};
    for (uintptr_t address : bad) {
        fresh();
        pce_diag_game_begin();
        fault(reinterpret_cast<uint32_t *>(address));
        assert((s_record.flags & kBadFrame) && s_record.pc == 0 && s_record.lr == 0);
    }
    fresh();
    pce_diag_game_begin();
    fake_scb.cfsr = 1u << 20; // stack overflow, frame in SRAM but unreliable
    fault(frame);
    assert((s_record.flags & kBadFrame) && s_record.pc == 0);
    puts("PASS invalid/out-of-bounds/unaligned and failed-stacking frames: never read");

    std::memset(&s_record, 0, sizeof(s_record));
    reset_by_watchdog = true;
    pce_diag_boot();
    assert(s_record.flags & kBadFrame);
    pce_diag_render_report();
    assert(contains("Exception frame unreadable") && !contains("PC 00000000"));
    puts("PASS unreadable frame with lost SRAM: unavailable PC/LR remain labelled");

    fresh();
    pce_diag_game_begin();
    pce_diag_vram_dma(0x9000, 0x100, 0x7000);
    assert(valid() && (s_record.flags & kVramRange) && s_record.dma_source == 0x9000);
    reset_by_watchdog = false; // backup also survives an interrupted launch path
    pce_diag_boot();
    pce_diag_render_report();
    assert(contains("INTERRUPTED RUN") && contains("VRAM RANGE EXCEEDED"));
    handshake_untouched();
    puts("PASS interrupted run and invalid VRAM DMA: retained actual inputs");
    munmap(ram, kRamEnd - kRamStart);
    puts("All diagnostic recovery tests passed.");
}
