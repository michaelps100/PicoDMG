/**
 * \file           lwmem_opts.h
 * \brief          lwmem configuration for the PicoGBC / PicoDMG console build.
 *
 * Minimal: the emulator only needs lwmem for two things on a PSRAM board --
 * handing the PSRAM window to the allocator (PicoPlusPsram's constructor calls
 * lwmem_assignmem) and allocating the cartridge ROM. That is a handful of
 * allocations at boot, so the optional features lwmem offers (statistics,
 * defrag, thread safety) are all left off.
 *
 * Without this header lwmem falls back to its own default opts, which assume a
 * 16 MHz / 280 MHz-class Cortex-M with a 64-byte alignment and would report
 * the wrong defaults for the RP2350B.
 */

#ifndef LWMEM_OPTS_H
#define LWMEM_OPTS_H

/* Statistics cost a few hundred bytes of BSS and a small amount of per-alloc
 * work. Nothing in this project reads them, so they stay off. */
#define LWMEM_CFG_ENABLE_STATS 0

/* No threads in the emulator, and the ROM load happens before core 1 starts
 * drawing, so there is no allocator concurrency to protect against. */
#define LWMEM_CFG_ENABLE_THREAD_PROTECTION 0
#define LWMEM_CFG_ENABLE_MUTEX 0
#define LWMEM_CFG_ENABLE_SEMAPHORE 0
#define LWMEM_CFG_ENABLE_MSGQUEUE 0

/* The ROM is read from SD in one linear pass and then only read by the
 * emulator, so reallocating to coalesce it buys nothing. */
#define LWMEM_CFG_ENABLE_DEFRAG 0

/* PSRAM is mapped at 0x11000000 by SetupPsram and needs 4-byte alignment for
 * the 16-bit pixel and the 32-bit DMA transfers the display does from it. */
#define LWMEM_CFG_ALIGN_SIZE 4

/* Leave the original (unmoved) blocks alone; there is no need to compact the
 * heap after a failed large allocation. */
#define LWMEM_CFG_REALLOC_ENCODE_SIZE 0

#endif /* LWMEM_OPTS_H */
