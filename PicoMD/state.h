#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int Emulator_SaveState(const char* path);
int Emulator_LoadState(const char* path);

#ifdef __cplusplus
}
#endif

