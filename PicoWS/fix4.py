import sys

content = open('src/oswan/memory.c', 'r', encoding='utf-8').read()

old_code = """        case 2:
        case 3:
            return ws_rom[offset + ((ws_ioRam[IO_ROM_BANK_BASE_SELECTOR + bank] & ((romSize >> 16) - 1)) << 16)];
        default: {
            unsigned int romBank = (256 - (((ws_ioRam[IO_ROM_BANK_BASE_SELECTOR] & 0xf) << 4) | (bank & 0xf)));
            return ws_rom[(unsigned) (offset + romSize - (romBank << 16))];
        }"""

new_code = """        case 2:
        case 3: {
            uint32_t idx = offset + ((ws_ioRam[IO_ROM_BANK_BASE_SELECTOR + bank] & ((romSize >> 16) - 1)) << 16);
            if (idx >= romSize && romSize) idx %= romSize;
            return ws_rom[idx];
        }
        default: {
            unsigned int romBank = (256 - (((ws_ioRam[IO_ROM_BANK_BASE_SELECTOR] & 0xf) << 4) | (bank & 0xf)));
            uint32_t roff = (unsigned) (offset + romSize - (romBank << 16));
            uint32_t idx = roff & romAddressMask;
            if (idx >= romSize && romSize) idx %= romSize;
            return ws_rom[idx];
        }"""

content = content.replace(old_code, new_code)
open('src/oswan/memory.c', 'w', encoding='utf-8').write(content)
