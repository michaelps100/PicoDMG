/* Forced include for the MCUME reference runner: prototypes the legacy core
 * calls without declaring (implicit int would truncate 64-bit pointers). */
#ifndef A26_REF_COMPAT_H
#define A26_REF_COMPAT_H
void *emu_Malloc(int size);
int emu_LoadFile(char *name, char *buf, int size);
void emu_printf(const char *s);
void emu_SetPaletteEntry(unsigned char r, unsigned char g, unsigned char b, int i);
void emu_DrawScreenPal16(unsigned char *buf, int w, int h, int stride);
void emu_DrawVsync(void);
#endif
