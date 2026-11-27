/* The WAD, read with open / lseek / read (doomgeneric's w_file_stdc.c goes through stdio, whose small buffer split
   every lump into many little reads of the flash): each lump is one read, which the filesystem takes from flash in
   large pieces. */
#include <fcntl.h>
#include <unistd.h>
#include "w_file.h"
#include "z_zone.h"

typedef struct { wad_file_t wad; int fd; } wup_wad_file_t;
extern wad_file_class_t stdc_wad_file;

static wad_file_t *Open(char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    wup_wad_file_t *w = Z_Malloc(sizeof *w, PU_STATIC, 0);
    w->wad.file_class = &stdc_wad_file; w->wad.mapped = NULL;
    w->wad.length = (unsigned)lseek(fd, 0, SEEK_END);
    w->fd = fd;
    return &w->wad;
}
static void Close(wad_file_t *wad) { close(((wup_wad_file_t *)wad)->fd); Z_Free(wad); }
static size_t Read(wad_file_t *wad, unsigned int offset, void *buf, size_t len) {
    int fd = ((wup_wad_file_t *)wad)->fd;
    if (lseek(fd, offset, SEEK_SET) < 0) return 0;
    size_t got = 0;
    while (got < len) { ssize_t k = read(fd, (char *)buf + got, len - got); if (k <= 0) break; got += (size_t)k; }
    return got;
}
wad_file_class_t stdc_wad_file = { Open, Close, Read };
