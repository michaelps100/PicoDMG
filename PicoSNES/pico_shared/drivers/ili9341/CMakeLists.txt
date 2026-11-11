add_library(ili9341 STATIC
    ili9341.c
)

target_include_directories(ili9341 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../..
    # The driver calls into shared_menu/dmg_panel.h (runtime MADCTL + ST7789
    # timing) and dmg_menu.h (volume HUD rows, GB window geometry).
    ${CMAKE_CURRENT_LIST_DIR}/../../../../shared_menu
)

target_link_libraries(ili9341 PUBLIC
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_gpio
    hardware_sync
)

if (ILI9341_EXTERNAL_FB)
    target_compile_definitions(ili9341 PUBLIC ILI9341_EXTERNAL_FB=1)
endif()
