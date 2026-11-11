#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "platform.h"
#include "core_state.h"
#include <string.h>

typedef struct {
    void* fn;
    bool withArg;
    uint8_t optionalArg;
    int delayInColorClocks;
} DWEntry;

/* Queue depth for the TIA's comb-delayed register writes.
 *
 * This was 5, and `write_after_delay()` silently discards the write when the
 * queue is full (`if (i == -1) return;`). A kernel that issues a burst of TIA
 * register writes in one HBLANK -- which is exactly what a fine-scrolling game
 * does, e.g. PF0/PF1/PF2/GRP0/GRP1/HMOVE together for the Enduro road scroll --
 * overflows 5 easily, and a dropped PF* is a permanently wrong scanline: the
 * picture looks scrambled or the scroll jumps, with no error anywhere.
 *
 * 32 leaves ample headroom for any realistic kernel burst. The cost is
 * (32 * 12) bytes of .bss plus a slightly longer linear scan, and the scan is
 * short in practice because the first free slot is usually near the front. */
#define DWLEN 32
static DWEntry dw_list[DWLEN];
int s_dw_active = 0;
static uint32_t s_pending;

/* Register IDs make the queue portable across builds: never serialize function
 * pointers into a .ss file. All delayed TIA writes enter through memory.c. */
static const struct { void *fn; bool arg; uint8_t reg; } functions[] = {
    {video_VBLANK_write,true,0x01},{video_REFP0_write,true,0x0b},
    {video_REFP1_write,true,0x0c},{video_PF0_write,true,0x0d},
    {video_PF1_write,true,0x0e},{video_PF2_write,true,0x0f},
    {video_RESP0_write,false,0x10},{video_RESP1_write,false,0x11},
    {video_RESM0_write,false,0x12},{video_RESM1_write,false,0x13},
    {video_RESBL_write,false,0x14},{video_GRP0_write,true,0x1b},
    {video_GRP1_write,true,0x1c},{video_ENAM0_write,true,0x1d},
    {video_ENAM1_write,true,0x1e},{video_ENABL_write,true,0x1f},
    {video_HMP0_write,true,0x20},{video_HMP1_write,true,0x21},
    {video_HMM0_write,true,0x22},{video_HMM1_write,true,0x23},
    {video_HMBL_write,true,0x24},{video_HMOVE_write,false,0x2a},
    {video_HMCLR_write,false,0x2b}
};
void delayed_writer_reset(void) { memset(dw_list,0,sizeof(dw_list)); s_dw_active=0; s_pending=0; }
void delayed_writer_export(A26DelayedState *s) {
    memset(s,0,sizeof(*s));
    for(int i=0;i<DWLEN;++i) if(dw_list[i].fn)
        for(unsigned j=0;j<sizeof(functions)/sizeof(functions[0]);++j)
            if(functions[j].fn==dw_list[i].fn) {
                s->entries[i]=(A26DelayedEntry){functions[j].reg,dw_list[i].optionalArg,
                    (uint8_t)dw_list[i].delayInColorClocks,1}; break;
            }
}
bool delayed_writer_validate(const A26DelayedState *s) {
    for(int i=0;i<DWLEN;++i) {
        if(s->entries[i].active>1 || s->entries[i].delay>6) return false;
        if(!s->entries[i].active) continue;
        bool found=false;
        for(unsigned j=0;j<sizeof(functions)/sizeof(functions[0]);++j)
            if(functions[j].reg==s->entries[i].reg) found=true;
        if(!found) return false;
    }
    return true;
}
void delayed_writer_import(const A26DelayedState *s) {
    delayed_writer_reset();
    for(int i=0;i<DWLEN;++i) if(s->entries[i].active)
        for(unsigned j=0;j<sizeof(functions)/sizeof(functions[0]);++j)
            if(functions[j].reg==s->entries[i].reg) {
                dw_list[i]=(DWEntry){functions[j].fn,functions[j].arg,s->entries[i].value,s->entries[i].delay};
                ++s_dw_active; break;
            }
    for(int i=0;i<DWLEN;++i) if(dw_list[i].fn) s_pending|=1u<<i;
}

typedef void(FnWithoutArg)(void);
typedef void(FnWithArg)(uint8_t);

static inline int find_an_empty_spot(void) {
    uint32_t free_slots=~s_pending;
    return free_slots ? __builtin_ctz(free_slots) : -1;
}

void __not_in_flash_func(write_after_delay)(void* fn, bool withArg, uint8_t optionalArg, int delayInColorClocks) {
    int i = find_an_empty_spot();
    if (i == -1) return;

    dw_list[i].fn = fn;
    dw_list[i].withArg = withArg;
    dw_list[i].optionalArg = optionalArg;
    dw_list[i].delayInColorClocks = delayInColorClocks;
    s_pending |= 1u<<i;
    s_dw_active++;
}

void __not_in_flash_func(tick_delayed_writes)(void) {
    if (s_dw_active <= 0) return;

    /* Keep slot order and the original post-decrement timing. Do not scan
     * 32 empty slots for every colour clock of each pending register write. */
    uint32_t pending=s_pending;
    while (pending) {
        unsigned i=__builtin_ctz(pending);
        pending &= pending-1;
        if (dw_list[i].delayInColorClocks--) continue;

        a26_video_sync(); /* the write lands now; render under the old state */
        if (dw_list[i].withArg) {
            ((FnWithArg*)dw_list[i].fn)(dw_list[i].optionalArg);
        } else {
            ((FnWithoutArg*)dw_list[i].fn)();
        }

        dw_list[i].fn = NULL;
        s_pending &= ~(1u<<i);
        s_dw_active--;
    }
}
