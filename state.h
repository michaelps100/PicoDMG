#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Emulator_LoadState() result when a read failed after the core state had been
 * overwritten: the game is no longer in a consistent state and must be reset. */
#define SNES_STATE_LOAD_PARTIAL (-2)

/* Both return 0 on success. */
int Emulator_SaveState(const char* path);
int Emulator_LoadState(const char* path);

#ifdef __cplusplus
}
#endif
