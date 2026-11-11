import sys

content = open('src/oswan/memory.c', 'r', encoding='utf-8').read()

old_write = """    if (bank == 1)
        ws_staticRam[offset & sramAddressMask] = value;"""

new_write = """    if (bank == 1) {
        uint32_t sramOffset = ((uint32_t)ws_ioRam[0xC1] << 16) | offset;
        ws_staticRam[sramOffset & sramAddressMask] = value;
    }"""

old_read = """        case 1:    // 1 - SRAM (cart)
            return ws_staticRam[offset & sramAddressMask];"""

new_read = """        case 1: {   // 1 - SRAM (cart)
            uint32_t sramOffset = ((uint32_t)ws_ioRam[0xC1] << 16) | offset;
            return ws_staticRam[sramOffset & sramAddressMask];
        }"""

content = content.replace(old_write, new_write)
content = content.replace(old_read, new_read)
open('src/oswan/memory.c', 'w', encoding='utf-8').write(content)
