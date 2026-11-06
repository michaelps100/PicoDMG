cmake_minimum_required(VERSION 3.13)
set(projectname PicoSNES)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build : None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif()
message("* Current build type is : ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# Hardware / board selection
# ---------------------------------------------------------------------------
if(NOT DEFINED USE_ILI9341)
    set(USE_ILI9341 1 CACHE STRING "Use ILI9341 SPI TFT display instead of DVI/HSTX")
endif()
set(ILI9341_EXTERNAL_FB 1 CACHE BOOL "Use external framebuffer for ILI9341 to conserve SRAM")

if(USE_ILI9341 AND NOT HW_CONFIG)
    set(HW_CONFIG 15 CACHE STRING "Select the hardware configuration for your board")      
elseif(NOT HW_CONFIG)
    set(HW_CONFIG 8 CACHE STRING "Select the hardware configuration for your board")      
endif()

if(NOT DEFINED USE_PSRAM)
    set(USE_PSRAM 0 CACHE STRING "Enable PSRAM support on RP2350 boards with PSRAM (1) or use internal SRAM only (0)")
endif()
# A bare `-DUSE_PSRAM=0` is a CMake false value, but a stray `-DUSE_PSRAM=$var`
# can arrive as the literal string "$var" when the shell does not expand it.
# That string is not a recognised false constant, so CMake evaluates it as TRUE
# and silently switches PSRAM support on for a board that has none. Reject
# anything that is not exactly 0 or 1 rather than trusting the flag.
if(NOT USE_PSRAM MATCHES "^[01]$")
    message(WARNING "USE_PSRAM='${USE_PSRAM}' is not 0 or 1 -- treating it as 0 (no PSRAM).")
    set(USE_PSRAM 0)
endif()
message("PSRAM support: ${USE_PSRAM}")

# ---------------------------------------------------------------------------
# Fallback panel when /boot.txt is absent or has no ST7789= key.
#
# The panel is a runtime choice read by shared_menu/dmg_panel.c, not a build
# choice, so this only decides what a card with no /boot.txt gets. Defaults to 0
# (ILI9341) so the RP2350 Plus build keeps its current display; the HW_CONFIG 16
# console build overrides it with -DDEFAULT_PANEL_ST7789=1.
# ---------------------------------------------------------------------------
if(NOT DEFINED DEFAULT_PANEL_ST7789)
    set(DEFAULT_PANEL_ST7789 0 CACHE STRING
        "Fallback panel when /boot.txt is absent: 0=ILI9341 1=ST7789")
endif()
if(HW_CONFIG EQUAL 16 AND NOT DEFAULT_PANEL_ST7789)
    # The Core2350B console is an ST7789. A card with a missing or ST7789-less
    # /boot.txt would otherwise come up on the ILI9341 driver and show nothing.
    set(DEFAULT_PANEL_ST7789 1 CACHE STRING
        "Fallback panel when /boot.txt is absent: 0=ILI9341 1=ST7789" FORCE)
endif()
message("* Fallback display panel (no ST7789= in /boot.txt): ${DEFAULT_PANEL_ST7789}")

if(NOT USE_HSTX)
    set(USE_HSTX 0 CACHE STRING "Use HSTX for display output when possible")
endif()

include("pico_shared/BoardConfigs.cmake")

if(NOT PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_plus_16mb CACHE STRING "Board type")
    message("PICO_BOARD not set, using default: ${PICO_BOARD}")
endif()

add_definitions(-DNDEBUG -DDVIAUDIOFREQ=32000)

# ---------------------------------------------------------------------------
# TinyUSB family support + Pico SDK init
# ---------------------------------------------------------------------------
set(FAMILY rp2040)
include($ENV{PICO_SDK_PATH}/lib/tinyusb/hw/bsp/family_support.cmake)

if(ENABLE_PIO_USB EQUAL 0 AND TARGET tinyusb_common_base)
    get_target_property(_tu_defs tinyusb_common_base INTERFACE_COMPILE_DEFINITIONS)
    if(_tu_defs)
        list(REMOVE_ITEM _tu_defs "CFG_TUH_RPI_PIO_USB=1")
        set_target_properties(tinyusb_common_base PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${_tu_defs}")
    endif()
endif()

project(${projectname} C CXX ASM)
pico_sdk_init()

if(ENABLE_PIO_USB EQUAL 1)
    message("Checking for PIO USB support...")
    check_and_add_pico_pio_usb_support()
endif()

# ---------------------------------------------------------------------------
# Target Executable Sources
# ---------------------------------------------------------------------------
add_executable(${projectname}
    main.cpp
    splash.cpp
    DefaultSS444.c
    DefaultSS555.c
    EmuOverlay444.c
    EmuOverlay555.c
    state.cpp
    # --- snes9x core: PicoPlus-devel/pico-snesPlus, unmodified -------------
    # The previous core in src/ was an older snes9x 2002 fork with a hand-rolled
    # tile cache (see agents.md 2026-09-18). It loads ROMs and maps banks
    # correctly but renders nothing -- VRAM stayed empty across every diagnostic
    # frame -- so it was replaced rather than patched. The old tree is kept in
    # src_fork_backup/ for reference; nothing in the build reads it.
    snes9x_src/apu.c
    snes9x_src/c4.c
    snes9x_src/c4emu.c
    snes9x_src/clip.c
    snes9x_src/cpu.c
    snes9x_src/cpuexec.c
    snes9x_src/cpuops.c
    snes9x_src/dma.c
    snes9x_src/dsp.c
    snes9x_src/fxemu.c
    snes9x_src/fxinst.c
    snes9x_src/getset.c
    snes9x_src/gfx.c
    snes9x_src/globals.c
    snes9x_src/memmap.c
    snes9x_src/msu1.c
    snes9x_src/obc1.c
    snes9x_src/ppu.c
    snes9x_src/sa1.c
    snes9x_src/sa1cpu.c
    snes9x_src/soundux.c
    snes9x_src/spc700.c
    snes9x_src/srtc.c
    snes9x_src/tile.c
    snes9x_src/port_alloc.cpp
    snes9x_src/port_glue.cpp
    snes9x_src/msu1_port.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_menu.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_panel.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_combo.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_power.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_battery.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_resume.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_boot_intro.c
)

# Load the cartridge into PSRAM (1) or program the XIP flash window (0).
# See the long comment above SNES_LOAD_ROM_IN_PSRAM in main.cpp: the two paths
# are not equivalent, because stores to ROM are dropped on flash and honoured in
# PSRAM. Kept switchable until hardware says which one this core wants.
if(NOT DEFINED SNES_LOAD_ROM_IN_PSRAM)
    set(SNES_LOAD_ROM_IN_PSRAM 1 CACHE STRING
        "Load SNES cartridges into PSRAM (1) or into the flash XIP window (0)")
endif()
if(SNES_LOAD_ROM_IN_PSRAM)
    message("* SNES ROM destination: PSRAM (when present)")
else()
    message("* SNES ROM destination: flash XIP window")
endif()

if(NOT DEFINED PERIPH_CLOCK_DECOUPLE)
    set(PERIPH_CLOCK_DECOUPLE 1 CACHE STRING
        "clk_peri at 140 MHz via PLL_USB, as PicoNGP does on this console (0=leave clk_peri at clk_sys)")
endif()
if(PERIPH_CLOCK_DECOUPLE)
    message("* clk_peri: 140 MHz via PLL_USB, copied from PicoNGP (panel 70 MHz, microSD 17.5 MHz)")
else()
    message("* clk_peri: slaved to clk_sys (360 MHz)")
endif()

# The option above was only a CMake variable: without this it never reached the
# preprocessor, so `#if PERIPH_CLOCK_DECOUPLE` evaluated an undefined identifier
# as 0 and the whole 140 MHz block compiled out. The boot log showed
# clk_peri=360000 while the configure step announced 140 MHz.
# Passed to the preprocessor as LITERAL 0 or 1, not as ${VAR}.
#
# Interpolating the cache variable here produced `SNES_LOAD_ROM_IN_PSRAM=$$` on the
# compile line -- CMake emitted an escaped empty string, the branch compiled out,
# and the "PSRAM" build silently programmed flash instead. PERIPH_CLOCK_DECOUPLE
# on the next line went through the same code and was fine, which is exactly what
# makes this failure mode so easy to repeat: the message at configure time
# announces one thing and the binary does another.
#
# So each option sets its own define from an explicit branch. If a new option is
# added here, do it this way too.
if(SNES_LOAD_ROM_IN_PSRAM)
    target_compile_definitions(${projectname} PUBLIC SNES_LOAD_ROM_IN_PSRAM=1)
else()
    target_compile_definitions(${projectname} PUBLIC SNES_LOAD_ROM_IN_PSRAM=0)
endif()
if(PERIPH_CLOCK_DECOUPLE)
    target_compile_definitions(${projectname} PUBLIC PERIPH_CLOCK_DECOUPLE=1)
else()
    target_compile_definitions(${projectname} PUBLIC PERIPH_CLOCK_DECOUPLE=0)
endif()

# --------------------------------------------------------------------------

# Expansion chips.
#
# Taken from the upstream README's chip table, which is the authoritative list of
# what this core emulates. Everything in the first block is compiled in
# unconditionally because the core has no per-chip build flag -- the chip is
# selected at run time by InitROM() reading the cartridge header, and the code
# is simply linked. That is the whole mechanism, and it is why there is no
# ENABLE_DSP / ENABLE_C4 / ENABLE_SUPERFX here and there never should be.
#
#   sa1.c, sa1cpu.c   SA-1           Super Mario RPG, Kirby Super Star, KD3
#   dsp.c            DSP-1/1B/2/3/4 Super Mario Kart, Pilotwings, SD Gundam GX
#   fxemu.c fxinst.c Super FX (GSU) Yoshi's Island, Stunt Race FX, Doom
#   c4.c c4emu.c     C4             Mega Man X2/X3
#   obc1.c           OBC1           Metal Combat
#   srtc.c           S-RTC          Dai Kaijuu Monogatari II
#
#   NOT emulated, and refused at load time (main.cpp checks Settings.SDD1 /
#   Settings.SPC7110 after LoadROM): S-DD1 (Star Ocean, SF Alpha 2) and
#   SPC7110 (Far East of Eden Zero). SETA (ST010/ST011) and BS-X are also
#   missing but are not detected, so those carts load and misbehave -- an
#   upstream limitation, not ours to fix here.
#
# The old ENABLE_SA1 option is gone. It used to switch the *fork's* sa1.c, whose
# SA1_Map/SA1_WriteMap were two MEMMAP_NUM_BLOCKS pointer arrays = 32 KB of .bss
# that nothing read (see agents.md, 2026-09-15 and the HW_CONFIG 16 section).
# Upstream's sa1.c has no USE_SA1 gate at all and allocates its maps through
# port_alloc, so the 32 KB no longer exists and there is nothing to switch off.
#
# MSU-1 is the one chip with a real build flag, because msu1.c is wrapped in
# `#if ENABLE_MSU1` and the backend (msu1_port.cpp) is ours. On by default, which
# matches upstream. It allocates nothing and touches the card not at all unless a
# pack (<romname>.msu and/or <romname>-<n>.pcm) sits next to the ROM, so leaving
# it on costs a branch per port access on non-MSU1 carts.
# --------------------------------------------------------------------------
option(ENABLE_MSU1 "MSU-1 homebrew audio/data expansion (upstream default: ON)" ON)
if(ENABLE_MSU1)
    target_compile_definitions(${projectname} PUBLIC ENABLE_MSU1=1)
    message("* MSU-1: ENABLED (no allocation unless a .msu/.pcm pack is present)")
else()
    target_compile_definitions(${projectname} PUBLIC ENABLE_MSU1=0)
    message("* MSU-1: disabled")
endif()

# --------------------------------------------------------------------------
# Core (upstream snes9x) contract. Four of these are load-bearing and were each
# a silent failure mode:
#
#   PICO_MALLOC_PANIC=0   pico_malloc panics on OOM by default, which defeats
#                         every SRAM-first-with-PSRAM-fallback allocation in
#                         memmap.c and port_glue.cpp -- the fallback path NEEDS
#                         malloc to return NULL. Upstream memmap.c forces
#                         FillRAM to PSRAM under RENDER_TO_FB precisely so the
#                         four gfx strips fit in SRAM; with panicking malloc the
#                         process dies instead of falling back.
#
#   OPTIMISTIC_SBRK=0     newlib pads sbrk to 4 KB pages and an optimistic grant
#                         can hand out a block past __StackLimit -- straight into
#                         the core0/core1 stacks. Upstream records this as the
#                         cause of a HW_CONFIG=8 hardfault at game start.
#
#   RENDER_TO_FB=1        selects the strip renderer in port.h and gfx.c, which
#                         is where s9x_port_strip_copyout() comes from. Without
#                         it S9X_STRIP_ROWS is never defined and gfx.c does not
#                         call the copy-out hook at all -- the panel would then
#                         never be written.
#
#   PICO_SNESPLUS_HSTX    selects PIXEL_FORMAT = RGB555 in port.h. Our ILI9341
#                         driver already converts RGB555 -> RGB565 per pixel in
#                         snes_band_push(), so RGB555 is the format that path is
#                         proven against. Named for the upstream's HSTX board,
#                         which is also an RGB555 target; it is reused here as
#                         the pixel-format selector, not as an HSTX enable.
# --------------------------------------------------------------------------
target_compile_definitions(${projectname} PUBLIC
    PICO_MALLOC_PANIC=0
    PICO_USE_OPTIMISTIC_SBRK=0
    RENDER_TO_FB=1
    NO_ZERO_LUT=1
    PICO_SNESPLUS_HSTX=1
    # FillRAM stays in PSRAM, matching upstream's memory budget exactly.
    #
    # I had forced it into SRAM and it stopped the game from starting at all.
    # Upstream's own budget comment is explicit about this: "all emulator
    # buffers (Memory.RAM 128K, IAPU.RAM 64K, Memory.Map 16K, VRAM, SRAM,
    # FillRAM, MapInfo, ...) live in PSRAM. Under RENDER_TO_FB the GFX render
    # targets are ~26 KB SRAM staging strips (FillRAM forced to PSRAM to make
    # room)". Moving one 32 KB buffer out of a set that is coherent is not a
    # free win -- the SRAM it comes out of is the same SRAM the render strips
    # and the two stacks need, and taking 32 KB out of that arena is what broke
    # the boot. Do not re-derive this without measuring; upstream's partition is
    # the one that was tuned on hardware.
    # Upstream mixes audio on core1 and needs a cross-core lock around the S-DSP
    # state. We keep audio on core0 (pico_shared I2S), so the lock is compiled
    # out. Left at 0 explicitly: apu.c references port_sound_lock* under it.
    MIX_ON_CORE1=0
    # GSU wall-clock buckets in fxemu.c, reported in 0:/saves/snes/perf.txt at
    # game exit. Two timer reads per GSU run, i.e. per active scanline.
    PROFILE_BUCKETS=1
)
target_include_directories(${projectname} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/snes9x_src)

# --------------------------------------------------------------------------
# snes9x hot path -> SRAM (.time_critical)
#
# The single biggest performance lever available, and it is the same one
# pico-infonesPlus landed for the Game Boy (its AGENTS.md sec.14: "Emulacao 100%
# em SRAM (Zero-Wait-State)"). It is upstream's own mechanism too -- upstream
# CMakeLists.txt has this exact block and we were not using it.
#
# The problem, measured: of the snes9x code linked here, 449.410 B sits in FLASH
# and only the port glue is in SRAM. The core's hot functions are among it:
#
#     10091004 T S9xDoHDMA        <- FLASH
#     100aafcc T S9xUpdateScreen  <- FLASH
#     200005f8 T S9xMainLoop      <- SRAM (only because we made it so)
#
# Flash is an XIP window on this part, so every fetch from those addresses waits
# on the QSPI bus. Worse, the RP2350's XIP cache is 16 KB and SHARED between
# flash and PSRAM -- so while snes9x streams cart ROM out of PSRAM it evicts its
# own instruction fetch from that cache. Two effects, both measured by upstream:
#
#   1. ARM veneers. A branch from a 0x2xxxxxxx address to a 0x1xxxxxxx one
#      exceeds the +/-16 MB BL range, so GCC emits a trampoline per call site.
#      cpuops.c's 65816 dispatch reaches its handlers millions of times a second,
#      and each trampoline is a load + indirect branch.
#   2. Fetch stalls on every call into a flash-resident handler.
#
# The mechanism: rename .text/.rodata of the hot files to .time_critical_*, and
# the Pico SDK linker script's (*(.time_critical*)) rule places them in SRAM,
# generating the flash->SRAM copy at boot. .rodata moves too, because cpuops.c's
# five 65816 dispatch tables (~5 KB) are read on every emulated instruction and
# would otherwise stay in the shared XIP cache.
#
# -fno-function-sections / -fno-data-sections first: they coalesce each file into
# one .text, which is what makes the rename expressible (and which stops
# gc-sections from dropping any one function -- everything here is reachable via
# dispatch tables, so we want it all kept).
#
# Budget: this firmware links with only ~8 KB of SRAM heap free
# (__StackTop 0x20082000 - __HeapLimit 0x20080000), because port_alloc_sram()
# already claimed the SRAM it could for the gfx strips and the core's buffers
# went to PSRAM. So the list is deliberately the hot core only, ordered by cost:
# the files are added under the limit below and the build fails loudly rather
# than silently spilling if it does not fit.
# --------------------------------------------------------------------------
set(SNES9X_SRAM_HOT_FILES
    gfx.c       # RenderScreen / RenderLine / DrawBackground: the rasteriser
    tile.c      # DrawTile / DrawClippedTile / DrawLargePixel: per tile
    cpuops.c    # 65816 dispatch tables + every opcode handler
    ppu.c       # S9xUpdateScreen / S9xSetPPU
    dma.c       # S9xDoDMA / S9xDoHDMA
    soundux.c   # S9xMixSamples / MixStereo
    cpuexec.c   # S9xMainLoop's inner loop
    apu.c       # S9xInitAPU / S9xResetAPU / S9xSetAPU
    spc700.c    # APUExecute (16 KB on its own -- the largest single symbol)
    # memmap.c is deliberately NOT here. Every function in it is load-time only
    # (LoROMMap, SuperFXROMMap, ApplyROMFixes, InitROM, LoadROM ... ~32 KB) --
    # the run-time accessors S9xGetByte/S9xSetByte live in getset.c. Keeping it
    # in SRAM spent 32 KB of heap on code that runs once per game.
    clip.c      # ComputeClipWindows
    cpu.c
    getset.c    # S9xGetByte / S9xSetByte / GetWord: every emulated memory access
)

# Super FX (GSU): Yoshi's Island, Star Fox, Stunt Race FX, Doom. FxEmulate(~0)
# runs the GSU to STOP on every scanline it is active, so fxinst.c's ~620 opcode
# handlers (~54 KB) are the inner loop of those games -- and from flash they also
# evict the PSRAM ROM/TileCache lines from the shared 16 KB XIP cache. Paid for by
# taking memmap.c out above. OFF puts it back in flash if the SRAM heap is short
# (check [mem] heap_free / placements in 0:/saves/snes/perf.txt).
option(SNES_SUPERFX_BOUNDED "Run the GSU for a per-scanline budget instead of to STOP (see fxemu.c)" ON)
if(SNES_SUPERFX_BOUNDED)
    target_compile_definitions(${projectname} PRIVATE SNES_SUPERFX_BOUNDED=1)
    message("* Super FX scheduling: per-scanline budget (350/700)")
else()
    target_compile_definitions(${projectname} PRIVATE SNES_SUPERFX_BOUNDED=0)
    message("* Super FX scheduling: run to STOP (upstream)")
endif()

# Super FX carts only: GSU work RAM (plot target, tables, much of the GSU program
# data) in SRAM, VRAM in PSRAM instead. A trade between the GSU and the
# rasteriser. Measured A/B on Yoshi's Island: it won on every metric, so ON.
option(SNES_GSU_RAM_IN_SRAM "Super FX carts: GSU RAM in SRAM, VRAM in PSRAM" ON)
if(SNES_GSU_RAM_IN_SRAM)
    target_compile_definitions(${projectname} PRIVATE SNES_GSU_RAM_IN_SRAM=1)
    message("* Super FX carts: GSU RAM in SRAM / VRAM in PSRAM")
else()
    target_compile_definitions(${projectname} PRIVATE SNES_GSU_RAM_IN_SRAM=0)
endif()

option(SNES_SUPERFX_IN_SRAM "Place the Super FX core (fxinst.c/fxemu.c) in SRAM" ON)
if(SNES_SUPERFX_IN_SRAM)
    list(APPEND SNES9X_SRAM_HOT_FILES fxinst.c fxemu.c)
    message("* Super FX core in SRAM")
else()
    message("* Super FX core in flash")
endif()

# Object suffix: ".obj" under Ninja on Windows, ".o" on Unix hosts.
if(CMAKE_C_OUTPUT_EXTENSION)
    set(_objext "${CMAKE_C_OUTPUT_EXTENSION}")
else()
    set(_objext ".o")
endif()

foreach(_src IN LISTS SNES9X_SRAM_HOT_FILES)
    get_filename_component(_stem "${_src}" NAME_WE)
    set_source_files_properties(snes9x_src/${_src} PROPERTIES
        COMPILE_FLAGS "-fno-function-sections -fno-data-sections")
    add_custom_command(TARGET ${projectname} PRE_LINK
        COMMAND ${CMAKE_OBJCOPY}
                --rename-section .text=.time_critical_${_stem}
                --rename-section .rodata=.time_critical_rodata_${_stem}
                "${CMAKE_BINARY_DIR}/CMakeFiles/${projectname}.dir/snes9x_src/${_src}${_objext}"
        COMMENT "Moving snes9x ${_src} .text+.rodata to .time_critical (SRAM)"
        VERBATIM)
endforeach()
if(SNES_SUPERFX_IN_SRAM)
    # -Os for the GSU core: it shrinks fxinst.c by roughly a third, which is
    # what lets it fit in the heap memmap.c gave back without pushing FillRAM or
    # the sound LocalState out to PSRAM for every other game. Still far faster
    # than -O3 code fetched from flash through a cache it shares with PSRAM.
    set_source_files_properties(snes9x_src/fxinst.c PROPERTIES
        COMPILE_FLAGS "-fno-function-sections -fno-data-sections -Os")
endif()
list(LENGTH SNES9X_SRAM_HOT_FILES _n_hot)
message("* snes9x hot path in SRAM (.time_critical): ${_n_hot} files")

pico_set_program_name(${projectname} "PicoSNES")
pico_set_program_version(${projectname} "1.0")
pico_enable_stdio_uart(${projectname} 1)
pico_enable_stdio_usb(${projectname} 0)

target_compile_definitions(${projectname} PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)

target_compile_definitions(${projectname} PUBLIC
    USE_PSRAM=${USE_PSRAM}
    __OLD_RASTER_FX__=1
    STATIC_LINKING=1
    CORRECT_VRAM_READS=1
    ZLIB=0
    UNZIP_SUPPORT=0
    PIXEL_FORMAT=RGB555
    ILI9341_EXTERNAL_FB=1
)

target_compile_options(${projectname} PRIVATE
    -O3
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-cast-function-type
    -Wno-maybe-uninitialized
    -Wno-sign-compare
    -Wno-char-subscripts
    -Wno-strict-aliasing
    -fno-strict-aliasing
    # PRE-EXISTING, not introduced by the shared_menu migration.
    #
    # The vendored Snes9x 2002 sources declare several parallel byte arrays with
    # mismatched element types and assign between them without a cast, e.g.
    #   snes9x.h:216  uint32* Memory_MemorySpeed;
    #   cpu.c:166     CPU.Memory_MemorySpeed = Memory.MemorySpeed;   // uint8* source
    #   ppu.h:99      uint32  ScreenColors[256];
    #   gfx.c:2150    GFX.ScreenColors = DirectColourMaps[0];       // uint16* source
    #
    # These are deliberate byte-level reinterpretations of the same memory, so
    # they are harmless, but GCC 14 and newer promote
    # -Wincompatible-pointer-types from a warning to a hard error and the tree no
    # longer compiles from clean. It had been building only because build_bootloader
    # held stale .obj files -- the same masking the PicoWS io.c bug hid behind.
    #
    # The right fix is to correct the declarations in the core, which is a separate
    # and riskier change; suppressing matches the rest of this list and keeps the
    # menu/panel work unblocked.
    # it is applied to C only: it is not a valid C++ option and GCC warns on
    # every C++ translation unit if it is passed unconditionally.
    $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
)

target_compile_definitions(${projectname} PUBLIC 
    DVICONFIG=${DVICONFIG}
    SDCARD_PIN_CS=${SD_CS}
    SDCARD_PIN_SCK=${SD_SCK}
    SDCARD_PIN_MOSI=${SD_MOSI}
    SDCARD_PIN_MISO=${SD_MISO}
    SDCARD_PIO=pio1
    SDCARD_SPI=${SD_SPI}  
    NES_PIN_CLK=${NES_CLK}
    NES_PIN_DATA=${NES_DATA}
    NES_PIN_LAT=${NES_LAT}
    NES_PIO=${NES_PIO}
    NES_PIN_CLK_1=${NES_CLK_1}
    NES_PIN_DATA_1=${NES_DATA_1}
    NES_PIN_LAT_1=${NES_LAT_1}
    NES_PIO_1=${NES_PIO_1}
    WII_PIN_SDA=${WII_SDA}
    WII_PIN_SCL=${WII_SCL}
    WIIPAD_DELAYED_START=${WIIPAD_DELAYED_START}
    LED_GPIO_PIN=${LED_GPIO_PIN}
    HW_CONFIG=${HW_CONFIG}
    WIIPAD_I2C=${WIIPAD_I2C}
    USE_I2S_AUDIO=${USE_I2S_AUDIO}
    DVIAUDIOFREQ=32000
    PICO_AUDIO_I2S_DATA_PIN=${PICO_AUDIO_I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${PICO_AUDIO_I2S_CLOCK_PIN_BASE}
    PICO_AUDIO_I2S_PIO=${PICO_AUDIO_I2S_PIO}
    PICO_AUDIO_I2S_DMA_IRQ=1
    PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED=${PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED}
    PICO_AUDIO_I2S_RESET_PIN=${PICO_AUDIO_I2S_RESET_PIN}
    PICO_AUDIO_I2S_INTERRUPT_PIN=${PICO_AUDIO_I2S_INTERRUPT_PIN}
    PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON=${PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON}
    CFG_TUH_RPI_PIO_USB=${ENABLE_PIO_USB}
    PIO_USB_USE_PIO=${PIO_USB_USE_PIO}
    PSRAM_CS_PIN=${PSRAM_CS_PIN}
    GPIOHSTXD0=${GPIOHSTXD0}
    GPIOHSTXD1=${GPIOHSTXD1}
    GPIOHSTXD2=${GPIOHSTXD2}
    GPIOHSTXCK=${GPIOHSTXCK}
    GPIOHSTXINVERTED=${GPIOHSTXINVERTED}
    ENABLE_VU_METER=${ENABLE_VU_METER}
    ENABLEDVI=0
    USE_ILI9341=${USE_ILI9341}
    ILI9341_PIN_CS=${ILI9341_PIN_CS}
    ILI9341_PIN_SCK=${ILI9341_PIN_SCK}
    ILI9341_PIN_MOSI=${ILI9341_PIN_MOSI}
    ILI9341_PIN_DC=${ILI9341_PIN_DC}
    ILI9341_PIN_RST=${ILI9341_PIN_RST}
    ILI9341_PIN_BL=${ILI9341_PIN_BL}
    ILI9341_SPI_INST=${ILI9341_SPI_INST}
    DMG_DEFAULT_PANEL_ST7789=${DEFAULT_PANEL_ST7789}
)

if(NOT PICO_PLATFORM STREQUAL "rp2040")
    message(STATUS "Using PICO_PIO_USE_GPIO_BASE=1 for non-rp2040 platforms.")
    target_compile_definitions(${projectname} PRIVATE
        PICO_PIO_USE_GPIO_BASE=1
        PICO_STACK_SIZE=4096
    )
endif()

if(DEFINED PICO_DEFAULT_UART_TX_PIN)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_TX_PIN=${PICO_DEFAULT_UART_TX_PIN}
        PICO_DEFAULT_UART=0
    )
endif()
if(DEFINED PICO_DEFAULT_UART_RX_PIN)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART_RX_PIN=${PICO_DEFAULT_UART_RX_PIN}
    )
endif()
if(DEFINED PICO_DEFAULT_UART)
    target_compile_definitions(${projectname} PRIVATE
        PICO_DEFAULT_UART=${PICO_DEFAULT_UART}
    )
endif()

if(ENABLE_PIO_USB EQUAL 1)
    if(NOT PIO_DP_PLUS_PIN EQUAL -1)
        target_compile_definitions(${projectname} PRIVATE
            PICO_DEFAULT_PIO_USB_DP_PIN=${PIO_DP_PLUS_PIN}
        )
    endif()
    target_link_libraries(${projectname} PRIVATE tinyusb_pico_pio_usb)
endif()

# ---------------------------------------------------------------------------
# Link Libraries
# ---------------------------------------------------------------------------
target_link_libraries(${projectname}
PRIVATE
    pico_stdlib
    pico_multicore
    hardware_dma
    hardware_pio
    hardware_i2c
    hardware_interp
    hardware_timer
    hardware_clocks
    hardware_pwm
    hardware_flash
    hardware_xosc
    hardware_adc
    hardware_rosc
    util
    tinyusb_host 
    tinyusb_board
    pico_shared  
    xinput_host 
)

if(NOT USE_ILI9341)
    target_link_libraries(${projectname} PRIVATE dvi)
endif()

if(USE_ILI9341)
    target_link_libraries(${projectname} PRIVATE ili9341)
endif()

if(PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    target_link_libraries(${projectname} PRIVATE pico_cyw43_arch_none)
endif()

target_include_directories(${projectname}
PRIVATE
    src
    libretro
    libretro/libretro-common/include
    pico_lib
    tusb_xinput
    pico_shared
    pico_shared/drivers/ili9341
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu
    .
)

if(NOT USE_ILI9341)
    add_subdirectory(pico_lib/dvi)
endif()
add_subdirectory(pico_lib/util)
add_subdirectory(pico_shared)
add_subdirectory(tusb_xinput xinput_host)

target_link_options(${projectname} PRIVATE -Xlinker --print-memory-usage --data-sections)

# ---------------------------------------------------------------------------
# Bootloader Partition Relocation (offset 0x10080000)
# ---------------------------------------------------------------------------
if(BUILD_FOR_BOOTLOADER)
    include("pico_shared/BootPartition.cmake")
    if(DEFINED BUILD_FOR_BOOTLOADER_SLOT)
        frens_link_to_pinned_slot(${projectname} ${BUILD_FOR_BOOTLOADER_SLOT})
    else()
        frens_offset_for_bootloader(${projectname})
    endif()
endif()

pico_add_extra_outputs(${projectname})

