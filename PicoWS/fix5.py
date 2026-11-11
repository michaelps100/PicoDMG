import sys

content = open('src/oswan/memory.c', 'r', encoding='utf-8').read()

old_code = """    sramAddressMask = ws_rom_sramSize(ws_rom, romSize) - 1;
    externalEepromAddressMask = ws_rom_eepromSize(ws_rom, romSize) - 1;"""

new_code = """    uint32_t sramSize = ws_rom_sramSize(ws_rom, romSize);
    sramAddressMask = sramSize > 0 ? sramSize - 1 : 0;
    uint32_t eepromSize = ws_rom_eepromSize(ws_rom, romSize);
    externalEepromAddressMask = eepromSize > 0 ? eepromSize - 1 : 0;"""

content = content.replace(old_code, new_code)
open('src/oswan/memory.c', 'w', encoding='utf-8').write(content)
