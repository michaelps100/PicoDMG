cmake_minimum_required(VERSION 3.13)

add_library(shared_menu STATIC
    dmg_menu.c
    dmg_panel.c
    dmg_combo.c
    dmg_power.c
    dmg_battery.c
    dmg_resume.c
    dmg_boot_intro.c
)

target_include_directories(shared_menu PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../pico-bootLoader/src
)

target_link_libraries(shared_menu PUBLIC
    pico_stdlib
    hardware_gpio
    hardware_watchdog
    hardware_clocks
    hardware_xosc
    hardware_adc
    hardware_rosc
    hardware_pll
    hardware_vreg
    hardware_spi
    pico_multicore
    pico_fatfs
)

