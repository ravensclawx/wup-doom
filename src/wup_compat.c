/* What Chocolate Doom 3.1's music player and MIDI reader (i_oplmusic.c, midifile.c) call that doomgeneric's older
   Chocolate base does not have. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "i_system.h"

FILE *M_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int M_remove(const char *path) { return remove(path); }
void *I_Realloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) I_Error("I_Realloc: %u bytes", (unsigned)n);
    return q;
}
uint32_t SDL_SwapBE32(uint32_t v) { return __builtin_bswap32(v); }   /* (the pad is little-endian) */
uint16_t SDL_SwapBE16(uint16_t v) { return __builtin_bswap16(v); }
