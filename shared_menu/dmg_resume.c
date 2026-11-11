#include "dmg_resume.h"
#include "dmg_menu.h"
#include "dmg_power.h"
#include "dmg_battery.h"
#include "dmg_panel.h"

#include <stdio.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "ff.h"

#define RESUME_FILE       "0:/saves/resume.bin"
#define RESUME_FILE_MAGIC 0x524D5352u   /* "RSMR" */
#define RESUME_FILE_VER   1u

typedef struct {
    uint32_t            magic;
    uint32_t            version;
    dmg_resume_record_t rec;
} resume_file_t;

bool dmg_resume_write_record(const dmg_resume_record_t *rec) {
    if (rec == NULL) return false;
    (void)f_mkdir("0:/saves");

    resume_file_t f;
    memset(&f, 0, sizeof(f));
    f.magic   = RESUME_FILE_MAGIC;
    f.version = RESUME_FILE_VER;
    f.rec     = *rec;

    FIL fil;
    if (f_open(&fil, RESUME_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[resume] cannot create %s\n", RESUME_FILE);
        return false;
    }
    UINT bw = 0;
    const FRESULT fr = f_write(&fil, &f, sizeof(f), &bw);
    f_close(&fil);
    if (fr != FR_OK || bw != sizeof(f)) {
        printf("[resume] write failed (%d, %u bytes)\n", (int)fr, (unsigned)bw);
        return false;
    }
    return true;
}

void dmg_resume_set_mark(dmg_resume_kind_t kind, uint8_t reason) {
    watchdog_hw->scratch[DMG_RESUME_SCRATCH] =
        DMG_RESUME_MAGIC_BASE | ((uint32_t)kind & 0x0Fu) | (((uint32_t)reason & 0x0Fu) << 4);
}

void dmg_resume_consume(void) {
    watchdog_hw->scratch[DMG_RESUME_SCRATCH] = 0;
}

static bool read_record(dmg_resume_record_t *out) {
    FIL fil;
    if (f_open(&fil, RESUME_FILE, FA_READ) != FR_OK) return false;
    resume_file_t f;
    memset(&f, 0, sizeof(f));
    UINT br = 0;
    const FRESULT fr = f_read(&fil, &f, sizeof(f), &br);
    f_close(&fil);
    if (fr != FR_OK || br != sizeof(f)) return false;
    if (f.magic != RESUME_FILE_MAGIC || f.version != RESUME_FILE_VER) return false;
    /* Every string is ours, but terminate them anyway: a torn write must not
     * turn into an unterminated path. */
    f.rec.app[sizeof(f.rec.app) - 1]             = '\0';
    f.rec.rom_dir[sizeof(f.rec.rom_dir) - 1]     = '\0';
    f.rec.rom_file[sizeof(f.rec.rom_file) - 1]   = '\0';
    f.rec.game_name[sizeof(f.rec.game_name) - 1] = '\0';
    *out = f.rec;
    return true;
}

dmg_resume_kind_t dmg_resume_check(const dmg_emulator_desc_t *desc, dmg_resume_record_t *out) {
    const uint32_t mark = watchdog_hw->scratch[DMG_RESUME_SCRATCH];
    if ((mark & DMG_RESUME_MAGIC_MASK) != DMG_RESUME_MAGIC_BASE) return DMG_RESUME_NONE;

    dmg_resume_kind_t kind = (dmg_resume_kind_t)(mark & 0x0Fu);
    const uint8_t reason = (uint8_t)((mark >> 4) & 0x0Fu);
    if (kind != DMG_RESUME_MENU && kind != DMG_RESUME_GAME) {
        dmg_resume_consume();
        return DMG_RESUME_NONE;
    }

    /* Woken by the low-battery standby: only carry on if a charger is in or
     * the cell has recovered past the release level. */
    if (reason == DMG_POWER_REASON_BATTERY) {
        dmg_battery_init_pin(dmg_panel_get_batt_pin());
        dmg_battery_init_charge_pin(dmg_panel_get_charge_pin());
        const bool usb = dmg_battery_usb_present();
        const uint16_t mv = dmg_battery_configured() ? dmg_battery_read_mv() : 0u;
        const bool cell = (mv >= DMG_BATTERY_PRESENT_MV);
        if (!usb && cell && mv < DMG_BATTERY_RELEASE_MV) {
            printf("[resume] battery %u mV, no charger: back to standby\n", (unsigned)mv);
            dmg_power_sleep_and_reboot(DMG_POWER_REASON_BATTERY);
        }
    }

    if (kind == DMG_RESUME_GAME) {
        dmg_resume_record_t rec;
        if (out != NULL && read_record(&rec) && desc != NULL && desc->app_title != NULL &&
            strcmp(rec.app, desc->app_title) == 0 && rec.rom_file[0] != '\0') {
            *out = rec;
            if (rec.rom_dir[0] != '\0') dmg_set_current_rom_dir(rec.rom_dir);
            printf("[resume] game: %s/%s (state '%s')\n", rec.rom_dir, rec.rom_file, rec.game_name);
            return DMG_RESUME_GAME;
        }
        printf("[resume] game mark but no usable record: back to the menu\n");
        kind = DMG_RESUME_MENU;
    }
    printf("[resume] menu\n");
    return kind;
}
