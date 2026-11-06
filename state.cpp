#include "state.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ff.h"

extern "C" {
#include "gwenesis_savestate.h"
}

#define STATE_MAGIC "GWMD_SS1"
#define STATE_MAGIC_LEN 8

struct SaveState {
    char section[32];
    bool is_write;
};

static SaveState s_active_state;
static FIL s_state_file;
static bool s_file_open = false;
static FSIZE_t s_section_start_offset = STATE_MAGIC_LEN;

struct StateRecordHeader {
    uint8_t tag_len;
    char tag[31];
    uint8_t is_buf;
    uint32_t len_or_val;
};

extern "C" {

SaveState *saveGwenesisStateOpenForWrite(const char *fileName) {
    if (fileName) {
        strncpy(s_active_state.section, fileName, sizeof(s_active_state.section) - 1);
        s_active_state.section[sizeof(s_active_state.section) - 1] = '\0';
    } else {
        s_active_state.section[0] = '\0';
    }
    s_active_state.is_write = true;

    if (s_file_open) {
        StateRecordHeader rec;
        memset(&rec, 0, sizeof(rec));
        const char *sec_tag = "#SEC";
        rec.tag_len = (uint8_t)strlen(sec_tag);
        strncpy(rec.tag, sec_tag, sizeof(rec.tag) - 1);
        rec.is_buf = 1;
        rec.len_or_val = (uint32_t)strlen(s_active_state.section);
        UINT bw;
        f_write(&s_state_file, &rec, sizeof(rec), &bw);
        if (rec.len_or_val > 0) {
            f_write(&s_state_file, s_active_state.section, rec.len_or_val, &bw);
        }
    }
    return &s_active_state;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value) {
    (void)state;
    if (!s_file_open || !tagName) return;

    StateRecordHeader rec;
    memset(&rec, 0, sizeof(rec));
    size_t tlen = strlen(tagName);
    if (tlen > 30) tlen = 30;
    rec.tag_len = (uint8_t)tlen;
    memcpy(rec.tag, tagName, tlen);
    rec.tag[tlen] = '\0';
    rec.is_buf = 0;
    rec.len_or_val = (uint32_t)value;

    UINT bw;
    f_write(&s_state_file, &rec, sizeof(rec), &bw);
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName, void *buffer, int length) {
    (void)state;
    if (!s_file_open || !tagName || !buffer || length <= 0) return;

    StateRecordHeader rec;
    memset(&rec, 0, sizeof(rec));
    size_t tlen = strlen(tagName);
    if (tlen > 30) tlen = 30;
    rec.tag_len = (uint8_t)tlen;
    memcpy(rec.tag, tagName, tlen);
    rec.tag[tlen] = '\0';
    rec.is_buf = 1;
    rec.len_or_val = (uint32_t)length;

    UINT bw;
    f_write(&s_state_file, &rec, sizeof(rec), &bw);
    f_write(&s_state_file, buffer, (UINT)length, &bw);
}

SaveState *saveGwenesisStateOpenForRead(const char *fileName) {
    if (fileName) {
        strncpy(s_active_state.section, fileName, sizeof(s_active_state.section) - 1);
        s_active_state.section[sizeof(s_active_state.section) - 1] = '\0';
    } else {
        s_active_state.section[0] = '\0';
    }
    s_active_state.is_write = false;

    if (s_file_open) {
        f_lseek(&s_state_file, STATE_MAGIC_LEN);
        FSIZE_t fsz = f_size(&s_state_file);

        while (f_tell(&s_state_file) + sizeof(StateRecordHeader) <= fsz) {
            StateRecordHeader rec;
            UINT br = 0;
            if (f_read(&s_state_file, &rec, sizeof(rec), &br) != FR_OK || br != sizeof(rec)) break;

            if (strcmp(rec.tag, "#SEC") == 0 && rec.is_buf) {
                char sec_name[32];
                UINT to_read = (rec.len_or_val < sizeof(sec_name) - 1) ? rec.len_or_val : (UINT)(sizeof(sec_name) - 1);
                memset(sec_name, 0, sizeof(sec_name));
                if (f_read(&s_state_file, sec_name, to_read, &br) == FR_OK && br == to_read) {
                    if (rec.len_or_val > to_read) {
                        f_lseek(&s_state_file, f_tell(&s_state_file) + (rec.len_or_val - to_read));
                    }
                    if (strcmp(sec_name, s_active_state.section) == 0) {
                        s_section_start_offset = f_tell(&s_state_file);
                        break;
                    }
                }
            } else if (rec.is_buf && rec.len_or_val > 0) {
                f_lseek(&s_state_file, f_tell(&s_state_file) + rec.len_or_val);
            }
        }
    }
    return &s_active_state;
}

static bool find_next_record(const char *tagName, StateRecordHeader *rec_out) {
    if (!s_file_open || !tagName || !rec_out) return false;
    FSIZE_t fsz = f_size(&s_state_file);

    for (int pass = 0; pass < 2; pass++) {
        while (f_tell(&s_state_file) + sizeof(StateRecordHeader) <= fsz) {
            StateRecordHeader rec;
            UINT br = 0;
            if (f_read(&s_state_file, &rec, sizeof(rec), &br) != FR_OK || br != sizeof(rec)) break;

            if (strcmp(rec.tag, "#SEC") == 0) {
                break;
            }

            if (strcmp(rec.tag, tagName) == 0) {
                *rec_out = rec;
                return true;
            }

            if (rec.is_buf && rec.len_or_val > 0) {
                f_lseek(&s_state_file, f_tell(&s_state_file) + rec.len_or_val);
            }
        }
        f_lseek(&s_state_file, s_section_start_offset);
    }
    return false;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName) {
    (void)state;
    StateRecordHeader rec;
    if (find_next_record(tagName, &rec)) {
        return (int)rec.len_or_val;
    }
    return 0;
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName, void *buffer, int length) {
    (void)state;
    if (!buffer || length <= 0) return;
    StateRecordHeader rec;
    if (find_next_record(tagName, &rec) && rec.is_buf) {
        UINT copy_len = (rec.len_or_val < (uint32_t)length) ? (UINT)rec.len_or_val : (UINT)length;
        UINT br = 0;
        f_read(&s_state_file, buffer, copy_len, &br);
        if (rec.len_or_val > copy_len) {
            f_lseek(&s_state_file, f_tell(&s_state_file) + (rec.len_or_val - copy_len));
        }
    }
}

bool initLoadGwenesisState(unsigned char *srcBuffer) {
    (void)srcBuffer;
    return false;
}

int saveGwenesisState(unsigned char *destBuffer, int save_size) {
    (void)destBuffer;
    (void)save_size;
    return 0;
}

int loadGwenesisState(unsigned char *srcBuffer) {
    (void)srcBuffer;
    return 0;
}

} // extern "C"

int Emulator_SaveState(const char *path) {
    if (!path) return -1;

    FRESULT fr = f_open(&s_state_file, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("[E] Cannot create save state file: %d (%s)\n", fr, path);
        return -1;
    }

    UINT bw;
    f_write(&s_state_file, STATE_MAGIC, STATE_MAGIC_LEN, &bw);

    s_file_open = true;
    gwenesis_save_state();
    s_file_open = false;

    FRESULT frc = f_close(&s_state_file);
    if (frc != FR_OK) {
        printf("[E] Error closing save state file: %d (%s)\n", frc, path);
        return -1;
    }
    printf("[I] Save state saved successfully (%s)\n", path);
    return 0;
}

int Emulator_LoadState(const char *path) {
    if (!path) return -1;

    FRESULT fr = f_open(&s_state_file, path, FA_READ);
    if (fr != FR_OK) {
        printf("[W] Cannot open load state file: %d (%s)\n", fr, path);
        return -1;
    }

    FSIZE_t fsz = f_size(&s_state_file);
    if (fsz < STATE_MAGIC_LEN) {
        printf("[E] Invalid save state file size: %lu\n", (unsigned long)fsz);
        f_close(&s_state_file);
        return -1;
    }

    char magic[STATE_MAGIC_LEN];
    UINT br = 0;
    if (f_read(&s_state_file, magic, STATE_MAGIC_LEN, &br) != FR_OK || br != STATE_MAGIC_LEN ||
        memcmp(magic, STATE_MAGIC, STATE_MAGIC_LEN) != 0) {
        printf("[E] Save state magic mismatch\n");
        f_close(&s_state_file);
        return -1;
    }

    s_file_open = true;
    s_section_start_offset = STATE_MAGIC_LEN;
    gwenesis_load_state();
    s_file_open = false;

    f_close(&s_state_file);
    printf("[I] State loaded successfully (%s)\n", path);
    return 0;
}
