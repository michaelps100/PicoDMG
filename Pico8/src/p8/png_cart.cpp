#include "png_cart.hpp"
#include <emu/emulator.hpp>
#include <emu/bufferio.hpp>
#include <devices/drawpalette.hpp>
#include <devices/drawstatemisc.hpp>
#include <devices/screenpalette.hpp>
#include <devices/clippingrectangle.hpp>
#include <devices/sfx.hpp>
#include <devices/music.hpp>
#include "platform/picodmg/p8_audio.h"
#include <hardware/watchdog.h>
#include <ff.h>
#include <zlib.h>

#include <vector>
#include <array>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace p8 {

static std::string decompress_c(const uint8_t *data, size_t data_len, uint16_t expected_len) {
    static const char *lut = "\n 0123456789abcdefghijklmnopqrstuvwxyz!#%(){}[]<>+=/*:;.,~_";
    std::string out;
    out.reserve(expected_len);
    size_t pos = 0;
    while (out.size() < expected_len && pos < data_len) {
        uint8_t b = data[pos++];
        if (b == 0) {
            if (pos < data_len) {
                out.push_back((char)data[pos++]);
            }
        } else if (b <= 0x3b) {
            out.push_back(lut[b - 1]);
        } else {
            if (pos < data_len) {
                uint8_t b2 = data[pos++];
                uint16_t offset = (uint16_t)(b - 0x3c) * 16 + (b2 & 0xf);
                uint16_t length = (uint16_t)(b2 >> 4) + 2;
                if (offset <= out.size()) {
                    size_t start = out.size() - offset;
                    for (uint16_t i = 0; i < length; i++) {
                        out.push_back(out[start + i]);
                    }
                }
            }
        }
    }
    return out;
}

static std::string decompress_pxa(const uint8_t *data, size_t data_len, uint16_t expected_len) {
    size_t bit_pos = 0;
    auto read_bit = [&]() -> uint8_t {
        size_t byte_idx = bit_pos >> 3;
        uint8_t bit_idx = (uint8_t)(bit_pos & 7);
        bit_pos++;
        if (byte_idx >= data_len) return 0;
        return (data[byte_idx] >> bit_idx) & 1;
    };
    auto read_bits = [&](int n) -> uint32_t {
        uint32_t val = 0;
        for (int i = 0; i < n; i++) {
            val |= ((uint32_t)read_bit() << i);
        }
        return val;
    };

    uint8_t mtf[256];
    for (int i = 0; i < 256; i++) mtf[i] = (uint8_t)i;

    std::string out;
    out.reserve(expected_len);

    while (out.size() < expected_len && (bit_pos >> 3) < data_len) {
        uint8_t head = read_bit();
        if (head == 1) {
            int unary = 0;
            while (read_bit() == 1) {
                unary++;
            }
            uint32_t unary_mask = (1u << unary) - 1u;
            uint32_t index = read_bits(4 + unary) + (unary_mask << 4);
            if (index >= 256) break;
            uint8_t val = mtf[index];
            out.push_back((char)val);
            std::memmove(&mtf[1], &mtf[0], index);
            mtf[0] = val;
        } else {
            uint8_t b1 = read_bit();
            int offset_bits;
            if (b1 == 1) {
                uint8_t b2 = read_bit();
                offset_bits = (b2 == 1) ? 5 : 10;
            } else {
                offset_bits = 15;
            }
            uint32_t offset = read_bits(offset_bits) + 1;
            if (offset_bits == 10 && offset == 1) {
                // Bloco descomprimido (texto direto ate '\0')
                while (true) {
                    uint8_t ch = (uint8_t)read_bits(8);
                    if (ch == 0) break;
                    out.push_back((char)ch);
                }
            } else {
                uint32_t length = 3;
                while (true) {
                    uint32_t part = read_bits(3);
                    length += part;
                    if (part != 7) break;
                }
                if (offset <= out.size()) {
                    size_t start = out.size() - offset;
                    for (uint32_t i = 0; i < length; i++) {
                        out.push_back(out[start + i]);
                    }
                }
            }
        }
    }
    return out;
}

bool is_png_cart(std::string_view cart_path) {
    std::string path_str(cart_path);
    FIL fil;
    if (f_open(&fil, path_str.c_str(), FA_READ) != FR_OK) {
        return false;
    }
    uint8_t sig[8] = {0};
    UINT br = 0;
    FRESULT fr = f_read(&fil, sig, 8, &br);
    f_close(&fil);
    if (fr != FR_OK || br < 8) return false;
    return std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0;
}

bool load_png_cart(std::string_view cart_path, emu::Emulator &emulator) {
    std::string path_str(cart_path);
    FIL fil;
    if (f_open(&fil, path_str.c_str(), FA_READ) != FR_OK) {
        printf("[Pico8] PNG: Nao foi possivel abrir %s\n", path_str.c_str());
        return false;
    }

    uint8_t sig[8];
    UINT br = 0;
    if (f_read(&fil, sig, 8, &br) != FR_OK || br != 8 ||
        std::memcmp(sig, "\x89PNG\r\n\x1a\n", 8) != 0) {
        printf("[Pico8] PNG: Assinatura PNG invalida\n");
        f_close(&fil);
        return false;
    }

    // Le cabecalho do chunk IHDR
    uint8_t chunk_hdr[8];
    if (f_read(&fil, chunk_hdr, 8, &br) != FR_OK || br != 8) {
        f_close(&fil);
        return false;
    }
    uint32_t ihdr_len = ((uint32_t)chunk_hdr[0] << 24) | ((uint32_t)chunk_hdr[1] << 16) |
                        ((uint32_t)chunk_hdr[2] << 8)  | (uint32_t)chunk_hdr[3];
    if (std::memcmp(&chunk_hdr[4], "IHDR", 4) != 0 || ihdr_len < 13) {
        f_close(&fil);
        return false;
    }

    uint8_t ihdr[13];
    if (f_read(&fil, ihdr, 13, &br) != FR_OK || br != 13) {
        f_close(&fil);
        return false;
    }
    uint32_t w = ((uint32_t)ihdr[0] << 24) | ((uint32_t)ihdr[1] << 16) | ((uint32_t)ihdr[2] << 8) | ihdr[3];
    uint32_t h = ((uint32_t)ihdr[4] << 24) | ((uint32_t)ihdr[5] << 16) | ((uint32_t)ihdr[6] << 8) | ihdr[7];
    uint8_t depth = ihdr[8];
    uint8_t ctype = ihdr[9];
    if (w != 160 || h != 205 || depth != 8 || ctype != 6) {
        printf("[Pico8] PNG: Dimensoes invalidas (%lux%lu d=%u c=%u)\n", (unsigned long)w, (unsigned long)h, depth, ctype);
        f_close(&fil);
        return false;
    }
    // Pula resto do IHDR + CRC (4 bytes)
    f_lseek(&fil, f_tell(&fil) + (ihdr_len - 13) + 4);

    // Inicializa streaming do zlib
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK) {
        f_close(&fil);
        return false;
    }

    // Buffers compactos para evitar estouro de memoria SRAM
    uint8_t in_buf[1024];
    uint8_t scanline_raw[641];
    uint8_t curr_row[640];
    uint8_t prev_row[640];
    std::memset(prev_row, 0, sizeof(prev_row));

    // Grava dados de hardware diretamente na memoria do emulador (0x0000..0x42ff)
    emulator.memory().fill(0, 0, 0x4300);
    emulator.memory().fill(0, 0x6000, 0x2000);
    emu::device<devices::DrawPalette>.reset();
    emu::device<devices::DrawStateMisc>.reset();
    emu::device<devices::ScreenPalette>.reset();
    emu::device<devices::ClippingRectangle>.reset();
    uint8_t *cart_mem = emulator.memory().raw_view.data();

    // Armazena somente o codigo Lua comprimido (~15 KB)
    std::vector<uint8_t> code_buf;
    code_buf.reserve(16384);

    uint32_t cur_chunk_rem = 0;
    bool in_idat = false;

    auto fetch_input = [&]() -> bool {
        while (strm.avail_in == 0) {
            if (cur_chunk_rem > 0) {
                uint32_t to_read = (cur_chunk_rem < sizeof(in_buf)) ? cur_chunk_rem : (uint32_t)sizeof(in_buf);
                if (f_read(&fil, in_buf, to_read, &br) != FR_OK || br == 0) {
                    return false;
                }
                cur_chunk_rem -= br;
                strm.next_in = in_buf;
                strm.avail_in = br;
                return true;
            }

            if (in_idat) {
                f_lseek(&fil, f_tell(&fil) + 4); // Pula CRC do chunk IDAT anterior
                in_idat = false;
            }

            uint8_t chdr[8];
            if (f_read(&fil, chdr, 8, &br) != FR_OK || br != 8) {
                return false;
            }
            uint32_t clen = ((uint32_t)chdr[0] << 24) | ((uint32_t)chdr[1] << 16) |
                            ((uint32_t)chdr[2] << 8)  | (uint32_t)chdr[3];
            char ctype_str[5];
            std::memcpy(ctype_str, &chdr[4], 4);
            ctype_str[4] = '\0';

            if (std::strcmp(ctype_str, "IDAT") == 0) {
                cur_chunk_rem = clen;
                in_idat = true;
            } else if (std::strcmp(ctype_str, "IEND") == 0) {
                return false;
            } else {
                f_lseek(&fil, f_tell(&fil) + clen + 4);
            }
        }
        return true;
    };

    // Decodifica scanline por scanline (205 linhas)
    for (int y = 0; y < 205; y++) {
        watchdog_update();

        strm.next_out = scanline_raw;
        strm.avail_out = 641;

        while (strm.avail_out > 0) {
            if (strm.avail_in == 0) {
                if (!fetch_input()) {
                    inflateEnd(&strm);
                    f_close(&fil);
                    return false;
                }
            }
            int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                printf("[Pico8] PNG: inflate erro %d na linha %d\n", ret, y);
                inflateEnd(&strm);
                f_close(&fil);
                return false;
            }
            if (ret == Z_STREAM_END && strm.avail_out > 0) {
                break;
            }
        }

        uint8_t filter_type = scanline_raw[0];
        const uint8_t *row_raw = &scanline_raw[1];

        for (int x = 0; x < 640; x++) {
            uint8_t val = row_raw[x];
            uint8_t a = (x >= 4) ? curr_row[x - 4] : 0;
            uint8_t b = (y > 0) ? prev_row[x] : 0;
            uint8_t c = (y > 0 && x >= 4) ? prev_row[x - 4] : 0;
            uint8_t filt = 0;
            switch (filter_type) {
                case 0: filt = val; break;
                case 1: filt = (uint8_t)(val + a); break;
                case 2: filt = (uint8_t)(val + b); break;
                case 3: filt = (uint8_t)(val + ((a + b) >> 1)); break;
                case 4: {
                    int p = (int)a + (int)b - (int)c;
                    int pa = std::abs(p - (int)a);
                    int pb = std::abs(p - (int)b);
                    int pc = std::abs(p - (int)c);
                    int pr = (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c);
                    filt = (uint8_t)(val + pr);
                    break;
                }
                default: filt = val; break;
            }
            curr_row[x] = filt;
        }

        for (int px = 0; px < 160; px++) {
            int byte_idx = y * 160 + px;
            uint8_t r = curr_row[px * 4 + 0];
            uint8_t g = curr_row[px * 4 + 1];
            uint8_t b = curr_row[px * 4 + 2];
            uint8_t a = curr_row[px * 4 + 3];
            uint8_t cart_byte = (uint8_t)(((a & 3) << 6) | ((r & 3) << 4) | ((g & 3) << 2) | (b & 3));

            if (byte_idx < 0x4300) {
                cart_mem[byte_idx] = cart_byte;
            } else {
                code_buf.push_back(cart_byte);
            }
        }

        std::memcpy(prev_row, curr_row, sizeof(curr_row));
    }

    inflateEnd(&strm);
    f_close(&fil);

    // Descompressao do codigo Lua
    watchdog_update();
    const uint8_t *code_data = code_buf.data();
    size_t avail = code_buf.size();
    std::string lua_code;

    if (avail >= 4 && std::memcmp(code_data, ":c:\0", 4) == 0) {
        uint16_t code_len = ((uint16_t)code_data[4] << 8) | code_data[5];
        lua_code = decompress_c(code_data + 8, avail - 8, code_len);
    } else if (avail >= 4 && std::memcmp(code_data, "\0pxa", 4) == 0) {
        uint16_t code_len = ((uint16_t)code_data[4] << 8) | code_data[5];
        lua_code = decompress_pxa(code_data + 8, avail - 8, code_len);
    } else {
        size_t len = 0;
        while (len < avail && code_data[len] != '\0') {
            len++;
        }
        lua_code.assign((const char *)code_data, len);
    }

    // Em PICO-8, o codigo do cartucho termina no primeiro caractere nulo ('\0') ou EOF
    auto null_pos = lua_code.find('\0');
    if (null_pos != std::string::npos) {
        lua_code.erase(null_pos);
    }

    printf("[Pico8] Cartucho PNG decodificado com sucesso! Codigo Lua: %zu bytes\n", lua_code.size());

    // Injeta o codigo original diretamente no interpretador Lua do yocto-8
    // (o parser do yocto-8 ja tem suporte nativo a +=, -=, !=, if curto, ?, etc.)
    watchdog_update();
    emu::StringReader code_reader{lua_code};
    if (!emulator.load_and_inject_header(code_reader.get_reader())) {
        return false;
    }

    emu::device<devices::DrawPalette>.reset();
    emu::device<devices::DrawStateMisc>.reset();
    emu::device<devices::ScreenPalette>.reset();
    emu::device<devices::ClippingRectangle>.reset();

    p8_audio_set_memory(
        emulator.memory().raw_view.data() + devices::Sfx::default_map_address,
        emulator.memory().raw_view.data() + devices::Music::default_map_address
    );

    return true;
}

} // namespace p8
