#ifndef PICO8_MENU_H
#define PICO8_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "dmg_menu.h"

#ifdef __cplusplus
extern "C" {
#endif

void pico8_menu_init(void);
const dmg_emulator_desc_t *pico8_menu_get_desc(void);
dmg_action_t pico8_menu_open(const char *game_name);
bool pico8_select_rom(char *out_path, size_t max_len);
void pico8_menu_set_current_game(const char *name);
const char *pico8_menu_get_current_game(void);
void pico8_menu_set_resume_pending(bool pending);
void pico8_menu_service_resume(void);

bool pico8_menu_get_turbo(void);
void pico8_menu_set_turbo(bool turbo_on);
bool pico8_menu_get_fps(void);
void pico8_menu_set_fps(bool fps_on);

void pico8_show_error(const char *title, const char *msg);
void pico8_show_status(const char *msg, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* PICO8_MENU_H */
