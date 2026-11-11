import sys

content = open('src/oswan/memory.c', 'r', encoding='utf-8').read()

old_code = """void ws_memory_init(uint8 *rom, uint32 size) {
    ws_rom = rom;
    romSize = size;"""

new_code = """void ws_memory_init(uint8 *rom, uint32 size) {
    ws_rom = rom;
    romSize = size;
    memset(ws_staticRam, 0xFF, sizeof(ws_staticRam));
    memset(externalEeprom, 0xFF, sizeof(externalEeprom));"""

content = content.replace(old_code, new_code)
open('src/oswan/memory.c', 'w', encoding='utf-8').write(content)
