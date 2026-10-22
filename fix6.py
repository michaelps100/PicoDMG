import sys

content = open('src/oswan/io.c', 'r', encoding='utf-8').read()

old_code = """        case 0xb5:
            w1 = ws_ioRam[0xb5];
            if (w1 & 0x40) {
                if (ws_key_flipped)
                    w2 = (ws_key_start << 1);
                else
                    w2 = (ws_key_start << 1) | (ws_key_button_1 << 2) | (ws_key_button_2 << 3);
                return (uint8) ((w1 & 0xf0) | w2);
            }
            if (w1 & 0x20) {
                if (ws_key_flipped)
                    w2 = (ws_key_button_1) | (ws_key_button_2 << 2);
                else
                    w2 = (ws_key_up << 0) | (ws_key_right << 1) | (ws_key_down << 2) | (ws_key_left << 3);
                return (uint8) ((w1 & 0xf0) | w2);
            }
            if (w1 & 0x10) {
                w2 = 0x00;
                if (ws_key_flipped)
                    w2 = (ws_key_up << 1) | (ws_key_right << 2) | (ws_key_down << 3) | (ws_key_left);
                return (uint8) ((w1 & 0xf0) | w2);
            }
            break;"""

new_code = """        case 0xb5:
            w1 = ws_ioRam[0xb5];
            if (w1 & 0x40) {
                w2 = (ws_key_start << 1) | (ws_key_button_1 << 2) | (ws_key_button_2 << 3);
                return (uint8) ((w1 & 0xf0) | w2);
            }
            if ((w1 & 0x20) || (w1 & 0x10)) {
                w2 = (ws_key_up << 0) | (ws_key_right << 1) | (ws_key_down << 2) | (ws_key_left << 3);
                return (uint8) ((w1 & 0xf0) | w2);
            }
            break;"""

content = content.replace(old_code, new_code)
open('src/oswan/io.c', 'w', encoding='utf-8').write(content)
