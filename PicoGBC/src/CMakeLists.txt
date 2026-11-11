add_subdirectory(ext/FatFs_SPI build)

# ---------------------------------------------------------------------------
# HW_CONFIG -- console board selection
# ---------------------------------------------------------------------------
# Mirrors the PicoNES/PicoPCE convention (pico_shared/BoardConfigs.cmake) so the
# same card boots the same way across every emulator. 15 is the original PicoDMG
# handheld (Waveshare RP2350 Plus 16MB); 16 is the Waveshare Core2350B console
# (RP2350B, 16 MB flash + 8 MB PSRAM on GP47).
if(USE_ILI9341 AND NOT HW_CONFIG)
    set(HW_CONFIG 15 CACHE STRING "Select the hardware configuration for your board")
elseif(NOT HW_CONFIG)
    set(HW_CONFIG 8 CACHE STRING "Select the hardware configuration for your board")
endif()

if(HW_CONFIG EQUAL 16)
    # Same physical harness as HW_CONFIG 15: buttons GP2..GP9, TFT on SPI0,
    # microSD on SPI1. The Core2350B board header's own SPI0 defaults
    # (CS 17 / SCK 18 / MOSI 19) line up with the existing TFT wiring, and
    # 12..15 are unclaimed on that board, which is what pins the SD below.
    #
    # The panel is NOT a build choice: mk_st7789.c drives both ILI9341 and
    # ST7789, and MADCTL is passed through from RP2040_GB_ST7789_MADCTL. See
    # RP2040_GB_ST7789_MADCTL below for the 0x60 value the ST7789 needs.
    #
    # FORCE: the VS Code toolbar rewrites PICO_BOARD in the cache, and this board
    # must not silently fall back to the RP2350A one -- GPIO 47 (the PSRAM chip
    # select) does not exist there.
    set(PICO_BOARD "waveshare_core2350b" CACHE STRING "Board type" FORCE)
    # PICO_LED_BUILTIN resolves to GPIO 39 in boards/waveshare_core2350b.h.
    set(RP2040_GB_LED_GPIO 39)
    set(RP2040_GB_PSRAM_CS_PIN 47)
    set(RP2040_GB_TFT_SPI "spi0")
    set(RP2040_GB_SD_SPI "spi1")
    set(RP2040_GB_SD_CS 13)
    set(RP2040_GB_SD_SCK 14)
    set(RP2040_GB_SD_MOSI 15)
    set(RP2040_GB_SD_MISO 12)
    set(RP2040_GB_TFT_CS 17)
    set(RP2040_GB_TFT_SCK 18)
    set(RP2040_GB_TFT_MOSI 19)
    set(RP2040_GB_TFT_DC 20)
    set(RP2040_GB_TFT_RST 21)
    set(RP2040_GB_TFT_BL 22)
    set(RP2040_GB_I2S_DATA 26)
    set(RP2040_GB_I2S_CLK 27)
    message(STATUS "HW_CONFIG 16: Waveshare Core2350B + 8 MB PSRAM on GP47")
elseif(HW_CONFIG EQUAL 15)
    set(RP2040_GB_LED_GPIO 25)
    set(RP2040_GB_PSRAM_CS_PIN 0)
    set(RP2040_GB_TFT_SPI "spi0")
    set(RP2040_GB_SD_SPI "spi1")
    set(RP2040_GB_SD_CS 13)
    set(RP2040_GB_SD_SCK 14)
    set(RP2040_GB_SD_MOSI 15)
    set(RP2040_GB_SD_MISO 12)
    set(RP2040_GB_TFT_CS 17)
    set(RP2040_GB_TFT_SCK 18)
    set(RP2040_GB_TFT_MOSI 19)
    set(RP2040_GB_TFT_DC 20)
    set(RP2040_GB_TFT_RST 21)
    set(RP2040_GB_TFT_BL 22)
    set(RP2040_GB_I2S_DATA 26)
    set(RP2040_GB_I2S_CLK 27)
    message(STATUS "HW_CONFIG 15: Waveshare RP2350 Plus 16MB (no PSRAM)")
else()
    # The default must repeat HW_CONFIG 15 verbatim rather than erroring: the
    # original build_for_bootloader.ps1 never passed HW_CONFIG, and a project
    # that has run for weeks should not stop configuring because a new option
    # was introduced.
    set(PICO_BOARD "waveshare_rp2350_plus_16mb" CACHE STRING "Board type" FORCE)
    set(RP2040_GB_LED_GPIO 25)
    set(RP2040_GB_PSRAM_CS_PIN 0)
    set(RP2040_GB_TFT_SPI "spi0")
    set(RP2040_GB_SD_SPI "spi1")
    set(RP2040_GB_SD_CS 13)
    set(RP2040_GB_SD_SCK 14)
    set(RP2040_GB_SD_MOSI 15)
    set(RP2040_GB_SD_MISO 12)
    set(RP2040_GB_TFT_CS 17)
    set(RP2040_GB_TFT_SCK 18)
    set(RP2040_GB_TFT_MOSI 19)
    set(RP2040_GB_TFT_DC 20)
    set(RP2040_GB_TFT_RST 21)
    set(RP2040_GB_TFT_BL 22)
    set(RP2040_GB_I2S_DATA 26)
    set(RP2040_GB_I2S_CLK 27)
    message(STATUS "HW_CONFIG ${HW_CONFIG} unsupported, defaulting to 15 (Waveshare RP2350 Plus 16MB)")
endif()

set(RP2040_GB_USE_PSRAM 0 CACHE BOOL "Load and run the cartridge ROM from PSRAM instead of programming it into flash")
# SD SPI clock in Hz. 20 MHz matches what the rest of the console uses. The
# emulator ran at 12.5 MHz for its whole life (hardcoded in src/inc/sdcard.h)
# while the build advertised 20 MHz via a define nothing read.
set(RP2040_GB_SD_SPI_BAUD_HZ "20000000" CACHE STRING "SD card SPI clock in Hz")
if(HW_CONFIG EQUAL 16)
    # The Core2350B has no reason to ever program the ROM into flash: writing a
    # 512 KB..8 MB cart there both erases sectors the firmware occupies and
    # wastes the board's PSRAM entirely.
    set(RP2040_GB_USE_PSRAM 1 CACHE BOOL "Load and run the cartridge ROM from PSRAM instead of programming it into flash" FORCE)
endif()

set(DISPLAY_DRIVER "ST7789" CACHE STRING "LCD controller for RP2040_GB (ILI9225, ST7789, or ILI9341)")
set_property(CACHE DISPLAY_DRIVER PROPERTY STRINGS ILI9225 ST7789 ILI9341)
string(TOUPPER "${DISPLAY_DRIVER}" DISPLAY_DRIVER_UPPER)

option(RP2040_GB_ENABLE_LCD "Enable LCD output" ON)
option(RP2040_GB_ENABLE_SOUND "Enable I2S sound" ON)
option(RP2040_GB_ENABLE_SDCARD "Enable SD card features (ROM selector + saves)" ON)
option(RP2040_GB_ENABLE_NFC "Enable RC522 NFC boot tag support" OFF)
option(RP2040_GB_USE_DMG_BUTTON_MATRIX "Read buttons through DMG P10..P15 matrix lines" OFF)
option(RP2040_GB_DEFAULT_INTERLACE "Enable PeanutGB interlace mode by default at game boot" OFF)
set(RP2040_GB_NFC_CS_GPIO "10" CACHE STRING "RC522 chip-select GPIO")
set(RP2040_GB_NFC_RST_GPIO "11" CACHE STRING "RC522 reset GPIO")
set(RP2040_GB_NFC_SPI_BAUD "4000000" CACHE STRING "RC522 SPI baud rate")
set(RP2040_GB_NFC_MAP_FILE "nfc_games.csv" CACHE STRING "SD-card NFC UID to ROM mapping filename")
if(PICO_PLATFORM STREQUAL "rp2040")
    set(RP2040_GB_DEFAULT_SYS_CLOCK_VCO "1596000000")
    set(RP2040_GB_DEFAULT_SYS_CLOCK_DIV1 "6")
else()
    set(RP2040_GB_DEFAULT_SYS_CLOCK_VCO "1440000000")
    set(RP2040_GB_DEFAULT_SYS_CLOCK_DIV1 "5")
endif()
set(RP2040_GB_SYS_CLOCK_VCO "${RP2040_GB_DEFAULT_SYS_CLOCK_VCO}" CACHE STRING "System PLL VCO frequency in Hz")
set(RP2040_GB_SYS_CLOCK_DIV1 "${RP2040_GB_DEFAULT_SYS_CLOCK_DIV1}" CACHE STRING "System PLL post divider 1")
set(RP2040_GB_SYS_CLOCK_DIV2 "1" CACHE STRING "System PLL post divider 2")
set(RP2040_GB_ST7789_MADCTL "0x28" CACHE STRING "LCD MADCTL value for landscape 320x240 (ST7789/ILI9341). ILI9341: 0x28 (paisagem) / 0xE8 (180 deg). ST7789: 0x60 (paisagem) / 0xA0 (180 deg) -- 0x60 is 0x28+MX, measured on the Core2350B panel, and inherits BGR=1 so red/blue stay in order. Options: 0x20, 0x28, 0x60, 0xA0, 0xE8")
set(RP2040_GB_ST7789_COLMOD "0x55" CACHE STRING "ST7789 COLMOD value (0x55 for RGB565, some clones may need 0x05)")
option(RP2040_GB_ST7789_INVERT "Enable ST7789 inversion at init (INVON)" OFF)
option(RP2040_GB_ST7789_SWAP_RGB565_BYTES "Swap RGB565 byte order when sending to ST7789" OFF)
set(RP2040_GB_ST7789_RGB_ORDER "RGB" CACHE STRING "Software RGB565 channel order remap: RGB, RBG, GRB, GBR, BRG, BGR")
set_property(CACHE RP2040_GB_ST7789_RGB_ORDER PROPERTY STRINGS RGB RBG GRB GBR BRG BGR)
option(RP2040_GB_ST7789_INVERT_RGB565 "Invert RGB565 channel bits in software before SPI write" OFF)
set(RP2040_GB_BUILTIN_ROM "" CACHE FILEPATH "Path to a .gb ROM file embedded into RP2040_GB UF2")

set(RP2040_GB_BUILTIN_ROM_SOURCE "")
set(RP2040_GB_BUILTIN_ROM_DEFINES "")
if(RP2040_GB_BUILTIN_ROM)
    if(NOT EXISTS "${RP2040_GB_BUILTIN_ROM}")
        message(FATAL_ERROR "RP2040_GB_BUILTIN_ROM file not found: ${RP2040_GB_BUILTIN_ROM}")
    endif()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RP2040_GB_BUILTIN_ROM}")
    file(READ "${RP2040_GB_BUILTIN_ROM}" ROM_HEX HEX)
    string(LENGTH "${ROM_HEX}" ROM_HEX_LEN)
    math(EXPR ROM_SIZE_BYTES "${ROM_HEX_LEN} / 2")

    string(REGEX REPLACE "([0-9A-Fa-f]{64})" "\\1\n" ROM_HEX_LINES "${ROM_HEX}")
    string(REPLACE "\n" ";" ROM_HEX_LIST "${ROM_HEX_LINES}")

    set(ROM_C_ARRAY "")
    foreach(ROM_LINE IN LISTS ROM_HEX_LIST)
        if(NOT ROM_LINE STREQUAL "")
            string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1, " ROM_C_LINE "${ROM_LINE}")
            string(APPEND ROM_C_ARRAY "    ${ROM_C_LINE}\n")
        endif()
    endforeach()

    set(RP2040_GB_BUILTIN_ROM_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/rp2040_gb_builtin_rom.c")
    file(WRITE "${RP2040_GB_BUILTIN_ROM_SOURCE}"
"#include <stdint.h>

const uint8_t rp2040_gb_builtin_rom[] = {
${ROM_C_ARRAY}};
const unsigned int rp2040_gb_builtin_rom_size = sizeof(rp2040_gb_builtin_rom);
")
    set(RP2040_GB_BUILTIN_ROM_DEFINES RP2040_GB_HAS_BUILTIN_ROM=1)
    message(STATUS "RP2040_GB builtin ROM: ${RP2040_GB_BUILTIN_ROM} (${ROM_SIZE_BYTES} bytes)")
endif()

if(DISPLAY_DRIVER_UPPER STREQUAL "ILI9225")
    set(RP2040_GB_DISPLAY_SOURCE firmware/mk_ili9225.c)
    set(RP2040_GB_DISPLAY_DEFINES DISPLAY_ILI9225=1)
elseif(DISPLAY_DRIVER_UPPER STREQUAL "ST7789" OR DISPLAY_DRIVER_UPPER STREQUAL "ILI9341")
    set(RP2040_GB_DISPLAY_SOURCE firmware/mk_st7789.c)
    set(RP2040_GB_DISPLAY_DEFINES DISPLAY_ST7789=1 DISPLAY_ILI9341=1)
else()
    message(FATAL_ERROR "Unsupported DISPLAY_DRIVER='${DISPLAY_DRIVER}'. Use ILI9225, ST7789, or ILI9341.")
endif()

message(STATUS "RP2040_GB display driver: ${DISPLAY_DRIVER_UPPER}")

set(RP2040_GB_NFC_SOURCE "")
if(RP2040_GB_ENABLE_NFC)
    if(NOT RP2040_GB_ENABLE_SDCARD)
        message(FATAL_ERROR "RP2040_GB_ENABLE_NFC requires RP2040_GB_ENABLE_SDCARD because mappings are loaded from SD")
    endif()
    set(RP2040_GB_NFC_SOURCE firmware/mfrc522.c)
endif()

string(TOUPPER "${RP2040_GB_ST7789_RGB_ORDER}" ST7789_RGB_ORDER_UPPER)
set(ST7789_RGB_ORDER_VALUE 0)
if(ST7789_RGB_ORDER_UPPER STREQUAL "RGB")
    set(ST7789_RGB_ORDER_VALUE 0)
elseif(ST7789_RGB_ORDER_UPPER STREQUAL "RBG")
    set(ST7789_RGB_ORDER_VALUE 1)
elseif(ST7789_RGB_ORDER_UPPER STREQUAL "GRB")
    set(ST7789_RGB_ORDER_VALUE 2)
elseif(ST7789_RGB_ORDER_UPPER STREQUAL "GBR")
    set(ST7789_RGB_ORDER_VALUE 3)
elseif(ST7789_RGB_ORDER_UPPER STREQUAL "BRG")
    set(ST7789_RGB_ORDER_VALUE 4)
elseif(ST7789_RGB_ORDER_UPPER STREQUAL "BGR")
    set(ST7789_RGB_ORDER_VALUE 5)
else()
    message(FATAL_ERROR "Unsupported RP2040_GB_ST7789_RGB_ORDER='${RP2040_GB_ST7789_RGB_ORDER}'. Use RGB, RBG, GRB, GBR, BRG, or BGR.")
endif()

set(RP2040_GB_SOURCES
        firmware/main.c
        firmware/boot_sound_data.c
        firmware/gb_rumble.c
        ${RP2040_GB_DISPLAY_SOURCE}
        ${RP2040_GB_NFC_SOURCE}
        ${RP2040_GB_BUILTIN_ROM_SOURCE}
        ext/minigb_apu/minigb_apu.c
        ext/i2s/i2s.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_menu.c
        # dmg_menu.c and dmg_panel.c are the two files of the shared_menu
        # library. Only the first was listed here, so the panel/boot.txt readers
        # (dmg_panel_get_*, and the RUMBLE= / RTC_EPOCH= keys with them) were not
        # linked in despite being declared in dmg_menu.h. Both are listed
        # explicitly rather than linking the library, to keep this target's
        # source list self-contained as it already is.
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_panel.c
        # dmg_combo.c is the third shared_menu file: the in-game combo layer
        # (SELECT+START exit hold, START+B pause, START+A FPS, SELECT+A speed).
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_combo.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_power.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_battery.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_resume.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu/dmg_boot_intro.c
)

# The PSRAM driver and its allocator are only present on boards that have both.
# PicoPlusPsram's constructor hands the PSRAM window to lwmem, and the cartridge
# ROM is then allocated from there, so both are needed together.
if(HW_CONFIG EQUAL 16)
    list(APPEND RP2040_GB_SOURCES
            firmware/PicoPlusPsram.cpp
            firmware/SetupPsram.c
            firmware/gb_psram.cpp
    )
endif()

add_executable(PicoGBC
        ${RP2040_GB_SOURCES}
)

add_executable(PicoDMG
        ${RP2040_GB_SOURCES}
)

add_executable(display_smoke_test
        firmware/display_smoke_test.c
)

add_executable(nfc_smoke_test
        firmware/nfc_smoke_test.c
        firmware/mfrc522.c
)

pico_generate_pio_header(PicoGBC ${CMAKE_CURRENT_LIST_DIR}/ext/i2s/audio_i2s.pio)
pico_generate_pio_header(PicoDMG ${CMAKE_CURRENT_LIST_DIR}/ext/i2s/audio_i2s.pio)

target_include_directories(PicoGBC PRIVATE inc ext/minigb_apu ext/i2s ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu)
target_include_directories(PicoDMG PRIVATE inc ext/minigb_apu ext/i2s ${CMAKE_CURRENT_LIST_DIR}/../../shared_menu)
target_include_directories(display_smoke_test PRIVATE inc)
target_include_directories(nfc_smoke_test PRIVATE inc)

# lwmem lives in-tree (ext/lwmem) and is compiled straight into the two
# executables, which is how the PicoNES shared copy is consumed as well. The
# vendored tree has its own CMakeLists, but pulling it in via add_subdirectory
# would rename the target and hide it from --print-memory-usage, so the two
# source files are listed explicitly.
if(HW_CONFIG EQUAL 16)
    target_include_directories(PicoGBC PRIVATE ext/lwmem/lwmem/src/include)
    target_include_directories(PicoDMG PRIVATE ext/lwmem/lwmem/src/include)
    target_sources(PicoGBC PRIVATE
            ext/lwmem/lwmem/src/lwmem/lwmem.c
            ext/lwmem/lwmem/src/lwmem/lwmem.cpp
    )
    target_sources(PicoDMG PRIVATE
            ext/lwmem/lwmem/src/lwmem/lwmem.c
            ext/lwmem/lwmem/src/lwmem/lwmem.cpp
    )
endif()

set(RP2040_GB_LINK_LIBS
        FatFs_SPI
        pico_stdlib pico_stdio pico_bootrom pico_multicore
        hardware_clocks
    hardware_xosc
    hardware_adc
    hardware_rosc hardware_pio hardware_vreg hardware_pwm
        hardware_sync hardware_pll hardware_spi hardware_irq hardware_dma
        hardware_flash
        pico_binary_info
)

# PSRAM driver: only pulled in when the board actually has one (GP47 on the
# Core2350B, 0 on the RP2350 Plus). SetupPsram.c maps the window at 0x11000000
# and detects the size from the chip's JEDEC id.
if(HW_CONFIG EQUAL 16)
    list(APPEND RP2040_GB_LINK_LIBS
            hardware_gpio
    )
endif()

target_link_libraries(PicoGBC ${RP2040_GB_LINK_LIBS})
target_link_libraries(PicoDMG ${RP2040_GB_LINK_LIBS})
target_link_libraries(display_smoke_test
        pico_stdlib
        hardware_clocks
        hardware_spi)
target_link_libraries(nfc_smoke_test
        pico_stdlib
        hardware_spi)

set(RP2040_GB_COMMON_DEFINES
        ${RP2040_GB_DISPLAY_DEFINES}
        ${RP2040_GB_BUILTIN_ROM_DEFINES}
        ST7789_MADCTL_VALUE=${RP2040_GB_ST7789_MADCTL}
        ST7789_COLMOD_VALUE=${RP2040_GB_ST7789_COLMOD}
        ST7789_INIT_INVERT=$<BOOL:${RP2040_GB_ST7789_INVERT}>
        ST7789_SWAP_RGB565_BYTES=$<BOOL:${RP2040_GB_ST7789_SWAP_RGB565_BYTES}>
        ST7789_RGB_ORDER=${ST7789_RGB_ORDER_VALUE}
        ST7789_INVERT_RGB565=$<BOOL:${RP2040_GB_ST7789_INVERT_RGB565}>
        USE_DMG_BUTTON_MATRIX=$<BOOL:${RP2040_GB_USE_DMG_BUTTON_MATRIX}>
        DEFAULT_INTERLACE=$<IF:$<BOOL:${RP2040_GB_DEFAULT_INTERLACE}>,1,0>
        ENABLE_LCD=$<BOOL:${RP2040_GB_ENABLE_LCD}>
        ENABLE_SOUND=$<BOOL:${RP2040_GB_ENABLE_SOUND}>
        ENABLE_SDCARD=$<BOOL:${RP2040_GB_ENABLE_SDCARD}>
        ENABLE_NFC=$<BOOL:${RP2040_GB_ENABLE_NFC}>
        # Cartridge SRAM backing store, so the core can clamp num_ram_banks to
        # what main.c actually allocated instead of trusting header 0x0149.
        GB_CART_RAM_MAX_SIZE=65536u
        GB_MAX_RAM_BANKS=8
        RP2040_GB_SYS_CLOCK_VCO=${RP2040_GB_SYS_CLOCK_VCO}
        RP2040_GB_SYS_CLOCK_DIV1=${RP2040_GB_SYS_CLOCK_DIV1}
        RP2040_GB_SYS_CLOCK_DIV2=${RP2040_GB_SYS_CLOCK_DIV2}
        MFRC522_PIN_CS=${RP2040_GB_NFC_CS_GPIO}
        MFRC522_PIN_RST=${RP2040_GB_NFC_RST_GPIO}
        MFRC522_SPI_BAUD=${RP2040_GB_NFC_SPI_BAUD}
        NFC_MAP_FILE="${RP2040_GB_NFC_MAP_FILE}"
        # Board pinout, so main.c and the drivers stop hardcoding GPIO numbers.
        GB_LED_GPIO=${RP2040_GB_LED_GPIO}
        GB_PSRAM_CS_PIN=${RP2040_GB_PSRAM_CS_PIN}
        GB_USE_PSRAM=$<BOOL:${RP2040_GB_USE_PSRAM}>
        GB_TFT_SPI="${RP2040_GB_TFT_SPI}"
        GB_TFT_PIN_CS=${RP2040_GB_TFT_CS}
        GB_TFT_PIN_SCK=${RP2040_GB_TFT_SCK}
        GB_TFT_PIN_MOSI=${RP2040_GB_TFT_MOSI}
        GB_TFT_PIN_DC=${RP2040_GB_TFT_DC}
        GB_TFT_PIN_RST=${RP2040_GB_TFT_RST}
        GB_TFT_PIN_BL=${RP2040_GB_TFT_BL}
        GB_SD_SPI="${RP2040_GB_SD_SPI}"
        GB_SD_PIN_CS=${RP2040_GB_SD_CS}
        GB_SD_PIN_SCK=${RP2040_GB_SD_SCK}
        GB_SD_PIN_MOSI=${RP2040_GB_SD_MOSI}
        GB_SD_PIN_MISO=${RP2040_GB_SD_MISO}
        GB_I2S_PIN_DATA=${RP2040_GB_I2S_DATA}
        GB_I2S_PIN_CLK=${RP2040_GB_I2S_CLK}
        # Read by src/inc/sdcard.h. This define existed for a long time but was
        # never referenced from any source file, so the build advertised 20 MHz
        # while the SD actually ran at a hardcoded 12.5 MHz.
        GB_SD_SPI_BAUD_HZ=${RP2040_GB_SD_SPI_BAUD_HZ}
        SD_SPI_BAUD_RATE=20000000
        PARAM_ASSERTIONS_DISABLE_ALL=1
        PICO_STDIO_ENABLE_CRLF_SUPPORT=1
        PICO_STDIO_DEFAULT_CRLF=1
        PICO_PRINTF_SUPPORT_FLOAT=0
        PICO_PRINTF_SUPPORT_EXPONENTIAL=0
        PICO_PRINTF_SUPPORT_LONG_LONG=1
        PICO_PRINTF_SUPPORT_PTRDIFF_T=0
)

target_compile_definitions(PicoGBC PRIVATE
        ${RP2040_GB_COMMON_DEFINES}
        GBC_IS_DMG_BUILD=0
        GB_APP_LABEL="PicoGBC"
)

target_compile_definitions(PicoDMG PRIVATE
        ${RP2040_GB_COMMON_DEFINES}
        GBC_IS_DMG_BUILD=1
        GB_APP_LABEL="PicoDMG"
)

target_compile_definitions(display_smoke_test PRIVATE
        ST7789_MADCTL_VALUE=${RP2040_GB_ST7789_MADCTL}
        ST7789_COLMOD_VALUE=${RP2040_GB_ST7789_COLMOD}
        ST7789_INIT_INVERT=$<BOOL:${RP2040_GB_ST7789_INVERT}>
        ST7789_SWAP_RGB565_BYTES=$<BOOL:${RP2040_GB_ST7789_SWAP_RGB565_BYTES}>
        ST7789_RGB_ORDER=${ST7789_RGB_ORDER_VALUE}
        ST7789_INVERT_RGB565=$<BOOL:${RP2040_GB_ST7789_INVERT_RGB565}>
        PICO_STDIO_ENABLE_CRLF_SUPPORT=1
        PICO_STDIO_DEFAULT_CRLF=1)

target_compile_definitions(nfc_smoke_test PRIVATE
        MFRC522_PIN_CS=${RP2040_GB_NFC_CS_GPIO}
        MFRC522_PIN_RST=${RP2040_GB_NFC_RST_GPIO}
        MFRC522_SPI_BAUD=${RP2040_GB_NFC_SPI_BAUD}
        PICO_STDIO_ENABLE_CRLF_SUPPORT=1
        PICO_STDIO_DEFAULT_CRLF=1)

function(pico_add_verbose_dis_output TARGET)
	add_custom_command(TARGET ${TARGET} POST_BUILD
		COMMAND ${CMAKE_OBJDUMP} -h $<TARGET_FILE:${TARGET}> >$<IF:$<BOOL:$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>>,$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>,$<TARGET_PROPERTY:${TARGET},NAME>>.dis
		COMMAND ${CMAKE_OBJDUMP} -drwCSl $<TARGET_FILE:${TARGET}> >>$<IF:$<BOOL:$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>>,$<TARGET_PROPERTY:${TARGET},OUTPUT_NAME>,$<TARGET_PROPERTY:${TARGET},NAME>>.dis
		)
endfunction()

pico_set_program_name(PicoGBC "PicoGBC")
pico_set_program_version(PicoGBC "1.1.1")

pico_set_program_name(PicoDMG "PicoDMG")
pico_set_program_version(PicoDMG "1.1.1")

option(BUILD_FOR_BOOTLOADER "Link application for pico-bootLoader at 0x10080000" OFF)
if(BUILD_FOR_BOOTLOADER)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BootPartition.cmake)
    frens_offset_for_bootloader(PicoGBC)
    frens_offset_for_bootloader(PicoDMG)
else()
    pico_set_binary_type(PicoGBC copy_to_ram)
    pico_set_binary_type(PicoDMG copy_to_ram)
endif()

pico_enable_stdio_usb(PicoGBC 0)
pico_enable_stdio_uart(PicoGBC 1)
pico_add_verbose_dis_output(PicoGBC)
pico_add_bin_output(PicoGBC)
pico_add_uf2_output(PicoGBC)

pico_enable_stdio_usb(PicoDMG 0)
pico_enable_stdio_uart(PicoDMG 1)
pico_add_verbose_dis_output(PicoDMG)
pico_add_bin_output(PicoDMG)
pico_add_uf2_output(PicoDMG)

# Backward-compatibility alias so building 'RP2040_GB' builds both
add_custom_target(RP2040_GB DEPENDS PicoGBC PicoDMG)

# Copy PicoGBC.uf2 to the build root and project root directories for convenience
#
# The project-root copy is for the HW15/ILI9340 console only. Both build configs
# use the same target names, so an unconditional copy made build_hw16 overwrite
# the HW15 binary in the project root with the ST7789 one -- same filename, and
# the wrong one for a PicoDMG console. HW16 gets its suffixed name from
# build_core2350b.ps1 instead, which copies out of its own build directory.
set(RP2040_GB_COPY_TO_PROJECT_ROOT PicoGBC PicoDMG)
if(HW_CONFIG EQUAL 16)
    set(RP2040_GB_COPY_TO_PROJECT_ROOT)
endif()

add_custom_command(TARGET PicoGBC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/PicoGBC.uf2"
            "${CMAKE_BINARY_DIR}/PicoGBC.uf2"
    COMMENT "Copying PicoGBC.uf2 to build root"
)
if("PicoGBC" IN_LIST RP2040_GB_COPY_TO_PROJECT_ROOT)
    add_custom_command(TARGET PicoGBC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/PicoGBC.uf2"
                "${CMAKE_CURRENT_SOURCE_DIR}/../PicoGBC.uf2"
        COMMENT "Copying PicoGBC.uf2 to project root"
    )
endif()

# Copy PicoDMG.uf2 to the build root and project root directories for convenience
add_custom_command(TARGET PicoDMG POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/PicoDMG.uf2"
            "${CMAKE_BINARY_DIR}/PicoDMG.uf2"
    COMMENT "Copying PicoDMG.uf2 to build root"
)
if("PicoDMG" IN_LIST RP2040_GB_COPY_TO_PROJECT_ROOT)
    add_custom_command(TARGET PicoDMG POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_BINARY_DIR}/PicoDMG.uf2"
                "${CMAKE_CURRENT_SOURCE_DIR}/../PicoDMG.uf2"
        COMMENT "Copying PicoDMG.uf2 to project root"
    )
endif()

pico_enable_stdio_usb(display_smoke_test 0)
pico_enable_stdio_uart(display_smoke_test 1)
pico_add_bin_output(display_smoke_test)
pico_add_uf2_output(display_smoke_test)

pico_enable_stdio_usb(nfc_smoke_test 0)
pico_enable_stdio_uart(nfc_smoke_test 1)
pico_add_bin_output(nfc_smoke_test)
pico_add_uf2_output(nfc_smoke_test)
