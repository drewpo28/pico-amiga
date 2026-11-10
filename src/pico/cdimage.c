// CD images: .iso and .cue (cdimage.h).
//
// A sheet is read a line at a time: FILE "name" BINARY opens the next file,
// TRACK nn MODE1/2048 | MODE1/2352 | MODE2/2336 | MODE2/2352 | AUDIO a track in
// it, INDEX 01 mm:ss:ff where the track starts in the file, INDEX 00 where its
// pregap does (in the file), PREGAP mm:ss:ff a pregap that is not in the file.
// The disc's layout follows: a file starts where the one before ended, a PREGAP
// pushes everything after it, and a track runs to the next one's pregap (or its
// file's end).
#include <string.h>
#include "cdimage.h"

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}
static bool word(const char **pp, const char *end, const char *w) {
    const char *p = *pp;
    for (; *w; w++, p++) {
        if (p >= end) return false;
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != *w) return false;
    }
    if (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/') return false;
    *pp = skip_ws(p, end);
    return true;
}
static bool number(const char **pp, const char *end, uint32_t *v) {
    const char *p = *pp;
    if (p >= end || *p < '0' || *p > '9') return false;
    uint32_t n = 0;
    while (p < end && *p >= '0' && *p <= '9') n = n * 10 + (uint32_t)(*p++ - '0');
    *v = n;
    *pp = p;
    return true;
}
static bool msf(const char **pp, const char *end, uint32_t *frames) {
    uint32_t m, s, f;
    const char *p = *pp;
    if (!number(&p, end, &m) || p >= end || *p++ != ':' || !number(&p, end, &s) || p >= end || *p++ != ':' ||
        !number(&p, end, &f)) return false;
    *frames = (m * 60 + s) * 75 + f;
    *pp = p;
    return true;
}

bool cd_parse_cue(const char *text, uint32_t len, cd_toc_t *toc, cd_file_fn file, void *ctx) {
    memset(toc, 0, sizeof *toc);
    const char *p = text, *const end = text + len;
    int files = 0;
    uint32_t fsize = 0;                 // the current file's size, bytes
    uint32_t base = 0;                  // where the current file's sector 0 is on the disc
    uint32_t disc = 0;                  // the disc so far: where the next file starts
    uint32_t gap[CD_MAX_TRACKS];        // each track's pregap start on the disc
    uint32_t fend[CD_MAX_TRACKS];       // its file's end on the disc (set with the file's last track)
    int cur = -1;                       // the track being described
    uint32_t pre = 0;                   // its PREGAP (not in the file)
    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n') eol++;
        const char *q = skip_ws(p, eol);
        if (word(&q, eol, "FILE")) {
            char name[256];
            unsigned n = 0;
            if (q < eol && *q == '"') {
                for (q++; q < eol && *q != '"' && n + 1 < sizeof name; q++) name[n++] = *q;
                if (q < eol) q++;
            } else {
                for (; q < eol && *q != ' ' && *q != '\t' && *q != '\r' && n + 1 < sizeof name; q++) name[n++] = *q;
            }
            name[n] = 0;
            q = skip_ws(q, eol);
            if (!word(&q, eol, "BINARY")) return false;             // WAVE, MP3, MOTOROLA: not read
            if (cur >= 0 && fsize) disc = base + fsize / toc->t[cur].secsize;
            if (cur >= 0) fend[cur] = disc;
            base = disc;
            fsize = file(ctx, files, name);
            if (!fsize) return false;
            files++;
        } else if (word(&q, eol, "TRACK")) {
            uint32_t no;
            if (!files || !number(&q, eol, &no) || toc->tracks >= CD_MAX_TRACKS) return false;
            q = skip_ws(q, eol);
            cd_track_t *t = &toc->t[toc->tracks];
            t->file = (uint8_t)(files - 1);
            if (word(&q, eol, "AUDIO")) { t->mode = 0; t->secsize = 2352; }
            else if (word(&q, eol, "MODE1")) { t->mode = 1; }
            else if (word(&q, eol, "MODE2")) { t->mode = 2; }
            else return false;
            if (t->mode) {
                uint32_t ss;
                if (q >= eol || *q++ != '/' || !number(&q, eol, &ss) || (ss != 2048 && ss != 2336 && ss != 2352)) return false;
                t->secsize = (uint16_t)ss;
            }
            cur = toc->tracks++;
            gap[cur] = 0xFFFFFFFFu;
            fend[cur] = 0;
            pre = 0;
        } else if (word(&q, eol, "PREGAP")) {
            uint32_t f;
            if (cur < 0 || !msf(&q, eol, &f)) return false;
            pre = f;
            base += f;                                               // not in the file: the rest moves
        } else if (word(&q, eol, "INDEX")) {
            uint32_t no, f;
            if (cur < 0 || !number(&q, eol, &no)) return false;
            q = skip_ws(q, eol);
            if (!msf(&q, eol, &f)) return false;
            cd_track_t *t = &toc->t[cur];
            if (no == 0) gap[cur] = base + f;
            else if (no == 1) {
                t->lsn = base + f;
                t->foff = f * t->secsize;
                if (gap[cur] == 0xFFFFFFFFu) gap[cur] = t->lsn - pre;
            }
        }
        p = eol < end ? eol + 1 : end;
    }
    if (!toc->tracks || cur < 0) return false;
    disc = base + fsize / toc->t[cur].secsize;
    fend[cur] = disc;
    toc->leadout = disc;
    for (int i = 0; i < toc->tracks; i++) {
        cd_track_t *t = &toc->t[i];
        // To the next track's pregap, but not past this one's file.
        uint32_t stop = i + 1 < toc->tracks ? gap[i + 1] : disc;
        if (fend[i] && stop > fend[i]) stop = fend[i];
        if (stop <= t->lsn) return false;
        t->sectors = stop - t->lsn;
    }
    return true;
}

bool cd_from_iso(uint32_t size, cd_toc_t *toc) {
    memset(toc, 0, sizeof *toc);
    if (size < 2048 * 16) return false;
    toc->tracks = 1;
    toc->t[0].mode = 1;
    toc->t[0].secsize = 2048;
    toc->t[0].sectors = size / 2048;
    toc->leadout = toc->t[0].sectors;
    return true;
}

bool cd_locate(const cd_toc_t *toc, uint32_t lsn, int *file, uint32_t *off, uint32_t *secsize, int *mode, uint32_t *run) {
    for (int i = 0; i < toc->tracks; i++) {
        const cd_track_t *t = &toc->t[i];
        if (lsn < t->lsn || lsn >= t->lsn + t->sectors) continue;
        if (!t->mode) return false;
        *file = t->file;
        *off = t->foff + (lsn - t->lsn) * t->secsize;
        *secsize = t->secsize;
        *mode = t->mode;
        *run = t->lsn + t->sectors - lsn;
        return true;
    }
    return false;
}

void cd_unpack_sector(const uint8_t *src, uint32_t secsize, uint8_t *dst) {
    if (secsize == 2048) { memcpy(dst, src, 2048); memset(dst + 2048, 0, 2336 - 2048); }
    else memcpy(dst, src + (secsize == 2352 ? 16 : 0), 2336);
}

int cd_read_sector(const cd_toc_t *toc, uint32_t lsn, uint8_t *dst, cd_read_fn rd, void *ctx) {
    int file, mode;
    uint32_t off, ss, run;
    if (!cd_locate(toc, lsn, &file, &off, &ss, &mode, &run)) return -1;
    if (ss == 2048) {
        if (!rd(ctx, file, off, dst, 2048)) return -1;
        memset(dst + 2048, 0, 2336 - 2048);
    } else if (!rd(ctx, file, off + (ss == 2352 ? 16 : 0), dst, 2336)) {
        return -1;
    }
    return mode;
}
