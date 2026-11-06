cmake_minimum_required(VERSION 3.13)
set(projectname PicoWS)

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

# ---------------------------------------------------------------------------
# WonderSwan / Oswan Core Sources
# ---------------------------------------------------------------------------
set(OSWAN_SRC
    src/oswan/ws.c
    src/oswan/rom.c
    src/oswan/memory.c
    src/oswan/io.c
    src/oswan/gpu.c
    src/oswan/ws_audio.c
    src/oswan/ws_snd_trace.c
    src/oswan/nec/nec.c
)

add_executable(${projectname}
    src/main.cpp
    src/ws_state_io.cpp
    ${OSWAN_SRC}
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_menu.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_panel.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_combo.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_power.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_battery.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_resume.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_boot_intro.c
)

# Test builds: -DWS_EXTRA_DEFS="WS_NO_IDLE_SKIP;..." (see build_variant.ps1)
if(WS_EXTRA_DEFS)
    target_compile_definitions(${projectname} PRIVATE ${WS_EXTRA_DEFS})
endif()

pico_set_program_name(${projectname} "PicoWS")
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
    -funroll-loops
    -ffast-math
    -feliminate-unused-debug-types
    -Ofast
)

set_source_files_properties(
    src/oswan/nec/nec.c
    src/oswan/gpu.c
    src/oswan/memory.c
    src/oswan/ws.c
    src/oswan/ws_audio.c
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
    I2S_AUDIO_RING_SIZE=2048
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
    src/oswan
    src/oswan/nec
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu
    .
)

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

# Route performance-critical NEC V30 CPU, memory, and dispatch into SRAM (keep 123KB gpu.c in Flash XIP to free SRAM)
set(PICO_WS_LDOVERRIDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/picows_ldoverride")
file(MAKE_DIRECTORY "${PICO_WS_LDOVERRIDE_DIR}")
file(WRITE "${PICO_WS_LDOVERRIDE_DIR}/default_text_excludes.incl"
    "*(EXCLUDE_FILE(*libgcc.a: *libm.a: *nec.c.obj *nec.c.o *memory.c.obj *memory.c.o *ws.c.obj *ws.c.o *ws_audio.c.obj *ws_audio.c.o) .text*)\n")
file(WRITE "${PICO_WS_LDOVERRIDE_DIR}/default_rodata_excludes.incl"
    "*(EXCLUDE_FILE(*libgcc.a: *libm.a: *nec.c.obj *nec.c.o) .rodata*)\n")


if(COMMAND pico_add_linker_script_override_path)
    pico_add_linker_script_override_path(${projectname} "${PICO_WS_LDOVERRIDE_DIR}" FILES default_text_excludes.incl default_rodata_excludes.incl)
endif()

pico_add_extra_outputs(${projectname})
