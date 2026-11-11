#include "atari_state.h"
#include "a26_core.h"
#include "ff.h"
#include <stdio.h>

// Shared scratch storage: never put a snapshot or FIL on the 4 KB core0 stack.
static uint8_t s_state_buffer[A26_STATE_MAX_SIZE];
static FIL s_state_file;
int atari_save_state(const char *path) {
    if(!path) return -1;
    size_t n=a26_core_save_state(s_state_buffer,sizeof(s_state_buffer));
    if(!n) return -2;
    if(f_open(&s_state_file,path,FA_WRITE|FA_CREATE_ALWAYS)!=FR_OK) return -1;
    UINT written=0;
    FRESULT result=f_write(&s_state_file,s_state_buffer,(UINT)n,&written);
    FRESULT closed=f_close(&s_state_file);
    return result==FR_OK && closed==FR_OK && written==n ? 0 : -2;
}
int atari_load_state(const char *path) {
    if(!path) return -1;
    if(f_open(&s_state_file,path,FA_READ)!=FR_OK) return -1;
    FSIZE_t n=f_size(&s_state_file);
    if(n>sizeof(s_state_buffer) || n<28) { f_close(&s_state_file); return -3; }
    UINT read=0;
    FRESULT result=f_read(&s_state_file,s_state_buffer,(UINT)n,&read);
    f_close(&s_state_file);
    if(result!=FR_OK || read!=n) return -2;
    if(!a26_core_load_state(s_state_buffer,read)) {
        printf("[A26] State rejeitado: versao, ROM ou dados invalidos\n");
        return -3;
    }
    return 0;
}
