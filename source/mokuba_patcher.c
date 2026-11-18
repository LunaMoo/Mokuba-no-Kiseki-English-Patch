/* SPDX-License-Identifier: MIT */
/*
 * Mokuba no Kiseki English patcher
 *
 * Usage: drag "Kidou Senshi Gundam - Mokuba no Kiseki (Japan).iso" onto the exe
 *        (or: mokuba_patcher <input.iso> [patch file])
 * Output: <input>_english.iso next to the input. The input is never modified.
 *
 * Two editions, both applied to the original Japanese ISO:
 *   mokuba_english.patch         -> <name>_english.iso         (PSP and PPSSPP)
 *   mokuba_english_ppsspp.patch  -> <name>_english_ppsspp.iso  (PPSSPP texture-pack friendly)
 * A patch file contains:
 *   - whole replacement files (translated .MES text; EBOOT.BIN / PARAM.SFO in the PPSSPP edition)
 *   - replacement entries for FPAC archives; the archive is rebuilt from the
 *     user's own ISO, so unchanged assets are never redistributed.
 *   - (PPSSPP edition) the changes to EBOOT.BIN, applied to the user's own decrypted
 *     EBOOT (dumped by PPSSPP, placed next to the ISO); no game code is shipped
 *   - (PPSSPP edition) a list of original archive entries whose GIM textures are
 *     padded to their power-of-two size here (inflate, pad, deflate - mk_deflate.h).
 * Files that still fit in their original sectors are written in place, larger
 * files are appended at the end of the image and their ISO9660 directory
 * records are updated.
 *
 * Build:  MSVC:  cl /O2 mokuba_patcher.c
 *         MinGW: gcc -O2 -o mokuba_patcher.exe mokuba_patcher.c
 *         Linux: gcc -O2 -o mokuba_patcher mokuba_patcher.c
 * No external dependencies.
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#define FSEEK _fseeki64
#define FTELL _ftelli64
#else
#define FSEEK fseeko
#define FTELL ftello
#endif

#define SECTOR 2048
#define FPAC_SECTOR 0x800
static const char PATCH_MAGIC[7] = {'M','K','P','A','T','C','H'};
static int g_patch_ver;

static int g_pause = 1;
static void finish(int code) {
    if (g_pause) { printf("\nPress Enter to close..."); fflush(stdout); getchar(); }
    exit(code);
}
static void die(const char *msg) { fprintf(stderr, "\nERROR: %s\n", msg); finish(1); }

/* ---------- crc32 ---------- */
static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------- little helpers ---------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr32be(uint8_t *p, uint32_t v) { p[3] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v >> 16); p[0] = (uint8_t)(v >> 24); }
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }

static void xread(FILE *f, int64_t off, void *buf, size_t n) {
    if (FSEEK(f, off, SEEK_SET) != 0 || fread(buf, 1, n, f) != n) die("read error (is the ISO complete?)");
}
static void xwrite(FILE *f, int64_t off, const void *buf, size_t n) {
    if (FSEEK(f, off, SEEK_SET) != 0 || fwrite(buf, 1, n, f) != n) die("write error (disk full?)");
}

/* ---------- ISO9660 lookup ---------- */
typedef struct { int64_t rec_off; uint32_t lba; uint32_t size; } IsoFile;

/* compare an ISO name (may have ";1") with a path component, case-insensitive */
static int name_eq(const uint8_t *iso, int len, const char *comp, int clen) {
    int n = len;
    for (int i = 0; i < len; i++) if (iso[i] == ';') { n = i; break; }
    if (n > 0 && iso[n - 1] == '.' ) n--; /* "NAME." for extension-less */
    if (n != clen) return 0;
    for (int i = 0; i < n; i++) if (toupper(iso[i]) != toupper((unsigned char)comp[i])) return 0;
    return 1;
}

static int iso_find(FILE *f, uint32_t dir_lba, uint32_t dir_size, const char *path, IsoFile *out) {
    const char *slash = strchr(path, '/');
    int clen = slash ? (int)(slash - path) : (int)strlen(path);
    uint8_t *dir = xmalloc(dir_size);
    xread(f, (int64_t)dir_lba * SECTOR, dir, dir_size);
    uint32_t pos = 0;
    while (pos < dir_size) {
        uint8_t len = dir[pos];
        if (len == 0) { pos = (pos / SECTOR + 1) * SECTOR; continue; }
        uint8_t nlen = dir[pos + 32];
        const uint8_t *name = dir + pos + 33;
        uint8_t flags = dir[pos + 25];
        if (!(nlen == 1 && (name[0] == 0 || name[0] == 1)) && name_eq(name, nlen, path, clen)) {
            uint32_t lba = rd32(dir + pos + 2), size = rd32(dir + pos + 10);
            int64_t rec = (int64_t)dir_lba * SECTOR + pos;
            free(dir);
            if (slash) {
                if (!(flags & 2)) return 0;
                return iso_find(f, lba, size, slash + 1, out);
            }
            out->rec_off = rec; out->lba = lba; out->size = size;
            return 1;
        }
        pos += len;
    }
    free(dir);
    return 0;
}

#include "mk_deflate.h"

/* ---------- FPAC rebuild ---------- */
typedef struct { uint32_t csize; uint16_t flag; uint16_t sector; } FpacEnt;

/* locate entry i of an FPAC archive */
static const uint8_t *fpac_entry(const uint8_t *orig, uint32_t osize, uint32_t i, uint32_t *csize, uint32_t *sector) {
    uint32_t n = 0, minsec = 0xFFFFFFFFu;
    while (8 + 8 * (n + 1) <= osize && rd32(orig + 8 + 8 * n) != 0) { uint16_t sc = rd16(orig + 14 + 8 * n); if (sc < minsec) minsec = sc; n++; }
    if (i >= n) die("patch references a missing archive entry");
    uint32_t table_end = 8 + 8 * n, base = 0; int found = 0;
    for (uint32_t p = 0; p + 2 <= osize; p += FPAC_SECTOR)
        if (p >= table_end && orig[p] == 0x1F && orig[p + 1] == 0x8B) { base = p - minsec * FPAC_SECTOR; found = 1; break; }
    if (!found) die("cannot parse FPAC archive");
    *csize = rd32(orig + 8 + 8 * i); *sector = rd16(orig + 14 + 8 * i);
    uint64_t off = (uint64_t)base + (uint64_t)*sector * FPAC_SECTOR;
    if (off + *csize > osize) die("FPAC entry out of range (wrong ISO?)");
    return orig + off;
}

static uint8_t *fpac_rebuild(const uint8_t *orig, uint32_t osize, uint32_t nrep,
                             const uint32_t *rep_idx, uint8_t *const *rep_data, const uint32_t *rep_len,
                             uint32_t *out_size) {
    if (osize < 16 || memcmp(orig, "FPAC", 4) != 0) die("archive is not FPAC (wrong ISO?)");
    uint32_t n = 0;
    while (8 + 8 * (n + 1) <= osize && rd32(orig + 8 + 8 * n) != 0) n++;
    FpacEnt *e = xmalloc(sizeof(FpacEnt) * (n ? n : 1));
    uint32_t minsec = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        e[i].csize = rd32(orig + 8 + 8 * i);
        e[i].flag = rd16(orig + 12 + 8 * i);
        e[i].sector = rd16(orig + 14 + 8 * i);
        if (e[i].sector < minsec) minsec = e[i].sector;
    }
    /* data base: first sector-aligned gzip header after the table */
    uint32_t table_end = 8 + 8 * n, base = 0;
    int found = 0;
    for (uint32_t p = 0; p + 2 <= osize; p += FPAC_SECTOR) {
        if (p >= table_end && orig[p] == 0x1F && orig[p + 1] == 0x8B) { base = p - minsec * FPAC_SECTOR; found = 1; break; }
    }
    if (!found || base < table_end + 8) die("cannot parse FPAC archive");
    /* size */
    uint64_t total = base;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = e[i].csize;
        for (uint32_t r = 0; r < nrep; r++) if (rep_idx[r] == i) len = rep_len[r];
        total += ((uint64_t)len + FPAC_SECTOR - 1) / FPAC_SECTOR * FPAC_SECTOR;
    }
    if (total > 0xFFFFFFFFu) die("archive too large");
    uint8_t *out = xmalloc((size_t)total);
    memset(out, 0, (size_t)total);
    memcpy(out, orig, base);
    uint32_t pos = base;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t ooff = (uint64_t)base + (uint64_t)e[i].sector * FPAC_SECTOR;
        const uint8_t *src = NULL;
        uint32_t len = e[i].csize;
        for (uint32_t r = 0; r < nrep; r++) if (rep_idx[r] == i) { src = rep_data[r]; len = rep_len[r]; }
        if (!src) {  /* unchanged entry: copy from the original archive */
            if (ooff + len > osize) die("FPAC entry out of range (wrong ISO?)");
            src = orig + ooff;
        }
        uint32_t sec = (pos - base) / FPAC_SECTOR;
        if (sec > 0xFFFF) die("FPAC sector overflow");
        memcpy(out + pos, src, len);
        wr32le(out + 8 + 8 * i, len);
        wr16(out + 12 + 8 * i, e[i].flag);
        wr16(out + 14 + 8 * i, (uint16_t)sec);
        pos += (len + FPAC_SECTOR - 1) / FPAC_SECTOR * FPAC_SECTOR;
    }
    for (uint32_t r = 0; r < nrep; r++) if (rep_idx[r] >= n) die("patch references a missing archive entry");
    free(e);
    *out_size = (uint32_t)total;
    return out;
}

/* ---------- patch file reading ---------- */
typedef struct { const uint8_t *p, *end; } Rd;
static uint32_t r32(Rd *r) { if (r->p + 4 > r->end) die("patch file is truncated"); uint32_t v = rd32(r->p); r->p += 4; return v; }
static uint16_t r16(Rd *r) { if (r->p + 2 > r->end) die("patch file is truncated"); uint16_t v = rd16(r->p); r->p += 2; return v; }
static uint8_t r8(Rd *r) { if (r->p + 1 > r->end) die("patch file is truncated"); return *r->p++; }
static const uint8_t *rbytes(Rd *r, uint32_t n) { if (r->p + n > r->end) die("patch file is truncated"); const uint8_t *q = r->p; r->p += n; return q; }

static FILE *open_file(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8);
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}


/* ---------- decrypted EBOOT lookup (PPSSPP edition) ----------
 * The patch only carries the changes to the game's executable. The user supplies the
 * decrypted EBOOT.BIN dumped by PPSSPP, placed next to the ISO (any file name; it is
 * recognised by size and CRC32). */
static uint8_t *g_dec = NULL; static size_t g_dec_len = 0;
static const char *g_iso_path = NULL, *g_dec_arg = NULL;
static void dir_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *s1 = strrchr(out, '/'), *s2 = strrchr(out, '\\'); char *s = s1 > s2 ? s1 : s2;
    if (s) s[1] = 0; else out[0] = 0;
}
static int try_dec(const char *p, uint32_t size, uint32_t crc) {
    FILE *f = open_file(p, "rb"); if (!f) return 0;
    FSEEK(f, 0, SEEK_END); int64_t n = FTELL(f);
    if (n != (int64_t)size) { fclose(f); return 0; }
    uint8_t *b = xmalloc(size); FSEEK(f, 0, SEEK_SET);
    size_t got = fread(b, 1, size, f); fclose(f);
    if (got != size || crc32_buf(b, size) != crc) { free(b); return 0; }
    g_dec = b; g_dec_len = size; printf("Using decrypted EBOOT: %s\n", p); return 1;
}
#ifdef _WIN32
static int find_dec(uint32_t size, uint32_t crc) {
    if (g_dec) return 1;
    if (g_dec_arg && try_dec(g_dec_arg, size, crc)) return 1;
    char dir[1024]; dir_of(g_iso_path, dir, sizeof dir);
    char pat[1100]; snprintf(pat, sizeof pat, "%s*", dir);
    wchar_t wpat[1100]; MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, 1100);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow != size) continue;
        char name[600]; WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        char full[1700]; snprintf(full, sizeof full, "%s%s", dir, name);
        if (try_dec(full, size, crc)) { ok = 1; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
#else
#include <dirent.h>
#include <sys/stat.h>
static int find_dec(uint32_t size, uint32_t crc) {
    if (g_dec) return 1;
    if (g_dec_arg && try_dec(g_dec_arg, size, crc)) return 1;
    char dir[1024]; dir_of(g_iso_path, dir, sizeof dir);
    DIR *d = opendir(dir[0] ? dir : "."); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while ((e = readdir(d))) {
        char full[1400]; snprintf(full, sizeof full, "%s%s", dir, e->d_name);
        struct stat st; if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != (off_t)size) continue;
        if (try_dec(full, size, crc)) { ok = 1; break; }
    }
    closedir(d);
    return ok;
}
#endif
static void dec_missing(void) {
    fprintf(stderr,
        "\nThis edition needs the game's decrypted EBOOT.BIN (it is not included in the patch).\n"
        "  1. In PPSSPP: Settings > Tools > Developer tools > enable\n"
        "     \"Dump decrypted EBOOT.BIN on game boot\".\n"
        "  2. Start the original Japanese game once, until the title screen.\n"
        "  3. Copy the dumped file (in PPSSPP's memstick folder, PSP/SYSTEM/DUMP,\n"
        "     e.g. NPJH50442_EBOOT.BIN) into the same folder as the ISO.\n"
        "  4. Run the patcher again.\n");
    die("decrypted EBOOT.BIN not found next to the ISO");
}
static uint8_t *apply_ops(const uint8_t *src, size_t slen, uint32_t nops, const uint8_t *p, const uint8_t *end, uint32_t out_size) {
    uint8_t *o = xmalloc(out_size); size_t pos = 0;
    for (uint32_t i = 0; i < nops; i++) {
        if (p >= end) die("bad EBOOT patch");
        uint8_t t = *p++;
        if (t == 0) {
            uint32_t so = rd32(p), l = rd32(p + 4); p += 8;
            if ((uint64_t)so + l > slen || pos + l > out_size) die("bad EBOOT patch");
            memcpy(o + pos, src + so, l); pos += l;
        } else if (t == 1) {
            uint32_t l = rd32(p); p += 4;
            if (p + l > end || pos + l > out_size) die("bad EBOOT patch");
            memcpy(o + pos, p, l); p += l; pos += l;
        } else if (t == 2) {
            uint32_t l = rd32(p); p += 4;
            if (pos + l > out_size) die("bad EBOOT patch");
            memset(o + pos, 0, l); pos += l;
        } else die("bad EBOOT patch");
    }
    if (pos != out_size) die("bad EBOOT patch");
    return o;
}

static uint8_t *load_whole(const char *path, size_t *n) {
    FILE *f = open_file(path, "rb");
    if (!f) return NULL;
    FSEEK(f, 0, SEEK_END); int64_t s = FTELL(f); FSEEK(f, 0, SEEK_SET);
    uint8_t *b = xmalloc((size_t)s);
    if (fread(b, 1, (size_t)s, f) != (size_t)s) die("cannot read patch file");
    fclose(f); *n = (size_t)s; return b;
}

static int real_main(int argc, char **argv) {
    crc_init();
    printf("Mobile Suit Gundam: Path of the Trojan Horse - English patcher\n");
    printf("================================================================\n\n");
    if (argc < 2) {
        printf("Drag and drop the Japanese ISO onto this program.\n");
        printf("(or run: %s <game.iso> [patch file] [decrypted EBOOT])\n", argv[0]);
        printf("Optional texture patch: drag the ISO onto apply_texture_patch.bat\n");
        finish(1);
    }
    const char *in_path = argv[1];
    g_iso_path = in_path;
    if (argc >= 4) g_dec_arg = argv[3];

    /* locate patch file: arg 2, else next to the exe */
    char patch_path[1024];
    if (argc >= 3) snprintf(patch_path, sizeof patch_path, "%s", argv[2]);
    else {
        snprintf(patch_path, sizeof patch_path, "%s", argv[0]);
        char *s1 = strrchr(patch_path, '/'), *s2 = strrchr(patch_path, '\\');
        char *s = s1 > s2 ? s1 : s2;
        if (s) s[1] = 0; else patch_path[0] = 0;
        strncat(patch_path, "mokuba_english.patch", sizeof patch_path - strlen(patch_path) - 1);
    }
    size_t plen;
    uint8_t *patch = load_whole(patch_path, &plen);
    if (!patch) { fprintf(stderr, "Cannot open patch file: %s\n", patch_path); die("patch file missing"); }
    Rd rd = { patch, patch + plen };
    if (plen < 12 || memcmp(patch, PATCH_MAGIC, 7) != 0 || (patch[7] != '1' && patch[7] != '2')) die("not a valid patch file");
    g_patch_ver = patch[7] - '0';
    rd.p += 8;
    uint32_t count = r32(&rd);

    /* output path */
    char out_path[1100];
    snprintf(out_path, sizeof out_path, "%s", in_path);
    char *dot = strrchr(out_path, '.');
    char *sep = strrchr(out_path, '\\'); char *sep2 = strrchr(out_path, '/'); if (sep2 > sep) sep = sep2;
    if (dot && (!sep || dot > sep)) *dot = 0;
    {   /* suffix: "_english" for the default patch, else "_<patch name without mokuba_>" */
        char suffix[256] = "_english";
        if (argc >= 3) {
            const char *b = patch_path, *q;
            for (q = patch_path; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
            if (strncmp(b, "mokuba_", 7) == 0) b += 7;
            snprintf(suffix, sizeof suffix, "_%s", b);
            char *e = strrchr(suffix, '.'); if (e) *e = 0;
            if (strcmp(suffix, "_english") != 0 && strcmp(suffix, "_") == 0) strcpy(suffix, "_patched");
        }
        strncat(out_path, suffix, sizeof out_path - strlen(out_path) - 1);
        strncat(out_path, ".iso", sizeof out_path - strlen(out_path) - 1);
    }

    FILE *in = open_file(in_path, "rb");
    if (!in) die("cannot open the input ISO");
    FSEEK(in, 0, SEEK_END); int64_t isz = FTELL(in);
    if (isz < 17 * SECTOR) die("input is too small to be an ISO");

    uint8_t pvd[SECTOR];
    xread(in, 16 * SECTOR, pvd, SECTOR);
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) die("input is not an ISO9660 image (CSO/CHD must be converted to ISO first)");
    uint32_t root_lba = rd32(pvd + 156 + 2), root_size = rd32(pvd + 156 + 10);

    /* pass 1: verify all original files before writing anything */
    printf("Checking input ISO...\n");
    {
        Rd v = rd;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t type = r8(&v); uint16_t pl = r16(&v);
            char path[512]; if (pl >= sizeof path) die("bad path in patch");
            memcpy(path, rbytes(&v, pl), pl); path[pl] = 0;
            uint32_t osize = r32(&v), ocrc = r32(&v);
            IsoFile fi;
            if (!iso_find(in, root_lba, root_size, path, &fi)) { fprintf(stderr, "missing: %s\n", path); die("this is not the expected ISO"); }
            uint8_t *buf = xmalloc(fi.size);
            xread(in, (int64_t)fi.lba * SECTOR, buf, fi.size);
            if (fi.size != osize || crc32_buf(buf, fi.size) != ocrc) {
                fprintf(stderr, "mismatch: %s\n", path);
                die("ISO does not match. Use a clean dump of\n       Kidou Senshi Gundam - Mokuba no Kiseki (Japan) [NPJH-50442].");
            }
            free(buf);
            if (type == 0) { uint32_t n = r32(&v); rbytes(&v, n); }
            else if (type == 3) {
                uint32_t ds = r32(&v), dc = r32(&v); r32(&v); r32(&v); r32(&v); uint32_t bl = r32(&v); rbytes(&v, bl);
                if (!find_dec(ds, dc)) dec_missing();
            }
            else if (type == 1 || type == 2) {
                uint32_t k = r32(&v); for (uint32_t j = 0; j < k; j++) { r32(&v); uint32_t n = r32(&v); rbytes(&v, n); }
                if (type == 2) {
                    if (g_patch_ver < 2) die("this patch file is outdated, get the current one");
                    uint32_t m = r32(&v); rbytes(&v, 4 * m);
                }
            }
            else die("unknown patch record");
        }
    }

    /* copy ISO */
    printf("Copying ISO to %s ...\n", out_path);
    FILE *out = open_file(out_path, "w+b");
    if (!out) die("cannot create the output ISO");
    {
        const size_t CH = 4 << 20; uint8_t *b = xmalloc(CH);
        FSEEK(in, 0, SEEK_SET);
        int64_t done = 0; size_t n;
        while ((n = fread(b, 1, CH, in)) > 0) {
            if (fwrite(b, 1, n, out) != n) die("write error (disk full?)");
            done += n;
            printf("\r  %3d%%", (int)(done * 100 / isz)); fflush(stdout);
        }
        printf("\n");
        free(b);
    }
    int64_t end_lba = (isz + SECTOR - 1) / SECTOR;

    /* pass 2: apply */
    for (uint32_t i = 0; i < count; i++) {
        uint8_t type = r8(&rd); uint16_t pl = r16(&rd);
        char path[512]; memcpy(path, rbytes(&rd, pl), pl); path[pl] = 0;
        r32(&rd); r32(&rd);
        IsoFile fi; iso_find(out, root_lba, root_size, path, &fi);
        uint8_t *data; uint32_t dlen;
        if (type == 0) {
            dlen = r32(&rd); data = xmalloc(dlen); memcpy(data, rbytes(&rd, dlen), dlen);
        } else if (type == 3) {
            r32(&rd); r32(&rd);
            uint32_t osz = r32(&rd), ocrc = r32(&rd), nops = r32(&rd), bl = r32(&rd);
            const uint8_t *b = rbytes(&rd, bl);
            data = apply_ops(g_dec, g_dec_len, nops, b, b + bl, osz); dlen = osz;
            if (crc32_buf(data, dlen) != ocrc) die("patched EBOOT does not verify");
        } else {
            uint32_t k = r32(&rd);
            uint32_t *idx = xmalloc(4 * (k ? k : 1)), *len = xmalloc(4 * (k ? k : 1));
            uint8_t **pd = xmalloc(sizeof(uint8_t *) * (k ? k : 1));
            for (uint32_t j = 0; j < k; j++) { idx[j] = r32(&rd); len[j] = r32(&rd); pd[j] = (uint8_t *)rbytes(&rd, len[j]); }
            uint8_t *orig = xmalloc(fi.size);
            xread(in, (int64_t)fi.lba * SECTOR, orig, fi.size);
            uint32_t kk = k;
            if (type == 2) {   /* also re-lay out the GIM textures of these original entries */
                uint32_t m = r32(&rd);
                idx = realloc(idx, 4 * (k + m + 1)); len = realloc(len, 4 * (k + m + 1));
                pd = realloc(pd, sizeof(uint8_t *) * (k + m + 1));
                if (!idx || !len || !pd) die("out of memory");
                for (uint32_t j = 0; j < m; j++) {
                    uint32_t ei = r32(&rd);
                    uint32_t csz, sec; const uint8_t *g = fpac_entry(orig, fi.size, ei, &csz, &sec);
                    uint8_t *raw; size_t rl = gz_unpack(g, csz, &raw);
                    size_t pl = 0; uint8_t *pad = entry_fix(raw, rl, &pl);
                    if (pad) {
                        uint8_t *z; size_t zl = gz_pack(g, csz, pad, pl, &z);
                        idx[kk] = ei; len[kk] = (uint32_t)zl; pd[kk] = z; kk++;
                        free(pad);
                    }
                    free(raw);
                    if ((j & 15) == 15 || j + 1 == m) { printf("\r  %-60s %u/%u", path, j + 1, m); fflush(stdout); }
                }
                if (m) printf("\r%*s\r", 79, "");
            }
            data = fpac_rebuild(orig, fi.size, kk, idx, pd, len, &dlen);
            for (uint32_t j = k; j < kk; j++) free(pd[j]);
            free(orig); free(idx); free(len); free(pd);
        }
        uint32_t old_secs = (fi.size + SECTOR - 1) / SECTOR, new_secs = (dlen + SECTOR - 1) / SECTOR;
        uint32_t lba = fi.lba;
        if (new_secs > old_secs) { lba = (uint32_t)end_lba; end_lba += new_secs; }
        uint32_t span = (new_secs > old_secs ? new_secs : old_secs) * SECTOR;
        uint8_t *blk = xmalloc(span); memset(blk, 0, span); memcpy(blk, data, dlen);
        xwrite(out, (int64_t)lba * SECTOR, blk, new_secs > old_secs ? new_secs * SECTOR : span);
        free(blk); free(data);
        uint8_t rec[18]; xread(out, fi.rec_off, rec, 18);
        wr32le(rec + 2, lba); wr32be(rec + 6, lba); wr32le(rec + 10, dlen); wr32be(rec + 14, dlen);
        xwrite(out, fi.rec_off, rec, 18);
        printf("  %-60s %s\n", path, lba == fi.lba ? "in place" : "relocated");
    }
    /* pad image and update volume size */
    {
        int64_t cur; FSEEK(out, 0, SEEK_END); cur = FTELL(out);
        int64_t want = end_lba * SECTOR;
        if (cur < want) { uint8_t z = 0; xwrite(out, want - 1, &z, 1); }
        uint8_t v[8]; wr32le(v, (uint32_t)end_lba); wr32be(v + 4, (uint32_t)end_lba);
        xwrite(out, 16 * SECTOR + 80, v, 8);
    }
    fclose(out); fclose(in); free(patch);
    printf("\nDone! Created:\n  %s\n", out_path);
    finish(0);
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv) {
    char **argv = xmalloc(sizeof(char *) * (argc + 1));
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = xmalloc(n); WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    argv[argc] = NULL;
    SetConsoleOutputCP(CP_UTF8);
    return real_main(argc, argv);
}
#else
int main(int argc, char **argv) {
    g_pause = 0;   /* command-line platforms: never wait for Enter */
    return real_main(argc, argv);
}
#endif
