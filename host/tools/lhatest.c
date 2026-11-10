// lha.c on the PC: list an archive's members and unpack them into a folder
// (host/tests/smoke.sh checks the decoder against the archives it has).
//   lhatest archive.lha [outdir]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "lha.h"

static uint32_t rd(void *ctx, uint32_t off, void *dst, uint32_t len) {
    FILE *f = (FILE *)ctx;
    if (fseek(f, (long)off, SEEK_SET)) return 0;
    return (uint32_t)fread(dst, 1, len, f);
}
static int wr(void *ctx, const uint8_t *src, uint32_t len) {
    return ctx && fwrite(src, 1, len, (FILE *)ctx) != len;
}
static void mkdirs(char *p) {
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) { *s = 0; mkdir(p, 0755); *s = '/'; }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "lhatest archive.lha [outdir]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    void *work = malloc(lha_work_size());
    lha_entry_t e;
    uint32_t off = 0;
    int bad = 0, n = 0, r;
    while ((r = lha_header(rd, f, off, &e)) == LHA_OK) {
        FILE *o = NULL;
        if (argc > 2 && !e.dir) {
            char path[600];
            snprintf(path, sizeof path, "%s/%s", argv[2], e.path);
            mkdirs(path);
            o = fopen(path, "wb");
        }
        r = lha_unpack(rd, f, &e, wr, o, work);
        if (o) fclose(o);
        printf("%s %8u %8u %02X %s%s%s%s\n", e.method, e.size, e.packed, e.attr, e.path, e.dir ? "/" : "",
               e.comment[0] ? "  ; " : "", e.comment);
        if (r != LHA_OK) { printf("  ** %s\n", lha_error(r)); bad++; }
        n++;
        off = e.next_off;
    }
    if (r != LHA_END) { printf("** header at %u: %s\n", off, lha_error(r)); bad++; }
    printf("%d members, %d bad\n", n, bad);
    free(work);
    fclose(f);
    return bad != 0;
}
