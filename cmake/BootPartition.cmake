# BootPartition.cmake - Shared flash memory map for emuLoader / pico-bootLoader.
#
# Flash layout (Waveshare RP2350 Plus 16MB / Fruit Jam 16MB):
#
#   0x10000000  +-----------------------------+  <- bootrom always boots this
#               |   Bootloader (pico-bootLoader)|
#               |        512 KB               |
#   0x10080000  +-----------------------------+  <- FRENS_APP_BASE
#               |   Application partition      |     PicoDMG UF2 lands here.
#               |        15.5 MB              |
#   0x11000000  +-----------------------------+
#

if(DEFINED _FRENS_BOOTPARTITION_INCLUDED)
    return()
endif()
set(_FRENS_BOOTPARTITION_INCLUDED 1)

set(FRENS_XIP_BASE        "0x10000000" CACHE STRING "RP2350 XIP flash base")
set(FRENS_BOOTLOADER_SIZE "0x80000"    CACHE STRING "Bytes reserved for the resident bootloader (512 KB)")
set(FRENS_FLASH_TOTAL     "0x1000000"  CACHE STRING "Total external flash on the board (16 MB)")

# Derived addresses.
math(EXPR FRENS_APP_BASE "${FRENS_XIP_BASE} + ${FRENS_BOOTLOADER_SIZE}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR FRENS_APP_SIZE  "${FRENS_FLASH_TOTAL} - ${FRENS_BOOTLOADER_SIZE}" OUTPUT_FORMAT HEXADECIMAL)

function(_frens_relink_flash TARGET ORIGIN LENGTH)
    # 1. For Pico SDK >= 2.1.0 / 2.3.1: override pico_flash_region.ld
    set(_override_dir "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_ldoverride")
    file(MAKE_DIRECTORY "${_override_dir}")
    file(WRITE "${_override_dir}/pico_flash_region.ld"
         "FLASH(rx) : ORIGIN = ${ORIGIN}, LENGTH = ${LENGTH}\n")
    if(COMMAND pico_add_linker_script_override_path)
        pico_add_linker_script_override_path(${TARGET} "${_override_dir}" FILES pico_flash_region.ld)
    endif()

    # 2. For backwards compatibility with monolithic memmap_default.ld
    set(_candidates
        "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld"
        "${PICO_SDK_PATH}/src/rp2350/pico_platform/memmap_default.ld"
        "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link/memmap_default.ld"
    )
    set(_src_ld "")
    foreach(_c IN LISTS _candidates)
        if(EXISTS "${_c}")
            set(_src_ld "${_c}")
            break()
        endif()
    endforeach()

    if(_src_ld)
        file(READ "${_src_ld}" _ld_text)
        if(_ld_text MATCHES "INCLUDE \"pico_flash_region.ld\"" OR _ld_text MATCHES "FLASH\\(rx\\)")
            string(REPLACE
                "INCLUDE \"pico_flash_region.ld\""
                "FLASH(rx) : ORIGIN = ${ORIGIN}, LENGTH = ${LENGTH}"
                _ld_text "${_ld_text}")
            string(REGEX REPLACE
                "FLASH\\(rx\\)[ \t]*:[^\n]*"
                "FLASH(rx) : ORIGIN = ${ORIGIN}, LENGTH = ${LENGTH}"
                _ld_text "${_ld_text}")

            set(_out_ld "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_frens_memmap.ld")
            file(WRITE "${_out_ld}" "${_ld_text}")
            pico_set_linker_script(${TARGET} "${_out_ld}")
        endif()
    endif()

    message(STATUS "BootPartition: ${TARGET} -> ORIGIN=${ORIGIN} LENGTH=${LENGTH}")

    target_compile_definitions(${TARGET} PRIVATE
        PICO_FLASH_SIZE_BYTES=${FRENS_FLASH_TOTAL})


endfunction()

function(frens_link_as_bootloader TARGET)
    _frens_relink_flash(${TARGET} ${FRENS_XIP_BASE} ${FRENS_BOOTLOADER_SIZE})
endfunction()

function(frens_offset_for_bootloader TARGET)
    _frens_relink_flash(${TARGET} ${FRENS_APP_BASE} ${FRENS_APP_SIZE})
endfunction()
