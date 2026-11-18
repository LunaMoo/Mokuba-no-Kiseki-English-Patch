/* SPDX-License-Identifier: MIT */
/*
 * Small self-contained inflate / deflate (fixed Huffman + LZ77) and GIM layout
 * for the Mokuba no Kiseki patcher. Public-domain style, no dependencies.
 * Included by mokuba_patcher.c (expects die(), xmalloc(), crc32_buf(), rd16/rd32/wr32le).
 */

/* ===================== inflate (puff-style) ===================== */
typedef struct {
    const uint8_t *in; size_t inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outcap;
} Inf;

static int inf_bits(Inf *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) die("corrupt deflate stream (truncated)");
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need; s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}
static void inf_put(Inf *s, uint8_t c) {
    if (s->outlen == s->outcap) {
        size_t nc = s->outcap ? s->outcap * 2 : 1 << 16;
        uint8_t *n = realloc(s->out, nc); if (!n) die("out of memory");
        s->out = n; s->outcap = nc;
    }
    s->out[s->outlen++] = c;
}
typedef struct { short count[16]; short symbol[320]; } Huff;
static int inf_decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    die("corrupt deflate stream (bad code)"); return 0;
}
static void inf_construct(Huff *h, const short *length, int n) {
    short offs[16];
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
}
static const short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short DEXT[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static void inf_codes(Inf *s, const Huff *lc, const Huff *dc) {
    for (;;) {
        int sym = inf_decode(s, lc);
        if (sym < 256) inf_put(s, (uint8_t)sym);
        else if (sym == 256) return;
        else {
            sym -= 257; if (sym >= 29) die("corrupt deflate stream (length)");
            int len = LBASE[sym] + inf_bits(s, LEXT[sym]);
            int ds = inf_decode(s, dc); if (ds >= 30) die("corrupt deflate stream (distance)");
            size_t dist = (size_t)DBASE[ds] + inf_bits(s, DEXT[ds]);
            if (dist > s->outlen) die("corrupt deflate stream (too far)");
            while (len--) inf_put(s, s->out[s->outlen - dist]);
        }
    }
}
static size_t inflate_raw(const uint8_t *in, size_t inlen, uint8_t **out) {
    Inf s = { in, inlen, 0, 0, 0, NULL, 0, 0 };
    static Huff flc, fdc; static int fixed_ready = 0;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.inpos + 4 > s.inlen) die("corrupt deflate stream");
            unsigned len = s.in[s.inpos] | (s.in[s.inpos + 1] << 8);
            s.inpos += 4;
            if (s.inpos + len > s.inlen) die("corrupt deflate stream");
            while (len--) inf_put(&s, s.in[s.inpos++]);
        } else if (type == 1) {
            if (!fixed_ready) {
                short l[288]; int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                inf_construct(&flc, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                inf_construct(&fdc, l, 30);
                fixed_ready = 1;
            }
            inf_codes(&s, &flc, &fdc);
        } else if (type == 2) {
            static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            short lengths[320]; Huff lc, dc;
            int nlen = inf_bits(&s, 5) + 257, ndist = inf_bits(&s, 5) + 1, ncode = inf_bits(&s, 4) + 4;
            int i;
            for (i = 0; i < ncode; i++) lengths[order[i]] = (short)inf_bits(&s, 3);
            for (; i < 19; i++) lengths[order[i]] = 0;
            inf_construct(&lc, lengths, 19);
            for (i = 0; i < nlen + ndist;) {
                int sym = inf_decode(&s, &lc), len = 0, rep;
                if (sym < 16) { lengths[i++] = (short)sym; continue; }
                if (sym == 16) { if (!i) die("corrupt deflate stream"); len = lengths[i - 1]; rep = 3 + inf_bits(&s, 2); }
                else if (sym == 17) rep = 3 + inf_bits(&s, 3);
                else rep = 11 + inf_bits(&s, 7);
                if (i + rep > nlen + ndist) die("corrupt deflate stream");
                while (rep--) lengths[i++] = (short)len;
            }
            inf_construct(&lc, lengths, nlen);
            inf_construct(&dc, lengths + nlen, ndist);
            inf_codes(&s, &lc, &dc);
        } else die("corrupt deflate stream (block type)");
    } while (!last);
    *out = s.out;
    return s.outlen;
}

/* ===================== deflate: LZ77 + fixed Huffman ===================== */
typedef struct { uint8_t *p; size_t n, cap; uint32_t bb; int bc; } Bw;
static void bw_byte(Bw *w, uint8_t b) {
    if (w->n == w->cap) { size_t nc = w->cap ? w->cap * 2 : 1 << 16; uint8_t *q = realloc(w->p, nc); if (!q) die("out of memory"); w->p = q; w->cap = nc; }
    w->p[w->n++] = b;
}
static void bw_bits(Bw *w, uint32_t v, int n) {   /* LSB first */
    w->bb |= v << w->bc; w->bc += n;
    while (w->bc >= 8) { bw_byte(w, (uint8_t)w->bb); w->bb >>= 8; w->bc -= 8; }
}
static void bw_huff(Bw *w, uint32_t code, int n) { /* Huffman codes go MSB first */
    uint32_t r = 0; for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i);
    bw_bits(w, r, n);
}
static void fx_lit(Bw *w, int c) {
    if (c < 144) bw_huff(w, 0x30 + c, 8);
    else if (c < 256) bw_huff(w, 0x190 + c - 144, 9);
    else if (c < 280) bw_huff(w, c - 256, 7);
    else bw_huff(w, 0xC0 + c - 280, 8);
}
static void fx_match(Bw *w, int len, int dist) {
    int i = 28; while (LBASE[i] > len) i--;
    fx_lit(w, 257 + i); if (LEXT[i]) bw_bits(w, (uint32_t)(len - LBASE[i]), LEXT[i]);
    int d = 29; while (DBASE[d] > dist) d--;
    bw_huff(w, (uint32_t)d, 5); if (DEXT[d]) bw_bits(w, (uint32_t)(dist - DBASE[d]), DEXT[d]);
}
#define HBITS 15
#define WSIZE 32768
static size_t deflate_fixed(const uint8_t *in, size_t n, uint8_t **out) {
    Bw w = { NULL, 0, 0, 0, 0 };
    int32_t *head = xmalloc(sizeof(int32_t) << HBITS), *prev = xmalloc(sizeof(int32_t) * WSIZE);
    for (int i = 0; i < (1 << HBITS); i++) head[i] = -1;
    bw_bits(&w, 1, 1); bw_bits(&w, 1, 2);    /* final block, fixed Huffman */
    size_t i = 0;
#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & ((1u << HBITS) - 1))
    while (i < n) {
        int best = 0, bdist = 0;
        if (i + 3 <= n) {
            uint32_t h = HASH(in + i);
            int32_t c = head[h]; int chain = 48;
            size_t maxl = n - i < 258 ? n - i : 258;
            while (c >= 0 && i - (size_t)c <= WSIZE - 1 && chain--) {
                const uint8_t *a = in + c, *b = in + i;
                if (a[best] == b[best]) {
                    size_t l = 0; while (l < maxl && a[l] == b[l]) l++;
                    if ((int)l > best) { best = (int)l; bdist = (int)(i - (size_t)c); if (l == maxl) break; }
                }
                int32_t pc = prev[c & (WSIZE - 1)];
                if (pc >= c) break;
                c = pc;
            }
        }
        size_t adv = best >= 3 ? (size_t)best : 1;
        if (best >= 3) fx_match(&w, best, bdist); else fx_lit(&w, in[i]);
        for (size_t k = 0; k < adv; k++, i++) {
            if (i + 3 <= n) { uint32_t h = HASH(in + i); prev[i & (WSIZE - 1)] = head[h]; head[h] = (int32_t)i; }
        }
    }
    fx_lit(&w, 256);
    if (w.bc) bw_bits(&w, 0, 8 - w.bc);
    free(head); free(prev);
    *out = w.p;
    return w.n;
}

/* ===================== gzip member helpers ===================== */
static size_t gz_header_len(const uint8_t *g, size_t n) {
    if (n < 18 || g[0] != 0x1F || g[1] != 0x8B || g[2] != 8) die("FPAC entry is not gzip");
    uint8_t fl = g[3]; size_t p = 10;
    if (fl & 4) { p += 2 + (size_t)(g[p] | (g[p + 1] << 8)); }
    if (fl & 8) { while (p < n && g[p]) p++; p++; }
    if (fl & 16) { while (p < n && g[p]) p++; p++; }
    if (fl & 2) p += 2;
    if (p >= n) die("bad gzip header");
    return p;
}
static size_t gz_unpack(const uint8_t *g, size_t n, uint8_t **out) {
    size_t h = gz_header_len(g, n);
    size_t len = inflate_raw(g + h, n - h - 8, out);
    if (crc32_buf(*out, len) != rd32(g + n - 8) || (uint32_t)len != rd32(g + n - 4)) die("gzip CRC mismatch");
    return len;
}
/* new member: same header as 'like' (keeps the stored file name), new data */
static size_t gz_pack(const uint8_t *like, size_t likelen, const uint8_t *data, size_t n, uint8_t **out) {
    size_t h = gz_header_len(like, likelen);
    uint8_t *def; size_t dl = deflate_fixed(data, n, &def);
    uint8_t *o = xmalloc(h + dl + 8);
    memcpy(o, like, h); o[3] &= (uint8_t)~2;   /* drop header CRC flag if any */
    memcpy(o + h, def, dl);
    wr32le(o + h + dl, crc32_buf(data, n)); wr32le(o + h + dl + 4, (uint32_t)n);
    free(def);
    *out = o;
    return h + dl + 8;
}

/* ===================== GIM layout (see tools/padgim.py fix_gim) =====================
 * PPSSPP edition: every GIM image is stored with a power-of-two row stride
 * (pitch_align = max(pow2(w) * bpp / 8, 16) bytes) and a power-of-two row count
 * (height_align = max(pow2(h), old height_align)), zero padded, swizzled again if it was.
 * The patched EBOOT binds and copies textures with exactly that layout, so the area the
 * GE (and PPSSPP's texture hash) reads holds one copy of the image and nothing else. */
static uint32_t p2u(uint32_t x) { uint32_t v = 1; while (v < x) v <<= 1; return v; }
static uint32_t alignu(uint32_t x, uint32_t a) { return (x + a - 1) / a * a; }

typedef struct { size_t off; uint16_t id; uint32_t size, next, doff; size_t par[8]; int np; } GimBlk;
static int gim_walk(const uint8_t *g, size_t o, size_t lim, size_t *par, int np, GimBlk *out, int *nout) {
    while (o + 16 <= lim) {
        uint16_t id = rd16(g + o); uint32_t size = rd32(g + o + 4), nx = rd32(g + o + 8), doff = rd32(g + o + 12);
        if (size == 0 || o + size > lim) break;
        if (*nout >= 64) return 0;
        GimBlk *b = &out[(*nout)++]; b->off = o; b->id = id; b->size = size; b->next = nx; b->doff = doff;
        b->np = np; memcpy(b->par, par, sizeof(size_t) * np);
        if (id == 2 || id == 3) {
            if (np >= 7) return 0;
            par[np] = o;
            if (!gim_walk(g, o + doff, o + size, par, np + 1, out, nout)) return 0;
        }
        o += size;
    }
    return 1;
}
static int gim_bpp(uint16_t fmt) {
    switch (fmt) { case 0: case 1: case 2: return 16; case 3: return 32; case 4: return 4; case 5: return 8; default: return 0; }
}
/* lin (rows x s) <-> PSP swizzle (16 bytes x 8 rows blocks) */
static void unswizzle(const uint8_t *src, uint8_t *lin, uint32_t s, uint32_t rows) {
    const uint8_t *p = src;
    for (uint32_t by = 0; by < rows / 8; by++)
        for (uint32_t bx = 0; bx < s / 16; bx++)
            for (uint32_t r = 0; r < 8; r++, p += 16) memcpy(lin + (size_t)(by * 8 + r) * s + bx * 16, p, 16);
}
static void swizzle(const uint8_t *lin, uint8_t *dst, uint32_t s, uint32_t rows) {
    uint8_t *p = dst;
    for (uint32_t by = 0; by < rows / 8; by++)
        for (uint32_t bx = 0; bx < s / 16; bx++)
            for (uint32_t r = 0; r < 8; r++, p += 16) memcpy(p, lin + (size_t)(by * 8 + r) * s + bx * 16, 16);
}
/* relayout one image block in place of buffer *pg (length *plen); returns 1 if changed */
static int gim_fix_image(uint8_t **pg, size_t *plen, const GimBlk *b) {
    uint8_t *g = *pg; size_t glen = *plen;
    size_t ih = b->off + b->doff;
    if (ih + 48 > glen) return 0;
    uint16_t fmt = rd16(g + ih + 4), order = rd16(g + ih + 6), w = rd16(g + ih + 8), h = rd16(g + ih + 10);
    uint16_t pa = rd16(g + ih + 14), hal = rd16(g + ih + 16);
    uint32_t idx = rd32(g + ih + 24), ps = rd32(g + ih + 28), pe = rd32(g + ih + 32);
    uint16_t lvc = rd16(g + ih + 42), frc = rd16(g + ih + 46);
    int bpp = gim_bpp(fmt);
    if (!bpp || lvc != 1 || order > 1 || !pa || !hal || !frc) return 0;
    uint32_t os = alignu(((uint32_t)w * bpp + 7) / 8, pa), orows = alignu(h, hal);
    uint32_t ns = p2u(w) * bpp / 8; if (ns < 16) ns = 16;
    uint32_t nrows = p2u(h); if (nrows < hal) nrows = hal;
    if (order == 1 && (orows % 8 || os % 16)) return 0;
    uint32_t fsz = os * orows, nsz = ns * nrows;
    if (ih + idx + 4u * frc > glen) die("bad GIM frame table");
    for (uint32_t k = 0; k < frc; k++) if (rd32(g + ih + idx + 4 * k) != ps + k * fsz) die("unexpected GIM frame layout");
    if (pe < ps + frc * fsz || ih + pe > b->off + b->size) die("bad GIM pixel data");
    if (os == ns && orows == nrows && pa == ns && hal == nrows && pe == ps + frc * nsz) return 0;
    size_t newpix = (size_t)frc * nsz;
    uint8_t *pix = xmalloc(newpix); memset(pix, 0, newpix);
    uint8_t *lin = xmalloc((size_t)nrows * ns), *tmp = xmalloc(fsz ? fsz : 1);
    uint32_t cw = os < ns ? os : ns;
    for (uint32_t k = 0; k < frc; k++) {
        const uint8_t *fr = g + ih + ps + (size_t)k * fsz;
        memset(lin, 0, (size_t)nrows * ns);
        const uint8_t *src = fr;
        if (order == 1) { unswizzle(fr, tmp, os, orows); src = tmp; }
        for (uint32_t y = 0; y < orows; y++) memcpy(lin + (size_t)y * ns, src + (size_t)y * os, cw);
        if (order == 1) swizzle(lin, pix + (size_t)k * nsz, ns, nrows);
        else memcpy(pix + (size_t)k * nsz, lin, nsz);
    }
    free(lin); free(tmp);
    size_t oldpix = pe - ps;
    size_t nlen = glen - oldpix + newpix;
    uint8_t *o = xmalloc(nlen);
    memcpy(o, g, ih + ps); memcpy(o + ih + ps, pix, newpix);
    memcpy(o + ih + ps + newpix, g + ih + pe, glen - (ih + pe));
    free(pix);
    int32_t delta = (int32_t)newpix - (int32_t)oldpix;
    o[ih + 14] = (uint8_t)ns; o[ih + 15] = (uint8_t)(ns >> 8);
    o[ih + 16] = (uint8_t)nrows; o[ih + 17] = (uint8_t)(nrows >> 8);
    wr32le(o + ih + 32, ps + (uint32_t)newpix);
    for (uint32_t k = 0; k < frc; k++) wr32le(o + ih + idx + 4 * k, ps + k * nsz);
    wr32le(o + b->off + 4, b->size + delta);
    if (b->next == b->size) wr32le(o + b->off + 8, b->next + delta);
    for (int k = 0; k < b->np; k++) { size_t p = b->par[k]; wr32le(o + p + 4, rd32(o + p + 4) + delta); }
    free(g); *pg = o; *plen = nlen;
    return 1;
}
/* fix all images of one GIM; returns new buffer (caller frees) or NULL if unchanged */
static uint8_t *gim_fix(const uint8_t *g0, size_t glen, size_t *newlen) {
    if (glen < 32 || memcmp(g0, "MIG.00.1PSP", 11) != 0) return NULL;
    size_t rsize = rd32(g0 + 20);
    if (16 + rsize > glen) return NULL;
    GimBlk bl[64]; int nb = 0; size_t par[8];
    if (!gim_walk(g0, 16, 16 + rsize, par, 0, bl, &nb)) return NULL;
    uint8_t *g = xmalloc(glen); memcpy(g, g0, glen);
    size_t len = glen; int changed = 0;
    for (int k = nb - 1; k >= 0; k--)      /* last first: earlier offsets stay valid */
        if (bl[k].id == 4) changed |= gim_fix_image(&g, &len, &bl[k]);
    if (!changed) { free(g); return NULL; }
    *newlen = len;
    return g;
}
/* fix all GIMs of an FCHN chain; returns NULL if nothing changed */
static uint8_t *fchn_fix(const uint8_t *d, size_t n, size_t *newlen) {
    if (n < 8 || memcmp(d, "FCHN", 4) != 0) return NULL;
    uint32_t hs = rd16(d + 4), cnt = rd16(d + 6);
    if (hs != 8 + 8 * cnt || hs > n) return NULL;
    uint32_t *sz = xmalloc(4 * (cnt + 1)), *of = xmalloc(4 * (cnt + 1)), *ord = xmalloc(4 * (cnt + 1));
    for (uint32_t i = 0; i < cnt; i++) { sz[i] = rd32(d + 8 + 8 * i); of[i] = rd32(d + 12 + 8 * i); ord[i] = i; }
    /* sort by (offset, size) */
    for (uint32_t i = 1; i < cnt; i++) {
        uint32_t t = ord[i]; int j = (int)i - 1;
        while (j >= 0 && (of[ord[j]] > of[t] || (of[ord[j]] == of[t] && sz[ord[j]] > sz[t]))) { ord[j + 1] = ord[j]; j--; }
        ord[j + 1] = t;
    }
    size_t cap = n * 2 + 1024, bl = 0; uint8_t *body = xmalloc(cap);
    uint32_t *nsz = xmalloc(4 * (cnt + 1)), *nof = xmalloc(4 * (cnt + 1));
    size_t pos = 0; int changed = 0;
#define BODY_ADD(src, len) do { size_t L_ = (len); if (bl + L_ > cap) { while (bl + L_ > cap) cap *= 2; uint8_t *q_ = realloc(body, cap); if (!q_) die("out of memory"); body = q_; } memcpy(body + bl, (src), L_); bl += L_; } while (0)
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t i = ord[k];
        if ((size_t)hs + of[i] + sz[i] > n) die("bad FCHN member");
        if (of[i] > pos) { BODY_ADD(d + hs + pos, of[i] - pos); pos = of[i]; }
        if (of[i] < pos) die("overlapping FCHN members");
        const uint8_t *m = d + hs + of[i];
        size_t ml = sz[i], gl = 0;
        if (ml >= 24 && memcmp(m, "MIG.", 4) == 0) gl = 16 + rd32(m + 20);
        nof[i] = (uint32_t)bl;
        size_t pl = 0; uint8_t *pg = (gl && gl <= ml) ? gim_fix(m, gl, &pl) : NULL;
        if (pg) {
            BODY_ADD(pg, pl); BODY_ADD(m + gl, ml - gl);
            nsz[i] = (uint32_t)(pl + ml - gl); changed = 1; free(pg);
        } else { BODY_ADD(m, ml); nsz[i] = (uint32_t)ml; }
        pos = of[i] + sz[i];
    }
    if (!changed) { free(body); free(sz); free(of); free(ord); free(nsz); free(nof); return NULL; }
    size_t tail = n - hs - pos;
    uint8_t *o = xmalloc(hs + bl + tail);
    memcpy(o, d, hs);
    for (uint32_t i = 0; i < cnt; i++) { wr32le(o + 8 + 8 * i, nsz[i]); wr32le(o + 12 + 8 * i, nof[i]); }
    memcpy(o + hs, body, bl); memcpy(o + hs + bl, d + hs + pos, tail);
    *newlen = hs + bl + tail;
    free(body); free(sz); free(of); free(ord); free(nsz); free(nof);
    return o;
}

/* fix an FPAC entry: FCHN chain or a bare GIM. NULL if unchanged */
static uint8_t *entry_fix(const uint8_t *d, size_t n, size_t *newlen) {
    if (n >= 24 && memcmp(d, "MIG.", 4) == 0) {
        size_t gl = 16 + rd32(d + 20);
        if (gl > n) return NULL;
        size_t pl = 0; uint8_t *g = gim_fix(d, gl, &pl);
        if (!g) return NULL;
        uint8_t *o = xmalloc(pl + n - gl);
        memcpy(o, g, pl); memcpy(o + pl, d + gl, n - gl); free(g);
        *newlen = pl + n - gl;
        return o;
    }
    return fchn_fix(d, n, newlen);
}
