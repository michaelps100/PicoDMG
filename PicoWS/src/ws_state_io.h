#ifndef WS_STATE_IO_H
#define WS_STATE_IO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Save emulator state to file (4 save slots).
 * @param path  Full FatFS path, e.g. "0:/saves/ws/game_1.ss"
 * @return true on success
 */
bool ws_state_save(const char *path);

/**
 * Restore emulator state from file.
 * @param path  Full FatFS path
 * @return true on success
 */
bool ws_state_load(const char *path);

/**
 * Save battery SRAM (ws_staticRam, 64KB).
 * @param path  Full FatFS path, e.g. "0:/saves/ws/game.sav"
 * @return true on success
 */
bool ws_sram_save(const char *path);

/**
 * Load battery SRAM.
 * @param path  Full FatFS path
 * @return true on success
 */
bool ws_sram_load(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* WS_STATE_IO_H */

