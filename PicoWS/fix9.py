import sys

content = open('src/oswan/memory.c', 'r', encoding='utf-8').read()

old_mask = """    uint32_t sramSize = ws_rom_sramSize(ws_rom, romSize);
    sramAddressMask = sramSize > 0 ? sramSize - 1 : 0;
    uint32_t eepromSize = ws_rom_eepromSize(ws_rom, romSize);
    externalEepromAddressMask = eepromSize > 0 ? eepromSize - 1 : 0;
    romAddressMask = romSize - 1;"""

new_mask = """    uint32_t sramSize = ws_rom_sramSize(ws_rom, romSize);
    uint32_t m = 1; while(m < sramSize && sramSize > 0) m <<= 1;
    sramAddressMask = sramSize > 0 ? m - 1 : 0;
    
    uint32_t eepromSize = ws_rom_eepromSize(ws_rom, romSize);
    m = 1; while(m < eepromSize && eepromSize > 0) m <<= 1;
    externalEepromAddressMask = eepromSize > 0 ? m - 1 : 0;
    
    m = 1; while(m < romSize && romSize > 0) m <<= 1;
    romAddressMask = romSize > 0 ? m - 1 : 0;"""

content = content.replace(old_mask, new_mask)
open('src/oswan/memory.c', 'w', encoding='utf-8').write(content)
