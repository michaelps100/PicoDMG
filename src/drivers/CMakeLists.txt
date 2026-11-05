# Força o sub-projeto da tela a usar arquivos de resposta para encurtar caminhos no Windows
set(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1 CACHE INTERNAL "")
set(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1 CACHE INTERNAL "")
set(CMAKE_ASM_USE_RESPONSE_FILE_FOR_OBJECTS 1 CACHE INTERNAL "")

add_library(ili9341 STATIC
    ili9341/ili9341.c
)

target_include_directories(ili9341 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(ili9341 PUBLIC
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_gpio
)
