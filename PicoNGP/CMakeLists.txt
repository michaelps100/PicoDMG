cmake_minimum_required(VERSION 3.13)
set(projectname PicoNGP)

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
    set(USE_ILI9341 1 CACHE STRING "Use ILI9341 SPI TFT display")
endif()

if(USE_ILI9341 AND NOT HW_CONFIG)
    set(HW_CONFIG 15 CACHE STRING "Select the hardware configuration for your board")      
elseif(NOT HW_CONFIG)
    set(HW_CONFIG 15 CACHE STRING "Select the hardware configuration for your board")      
endif()

include("pico_shared/BoardConfigs.cmake")

if(NOT PICO_BOARD)
    set(PICO_BOARD waveshare_rp2350_plus_16mb CACHE STRING "Board type")
    message("PICO_BOARD not set, using default: ${PICO_BOARD}")
endif()

add_definitions(-DNDEBUG)

# ---------------------------------------------------------------------------
# Fallback panel when /boot.txt is absent or has no ST7789= key.
#
# The panel is a runtime choice read by shared_menu/dmg_panel.c, not a build
# choice, so this only decides what a card with no /boot.txt gets. Defaults to 0
# (ILI9341) so the existing HW_CONFIG 15 build keeps its current display; the
# HW_CONFIG 16 console build overrides it with -DDEFAULT_PANEL_ST7789=1.
# ---------------------------------------------------------------------------
if(NOT DEFINED DEFAULT_PANEL_ST7789)
    set(DEFAULT_PANEL_ST7789 0 CACHE STRING
        "Fallback panel when /boot.txt is absent: 0=ILI9341 1=ST7789")
endif()
if(HW_CONFIG EQUAL 16 AND NOT DEFAULT_PANEL_ST7789)
    # The Core2350B console carries an ST7789, so a card with a missing or
    # ST7789-less /boot.txt would otherwise come up on the wrong driver and show
    # nothing at all. Matches the bootloader's default for this board.
    set(DEFAULT_PANEL_ST7789 1 CACHE STRING
        "Fallback panel when /boot.txt is absent: 0=ILI9341 1=ST7789" FORCE)
endif()

# ---------------------------------------------------------------------------
# TinyUSB family support + Pico SDK init
# ---------------------------------------------------------------------------
set(FAMILY rp2040)
include($ENV{PICO_SDK_PATH}/lib/tinyusb/hw/bsp/family_support.cmake)

project(${projectname} C CXX ASM)
pico_sdk_init()

# On-screen profiler (EMU/OTH/TRUN panel). Off by default; it has to be a
# directory-wide define because the ili9341 driver is a separate target.
option(NGP_PROFILE "Build the on-screen frame profiler" OFF)
if(NGP_PROFILE)
    add_compile_definitions(NGP_PROFILE=1)
endif()

# ---------------------------------------------------------------------------
# NeoPop Core Sources
# ---------------------------------------------------------------------------
set(NEOPOP_SRC
    src/neopop/TLCS-900h/TLCS900h_interpret.c
    src/neopop/TLCS-900h/TLCS900h_interpret_dst.c
    src/neopop/TLCS-900h/TLCS900h_interpret_reg.c
    src/neopop/TLCS-900h/TLCS900h_interpret_single.c
    src/neopop/TLCS-900h/TLCS900h_interpret_src.c
    src/neopop/TLCS-900h/TLCS900h_registers.c
    src/neopop/z80/Z80.c
    src/neopop/bios.c
    src/neopop/biosHLE.c
    src/neopop/dma.c
    src/neopop/flash.c
    src/neopop/gfx.c
    src/neopop/gfx_scanline_colour.c
    src/neopop/gfx_scanline_mono.c
    src/neopop/interrupt.c
    src/neopop/mem.c
    src/neopop/neopop.c
    src/neopop/rom.c
    src/neopop/sound.c
    src/neopop/state.c
    src/neopop/Z80_interface.c
)

add_executable(${projectname}
    src/main.cpp
    ${NEOPOP_SRC}
    ../shared_menu/dmg_menu.c
    ../shared_menu/dmg_panel.c
    ../shared_menu/dmg_combo.c
    ../shared_menu/dmg_power.c
    ../shared_menu/dmg_battery.c
    ../shared_menu/dmg_resume.c
    ../shared_menu/dmg_boot_intro.c
)

pico_set_program_name(${projectname} "PicoNGP")
pico_set_program_version(${projectname} "1.0")

pico_enable_stdio_uart(${projectname} 1)
pico_enable_stdio_usb(${projectname} 0)

target_compile_definitions(${projectname} PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)

target_compile_options(${projectname} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-cast-function-type
    -Wno-maybe-uninitialized
    -Wno-narrowing
    -Wno-sign-compare
    -Wno-write-strings
    -Ofast
)

# LTO was tried here and does NOT link: with the interpreter merged across its
# five translation units the resulting function overruns Thumb-2's branch range
# and the assembler fails with "offset out of range". Left off deliberately.
# If it is ever revisited, the fix is to stop merging the interpreter into one
# unit (e.g. -flto-partition=one) rather than to force the range.

set_source_files_properties(
    src/neopop/TLCS-900h/TLCS900h_interpret.c
    src/neopop/TLCS-900h/TLCS900h_interpret_dst.c
    src/neopop/TLCS-900h/TLCS900h_interpret_reg.c
    src/neopop/TLCS-900h/TLCS900h_interpret_single.c
    src/neopop/TLCS-900h/TLCS900h_interpret_src.c
    src/neopop/z80/Z80.c
    src/neopop/gfx_scanline_colour.c
    src/neopop/gfx_scanline_mono.c
    src/neopop/sound.c
    PROPERTIES COMPILE_FLAGS "-fno-function-sections -fno-data-sections"
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
    PICO_AUDIO_I2S_DATA_PIN=${PICO_AUDIO_I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${PICO_AUDIO_I2S_CLOCK_PIN_BASE}
    PICO_AUDIO_I2S_PIO=${PICO_AUDIO_I2S_PIO}
    PICO_AUDIO_I2S_DMA_IRQ=1
    PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED=${PICO_AUDIO_I2S_CLOCK_PINS_SWAPPED}
    PICO_AUDIO_I2S_RESET_PIN=${PICO_AUDIO_I2S_RESET_PIN}
    PICO_AUDIO_I2S_INTERRUPT_PIN=${PICO_AUDIO_I2S_INTERRUPT_PIN}
    PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON=${PICO_AUDIO_I2S_INTERRUPT_IS_BUTTON}
    CFG_TUH_RPI_PIO_USB=0
    PIO_USB_USE_PIO=2
    PSRAM_CS_PIN=${PSRAM_CS_PIN}
    GPIOHSTXD0=${GPIOHSTXD0}
    GPIOHSTXD1=${GPIOHSTXD1}
    GPIOHSTXD2=${GPIOHSTXD2}
    GPIOHSTXCK=${GPIOHSTXCK}
    GPIOHSTXINVERTED=${GPIOHSTXINVERTED}
    USE_ILI9341=${USE_ILI9341}
    ILI9341_PIN_CS=${ILI9341_PIN_CS}
    ILI9341_PIN_SCK=${ILI9341_PIN_SCK}
    ILI9341_PIN_MOSI=${ILI9341_PIN_MOSI}
    ILI9341_PIN_DC=${ILI9341_PIN_DC}
    ILI9341_PIN_RST=${ILI9341_PIN_RST}
    ILI9341_PIN_BL=${ILI9341_PIN_BL}
    ILI9341_SPI_INST=${ILI9341_SPI_INST}
    DMG_DEFAULT_PANEL_ST7789=${DEFAULT_PANEL_ST7789}
    PICO_FLASH_SIZE_BYTES=16777216
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
    hardware_xosc
    hardware_adc
    hardware_rosc
    hardware_pwm
    hardware_flash
    dvi
    util
    tinyusb_host 
    tinyusb_board
    pico_shared  
    xinput_host
)

if(USE_ILI9341)
    target_link_libraries(${projectname} PRIVATE ili9341)
endif()

target_include_directories(${projectname}
PRIVATE
    pico_lib
    tusb_xinput
    pico_shared
    pico_shared/drivers/ili9341
    src
    src/neopop
    src/neopop/TLCS-900h
    src/neopop/z80
    ../shared_menu
    .
)

add_subdirectory(pico_lib/dvi)
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

# Route performance-critical TLCS-900h CPU and rasterizer into SRAM
set(PICO_NGP_LDOVERRIDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/picongp_ldoverride")
file(MAKE_DIRECTORY "${PICO_NGP_LDOVERRIDE_DIR}")
file(WRITE "${PICO_NGP_LDOVERRIDE_DIR}/default_text_excludes.incl"
    "*(EXCLUDE_FILE(*TLCS900h* *gfx_scanline_colour.c* *gfx_scanline_mono.c*) .text*)\n")
file(WRITE "${PICO_NGP_LDOVERRIDE_DIR}/default_rodata_excludes.incl"
    "*(EXCLUDE_FILE(*TLCS900h*) .rodata*)\n")

if(COMMAND pico_add_linker_script_override_path)
    pico_add_linker_script_override_path(${projectname} "${PICO_NGP_LDOVERRIDE_DIR}" FILES default_text_excludes.incl default_rodata_excludes.incl)
endif()

pico_add_extra_outputs(${projectname})
