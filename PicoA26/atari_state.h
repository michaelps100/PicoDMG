#ifndef ATARI_STATE_H
#define ATARI_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int atari_save_state(const char *filepath);
int atari_load_state(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif /* ATARI_STATE_H */
