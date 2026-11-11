cmake_minimum_required(VERSION 3.13)
set(projectname PicoMD)

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

if(USE_ILI9341 AND NOT HW_CONFIG)
    set(HW_CONFIG 15 CACHE STRING "Select the hardware configuration for your board")      
elseif(NOT HW_CONFIG)
    set(HW_CONFIG 8 CACHE STRING "Select the hardware configuration for your board")      
endif()

if(NOT USE_HSTX)
    set(USE_HSTX 0 CACHE STRING "Use HSTX for display output when possible")
endif()

include("pico_shared/BoardConfigs.cmake")

# The HW_CONFIG 16 console block already sets the ST7789 fallback, but pass it
# explicitly too so the build does not depend on that coupling. The real panel
# selection is at runtime: shared_menu/dmg_panel.c reads ST7789= from /boot.txt,
# and DMG_DEFAULT_PANEL_ST7789 only covers a card with no /boot.txt at all.
if(HW_CONFIG EQUAL 16 AND NOT DEFAULT_PANEL_ST7789)
    set(DEFAULT_PANEL_ST7789 1 CACHE BOOL "Assume an ST7789 panel when /boot.txt does not say")
    message("HW_CONFIG 16: defaulting to the ST7789 panel when /boot.txt is absent")
endif()

if(NOT PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Board type")
    message("PICO_BOARD not set, using default: ${PICO_BOARD}")
endif()

add_definitions(-DNDEBUG)

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
message("PIO USB support: ${ENABLE_PIO_USB}, using PIO: ${PIO_USB_USE_PIO}")

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
    port/buffers.c
    port/gwsnd_core0.c
    port/gwsnd_core1.c
    port/gwsnd_resample.c
    port/gwsnd_shadow.c
    port/gwsram.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_menu.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_panel.c
${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_combo.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_power.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_battery.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_resume.c
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu/dmg_boot_intro.c
)

pico_set_program_name(${projectname} "PicoMD")
pico_set_program_version(${projectname} "1.0")

pico_enable_stdio_uart(${projectname} 1)
pico_enable_stdio_usb(${projectname} 0)

if(NOT DEFINED GENESIS_CART_SRAM)
    set(GENESIS_CART_SRAM 1 CACHE STRING "Emulate cartridge save RAM (1) or compile it out for perf A/B (0)")
endif()
message("Cartridge save RAM: ${GENESIS_CART_SRAM}")

target_compile_definitions(${projectname} PRIVATE
    PICO_RP2040_USB_DEVICE_ENUMERATION_FIX=1
)

target_compile_definitions(${projectname} PUBLIC
    GENESIS_CART_SRAM=${GENESIS_CART_SRAM}
    GWENESIS_PICO=1
    GWENESIS_PIXEL_FMT=565
    GWSND_OFFLOAD=1
)

target_compile_options(${projectname} PRIVATE
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-cast-function-type
    -Wno-maybe-uninitialized
)

set_source_files_properties(
    port/gwsnd_core0.c
    port/gwsnd_core1.c
    port/gwsnd_resample.c
    PROPERTIES COMPILE_OPTIONS "-O3;-fno-strict-aliasing"
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
    CFG_TUH_RPI_PIO_USB=${ENABLE_PIO_USB}
    PIO_USB_USE_PIO=${PIO_USB_USE_PIO}
    PSRAM_CS_PIN=${PSRAM_CS_PIN}
    GPIOHSTXD0=${GPIOHSTXD0}
    GPIOHSTXD1=${GPIOHSTXD1}
    GPIOHSTXD2=${GPIOHSTXD2}
    GPIOHSTXCK=${GPIOHSTXCK}
    GPIOHSTXINVERTED=${GPIOHSTXINVERTED}
    ENABLE_VU_METER=${ENABLE_VU_METER}
    ENABLEDVI=1
    USE_ILI9341=${USE_ILI9341}
    ILI9341_PIN_CS=${ILI9341_PIN_CS}
    ILI9341_PIN_SCK=${ILI9341_PIN_SCK}
    ILI9341_PIN_MOSI=${ILI9341_PIN_MOSI}
    ILI9341_PIN_DC=${ILI9341_PIN_DC}
    ILI9341_PIN_RST=${ILI9341_PIN_RST}
    ILI9341_PIN_BL=${ILI9341_PIN_BL}
    ILI9341_SPI_INST=${ILI9341_SPI_INST}
    PICO_FLASH_SIZE_BYTES=16777216
)

if(NOT PICO_PLATFORM STREQUAL "rp2040")
    message(STATUS "Using PICO_PIO_USE_GPIO_BASE=1 for non-rp2040 platforms.")
    target_compile_definitions(${projectname} PRIVATE
        PICO_PIO_USE_GPIO_BASE=1
        PICO_STACK_SIZE=3072
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
    gwenesis
)

if(USE_ILI9341)
    target_link_libraries(${projectname} PRIVATE ili9341)
endif()

if(PICO_BOARD STREQUAL "pico_w" OR PICO_BOARD STREQUAL "pico2_w")
    target_link_libraries(${projectname} PRIVATE pico_cyw43_arch_none)
endif()

target_include_directories(${projectname}
PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../shared_menu
    pico_lib
    tusb_xinput
    pico_shared
    pico_shared/drivers/ili9341
    port
    gwenesis
    .
)

add_subdirectory(pico_lib/dvi)
add_subdirectory(pico_lib/util)
add_subdirectory(pico_shared)
add_subdirectory(gwenesis)
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
