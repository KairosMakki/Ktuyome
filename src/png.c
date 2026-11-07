#include "ktuyome.h"
/* PNG support (1.4).
 *  - Decoder: zlib/inflate (stored, fixed and dynamic blocks), every colour type and bit depth,
 *    PLTE + tRNS transparency, Adam7 interlacing. Output is premultiplied ARGB like svg_render().
 *  - Encoder: 8-bit RGB, adaptive scanline filters, LZ77 hash-chain matcher, fixed Huffman codes.
 *  - png_fit(): letterbox an image into a square icon/thumbnail. */

/* ============================== checksums ============================== */
static u32 crc_tab[256]; static int crc_ready;
static void crc_init(void) {
    for (u32 n = 0; n < 256; n++) { u32 c = n; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[n] = c; }
    crc_ready = 1;
}
static u32 crc_update(u32 crc, const u8 *p, u32 n) {
    if (!crc_ready) crc_init();
    crc = ~crc;
    while (n--) crc = crc_tab[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}
static u32 adler32(const u8 *p, u32 n) {
    u32 a = 1, b = 0;
    while (n) {
        u32 k = n < 5552 ? n : 5552; n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* ============================== inflate ============================== */
typedef struct { u16 cnt[16]; u16 sym[288]; } Huff;
typedef struct { const u8 *s; u32 n, pos; u32 bb; int bc; u8 *o; u32 on, op; int err; } Inf;

static const u16 lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8  dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static const u8  clorder[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

static int ib(Inf *d) {
    if (!d->bc) { if (d->pos >= d->n) { d->err = 1; return 0; } d->bb = d->s[d->pos++]; d->bc = 8; }
    int b = (int)(d->bb & 1); d->bb >>= 1; d->bc--; return b;
}
static u32 ibits(Inf *d, int n) { u32 v = 0; for (int i = 0; i < n; i++) v |= (u32)ib(d) << i; return v; }
static void huff_build(Huff *t, const u8 *len, int num) {
    u16 offs[16]; int sum = 0;
    for (int i = 0; i < 16; i++) t->cnt[i] = 0;
    for (int i = 0; i < num; i++) t->cnt[len[i]]++;
    t->cnt[0] = 0;
    for (int i = 0; i < 16; i++) { offs[i] = (u16)sum; sum += t->cnt[i]; }
    for (int i = 0; i < num; i++) if (len[i]) t->sym[offs[len[i]]++] = (u16)i;
}
static int huff_dec(Inf *d, const Huff *t) {
    int sum = 0, cur = 0, len = 0;
    do {
        cur = 2 * cur + ib(d); len++;
        if (len > 15 || d->err) { d->err = 1; return 0; }
        sum += t->cnt[len]; cur -= t->cnt[len];
    } while (cur >= 0);
    return t->sym[sum + cur];
}
static Huff fix_lt, fix_dt, dyn_lt, dyn_dt, cl_t; static int fix_ready;
static u8 dyn_len[320];
static void fix_init(void) {
    u8 l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    huff_build(&fix_lt, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    huff_build(&fix_dt, l, 30);
    fix_ready = 1;
}
static void inf_block(Inf *d, const Huff *lt, const Huff *dt) {
    for (;;) {
        int s = huff_dec(d, lt);
        if (d->err) return;
        if (s < 256) { if (d->op >= d->on) { d->err = 1; return; } d->o[d->op++] = (u8)s; }
        else if (s == 256) return;
        else {
            s -= 257;
            if (s >= 29) { d->err = 1; return; }
            u32 len = lbase[s] + ibits(d, lext[s]);
            int ds = huff_dec(d, dt);
            if (ds >= 30 || d->err) { d->err = 1; return; }
            u32 dist = dbase[ds] + ibits(d, dext[ds]);
            if (dist > d->op || d->op + len > d->on) { d->err = 1; return; }
            u8 *o = d->o + d->op; const u8 *from = o - dist;
            for (u32 i = 0; i < len; i++) o[i] = from[i];
            d->op += len;
        }
    }
}
static int inf_dynamic(Inf *d) {
    int hlit = (int)ibits(d, 5) + 257, hdist = (int)ibits(d, 5) + 1, hclen = (int)ibits(d, 4) + 4;
    if (hlit > 286 || hdist > 30) return 0;
    u8 cl[19]; for (int i = 0; i < 19; i++) cl[i] = 0;
    for (int i = 0; i < hclen; i++) cl[clorder[i]] = (u8)ibits(d, 3);
    huff_build(&cl_t, cl, 19);
    int n = 0;
    while (n < hlit + hdist) {
        int s = huff_dec(d, &cl_t);
        if (d->err) return 0;
        if (s < 16) dyn_len[n++] = (u8)s;
        else {
            int rep, val = 0;
            if (s == 16) { if (!n) return 0; val = dyn_len[n - 1]; rep = 3 + (int)ibits(d, 2); }
            else if (s == 17) rep = 3 + (int)ibits(d, 3);
            else rep = 11 + (int)ibits(d, 7);
            if (n + rep > hlit + hdist) return 0;
            while (rep--) dyn_len[n++] = (u8)val;
        }
    }
    huff_build(&dyn_lt, dyn_len, hlit);
    huff_build(&dyn_dt, dyn_len + hlit, hdist);
    return !d->err;
}
/* zlib stream -> out. Returns bytes produced or -1. */
static int z_inflate(const u8 *src, u32 n, u8 *out, u32 outcap) {
    if (n < 6 || (src[0] & 15) != 8 || ((src[0] << 8) | src[1]) % 31 || (src[1] & 0x20)) return -1;
    if (!fix_ready) fix_init();
    Inf d; d.s = src + 2; d.n = n - 2; d.pos = 0; d.bb = 0; d.bc = 0; d.o = out; d.on = outcap; d.op = 0; d.err = 0;
    int last;
    do {
        last = ib(&d);
        int type = (int)ibits(&d, 2);
        if (d.err) return -1;
        if (type == 0) {
            d.bc = 0;                                         /* drop to a byte boundary */
            if (d.pos + 4 > d.n) return -1;
            u32 len = d.s[d.pos] | (d.s[d.pos + 1] << 8), nlen = d.s[d.pos + 2] | (d.s[d.pos + 3] << 8);
            d.pos += 4;
            if ((len ^ 0xFFFF) != nlen || d.pos + len > d.n || d.op + len > d.on) return -1;
            memcpy(d.o + d.op, d.s + d.pos, len); d.pos += len; d.op += len;
        } else if (type == 1) inf_block(&d, &fix_lt, &fix_dt);
        else if (type == 2) { if (!inf_dynamic(&d)) return -1; inf_block(&d, &dyn_lt, &dyn_dt); }
        else return -1;
        if (d.err) return -1;
    } while (!last);
    return (int)d.op;
}

/* ============================== decoder ============================== */
static const u8 a7x[7] = { 0, 4, 0, 2, 0, 1, 0 }, a7y[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const u8 a7dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, a7dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static void unfilter(u8 *r, int rb, int nrows, int bpb) {
    const u8 *prev = NULL;
    for (int y = 0; y < nrows; y++, r += rb + 1) {
        int f = r[0]; u8 *c = r + 1;
        if (f == 1) { for (int i = bpb; i < rb; i++) c[i] = (u8)(c[i] + c[i - bpb]); }
        else if (f == 2) { if (prev) for (int i = 0; i < rb; i++) c[i] = (u8)(c[i] + prev[i]); }
        else if (f == 3) {
            for (int i = 0; i < rb; i++) { int a = i >= bpb ? c[i - bpb] : 0, b = prev ? prev[i] : 0; c[i] = (u8)(c[i] + ((a + b) >> 1)); }
        } else if (f == 4) {
            for (int i = 0; i < rb; i++) {
                int a = i >= bpb ? c[i - bpb] : 0, b = prev ? prev[i] : 0, cc = (prev && i >= bpb) ? prev[i - bpb] : 0;
                int p = a + b - cc, pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - cc);
                c[i] = (u8)(c[i] + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : cc)));
            }
        }
        prev = c;
    }
}
static u32 samp(const u8 *c, int i, int depth) {
    if (depth == 8) return c[i];
    if (depth == 16) return ((u32)c[2 * i] << 8) | c[2 * i + 1];
    int per = 8 / depth, sh = 8 - depth * (i % per + 1);
    return (u32)((c[i / per] >> sh) & ((1 << depth) - 1));
}
static u32 pm(int a, int r, int g, int b) {
    if (a == 255) return ARGB(255, r, g, b);
    if (a == 0) return 0;
    return ARGB(a, (r * a + 127) / 255, (g * a + 127) / 255, (b * a + 127) / 255);
}
static u8 pal_rgb[256 * 3], pal_a[256];
static int has_key; static u32 key_v[3];

static u32 conv_pixel(const u8 *c, int x, int ctype, int depth) {
    int hi = depth == 16;
    switch (ctype) {
    case 0: {
        u32 v = samp(c, x, depth); int g = hi ? (int)(v >> 8) : depth == 8 ? (int)v : (int)(v * 255 / ((1u << depth) - 1));
        return pm((has_key && v == key_v[0]) ? 0 : 255, g, g, g);
    }
    case 2: {
        u32 r = samp(c, 3 * x, depth), g = samp(c, 3 * x + 1, depth), b = samp(c, 3 * x + 2, depth);
        int a = (has_key && r == key_v[0] && g == key_v[1] && b == key_v[2]) ? 0 : 255;
        return hi ? pm(a, (int)(r >> 8), (int)(g >> 8), (int)(b >> 8)) : pm(a, (int)r, (int)g, (int)b);
    }
    case 3: {
        u32 i = samp(c, x, depth);
        return pm(pal_a[i & 255], pal_rgb[(i & 255) * 3], pal_rgb[(i & 255) * 3 + 1], pal_rgb[(i & 255) * 3 + 2]);
    }
    case 4: {
        u32 g = samp(c, 2 * x, depth), a = samp(c, 2 * x + 1, depth);
        if (hi) { g >>= 8; a >>= 8; }
        return pm((int)a, (int)g, (int)g, (int)g);
    }
    default: {
        u32 r = samp(c, 4 * x, depth), g = samp(c, 4 * x + 1, depth), b = samp(c, 4 * x + 2, depth), a = samp(c, 4 * x + 3, depth);
        if (hi) { r >>= 8; g >>= 8; b >>= 8; a >>= 8; }
        return pm((int)a, (int)r, (int)g, (int)b);
    }
    }
}

int png_is(const char *name) { return ends_with(name, ".png") || ends_with(name, ".PNG"); }

int png_decode(const void *data, int len, PngImage *img) {
    static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    const u8 *p = (const u8 *)data;
    img->w = img->h = 0; img->px = NULL;
    if (len < 33 || memcmp(p, sig, 8)) return 0;
    int w = 0, h = 0, depth = 0, ctype = -1, interlace = 0, npal = 0, seen_hdr = 0;
    u32 zlen = 0;
    has_key = 0;
    for (int i = 0; i < 256; i++) pal_a[i] = 255;
    int pos = 8;
    while (pos + 12 <= len) {                                     /* pass 1: headers, palette, total IDAT size */
        u32 cl = be32(p + pos); const u8 *ty = p + pos + 4, *b = p + pos + 8;
        if (cl > (u32)(len - pos - 12)) break;
        if (!memcmp(ty, "IHDR", 4) && cl == 13) {
            w = (int)be32(b); h = (int)be32(b + 4); depth = b[8]; ctype = b[9]; interlace = b[12];
            if (b[10] || b[11] || interlace > 1) return 0;
            seen_hdr = 1;
        } else if (!memcmp(ty, "PLTE", 4)) {
            npal = (int)(cl / 3); if (npal > 256) npal = 256;
            for (int i = 0; i < npal * 3; i++) pal_rgb[i] = b[i];
        } else if (!memcmp(ty, "tRNS", 4) && seen_hdr) {
            if (ctype == 3) { for (u32 i = 0; i < cl && i < 256; i++) pal_a[i] = b[i]; }
            else if (ctype == 0 && cl >= 2) { has_key = 1; key_v[0] = ((u32)b[0] << 8) | b[1]; }
            else if (ctype == 2 && cl >= 6) { has_key = 1; for (int k = 0; k < 3; k++) key_v[k] = ((u32)b[2 * k] << 8) | b[2 * k + 1]; }
        } else if (!memcmp(ty, "IDAT", 4)) zlen += cl;
        else if (!memcmp(ty, "IEND", 4)) break;
        pos += 12 + (int)cl;
    }
    if (!seen_hdr || zlen == 0) return 0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (u32)w * (u32)h > 6000000u) return 0;
    int ok = (ctype == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
             (ctype == 2 && (depth == 8 || depth == 16)) ||
             (ctype == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8) && npal > 0) ||
             (ctype == 4 && (depth == 8 || depth == 16)) ||
             (ctype == 6 && (depth == 8 || depth == 16));
    if (!ok) return 0;
    if (has_key && depth < 16 && ctype != 3) { /* keys are stored as 16-bit values holding the raw sample */ }
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    int bpp = ch * depth, bpb = imax(1, bpp / 8);
    u32 rawsize = 0;
    if (!interlace) rawsize = (u32)h * (u32)((w * bpp + 7) / 8 + 1);
    else for (int ps = 0; ps < 7; ps++) {
        int pw = (w - a7x[ps] + a7dx[ps] - 1) / a7dx[ps], ph = (h - a7y[ps] + a7dy[ps] - 1) / a7dy[ps];
        if (pw > 0 && ph > 0) rawsize += (u32)ph * (u32)((pw * bpp + 7) / 8 + 1);
    }
    u8 *z = kmalloc(zlen), *raw = kmalloc(rawsize);
    u32 *px = kmalloc((u32)w * (u32)h * 4);
    if (!z || !raw || !px) { kfree(z); kfree(raw); kfree(px); return 0; }
    u32 zo = 0; pos = 8;
    while (pos + 12 <= len) {                                     /* pass 2: gather IDAT */
        u32 cl = be32(p + pos);
        if (cl > (u32)(len - pos - 12)) break;
        if (!memcmp(p + pos + 4, "IDAT", 4)) { memcpy(z + zo, p + pos + 8, cl); zo += cl; }
        else if (!memcmp(p + pos + 4, "IEND", 4)) break;
        pos += 12 + (int)cl;
    }
    int got = z_inflate(z, zlen, raw, rawsize);
    kfree(z);
    if (got < (int)rawsize) { kfree(raw); kfree(px); return 0; }
    memset(px, 0, (u32)w * (u32)h * 4);
    u8 *r = raw;
    if (!interlace) {
        int rb = (w * bpp + 7) / 8;
        unfilter(r, rb, h, bpb);
        for (int y = 0; y < h; y++, r += rb + 1) for (int x = 0; x < w; x++) px[y * w + x] = conv_pixel(r + 1, x, ctype, depth);
    } else {
        for (int ps = 0; ps < 7; ps++) {
            int pw = (w - a7x[ps] + a7dx[ps] - 1) / a7dx[ps], ph = (h - a7y[ps] + a7dy[ps] - 1) / a7dy[ps];
            if (pw <= 0 || ph <= 0) continue;
            int rb = (pw * bpp + 7) / 8;
            unfilter(r, rb, ph, bpb);
            for (int y = 0; y < ph; y++, r += rb + 1) for (int x = 0; x < pw; x++)
                px[(a7y[ps] + y * a7dy[ps]) * w + a7x[ps] + x * a7dx[ps]] = conv_pixel(r + 1, x, ctype, depth);
        }
    }
    kfree(raw);
    img->w = w; img->h = h; img->px = px;
    return 1;
}
void png_free(PngImage *img) { if (img->px) kfree(img->px); img->px = NULL; img->w = img->h = 0; }

/* Letterbox the image into size x size (premultiplied ARGB, transparent borders). */
void png_fit(const PngImage *im, u32 *out, int size) {
    memset(out, 0, (u32)size * size * 4);
    if (!im->px || im->w <= 0 || im->h <= 0) return;
    int dw, dh;
    if (im->w >= im->h) { dw = size; dh = imax(1, im->h * size / im->w); }
    else { dh = size; dw = imax(1, im->w * size / im->h); }
    int ox = (size - dw) / 2, oy = (size - dh) / 2;
    if (dw >= im->w && dh >= im->h) {                             /* same size or enlarging: bilinear (premultiplied, so channels blend directly) */
        int stx = (im->w << 16) / dw, sty = (im->h << 16) / dh;
        for (int y = 0; y < dh; y++) {
            int sy = y * sty + (sty >> 1) - 32768, y0 = sy >> 16, fy = (sy >> 8) & 255, y1 = y0 + 1;
            y0 = iclamp(y0, 0, im->h - 1); y1 = iclamp(y1, 0, im->h - 1);
            for (int x = 0; x < dw; x++) {
                int sx = x * stx + (stx >> 1) - 32768, x0 = sx >> 16, fx = (sx >> 8) & 255, x1 = x0 + 1;
                x0 = iclamp(x0, 0, im->w - 1); x1 = iclamp(x1, 0, im->w - 1);
                u32 p00 = im->px[y0 * im->w + x0], p10 = im->px[y0 * im->w + x1], p01 = im->px[y1 * im->w + x0], p11 = im->px[y1 * im->w + x1], o = 0;
                for (int sh = 0; sh < 32; sh += 8) {
                    int a = (p00 >> sh) & 255, b = (p10 >> sh) & 255, c = (p01 >> sh) & 255, d = (p11 >> sh) & 255;
                    int t = a + (((b - a) * fx) >> 8), u = c + (((d - c) * fx) >> 8);
                    o |= (u32)(t + (((u - t) * fy) >> 8)) << sh;
                }
                out[(oy + y) * size + ox + x] = o;
            }
        }
        return;
    }
    for (int y = 0; y < dh; y++) {                                /* shrinking: box filter */
        int sy0 = y * im->h / dh, sy1 = (y + 1) * im->h / dh; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * im->w / dw, sx1 = (x + 1) * im->w / dw; if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 a = 0, r = 0, g = 0, b = 0, n = (u32)(sy1 - sy0) * (u32)(sx1 - sx0);
            for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) {
                u32 c = im->px[sy * im->w + sx];
                a += c >> 24; r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
            }
            out[(oy + y) * size + ox + x] = ARGB(a / n, r / n, g / n, b / n);
        }
    }
}

/* ============================== encoder ============================== */
typedef struct { u8 *o; u32 cap, n, acc; int nb, err; } BW;
static void bw_bits(BW *b, u32 v, int n) {                       /* LSB first */
    b->acc |= v << b->nb; b->nb += n;
    while (b->nb >= 8) {
        if (b->n < b->cap) b->o[b->n++] = (u8)b->acc; else b->err = 1;
        b->acc >>= 8; b->nb -= 8;
    }
}
static u16 f_code[288]; static u8 f_len[288]; static u8 f_dcode[30]; static u8 f_lsym[259]; static int f_ready;
static u32 rev(u32 v, int n) { u32 r = 0; for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static void enc_init(void) {
    for (int s = 0; s < 288; s++) {
        u32 c; int l;
        if (s < 144) { c = 0x30 + s; l = 8; } else if (s < 256) { c = 0x190 + (s - 144); l = 9; }
        else if (s < 280) { c = s - 256; l = 7; } else { c = 0xC0 + (s - 280); l = 8; }
        f_code[s] = (u16)rev(c, l); f_len[s] = (u8)l;
    }
    for (int s = 0; s < 30; s++) f_dcode[s] = (u8)rev((u32)s, 5);
    for (int l = 3; l <= 257; l++) { int i = 27; while (lbase[i] > l) i--; f_lsym[l] = (u8)i; }
    f_lsym[258] = 28;
    f_ready = 1;
}
static void put_lit(BW *b, int s) { bw_bits(b, f_code[s], f_len[s]); }
static void put_match(BW *b, int len, int dist) {
    int ls = f_lsym[len];
    put_lit(b, 257 + ls);
    if (lext[ls]) bw_bits(b, (u32)(len - lbase[ls]), lext[ls]);
    int ds = 29; while (dbase[ds] > dist) ds--;
    bw_bits(b, f_dcode[ds], 5);
    if (dext[ds]) bw_bits(b, (u32)(dist - dbase[ds]), dext[ds]);
}
static int lz_head[1 << 15], lz_prv[32768];
#define LZ_CHAIN 10
#define LZ_NICE 96
static inline u32 lz_hash(const u8 *p) { return (((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16) * 2654435761u) >> 17; }
static void lz_ins(const u8 *raw, int pos, int n) {
    if (pos + 3 > n) return;
    u32 h = lz_hash(raw + pos);
    lz_prv[pos & 32767] = lz_head[h]; lz_head[h] = pos;
}
static void deflate_fixed(BW *b, const u8 *raw, int n) {
    for (int i = 0; i < (1 << 15); i++) lz_head[i] = -1;
    bw_bits(b, 1, 1); bw_bits(b, 1, 2);                           /* final block, fixed Huffman */
    int i = 0;
    while (i < n) {
        int bl = 0, bd = 0;
        if (i + 3 <= n) {
            int cand = lz_head[lz_hash(raw + i)], chain = LZ_CHAIN, maxl = imin(258, n - i);
            while (cand >= 0 && i - cand <= 32768 && chain--) {
                int l = 0; while (l < maxl && raw[cand + l] == raw[i + l]) l++;
                if (l > bl) { bl = l; bd = i - cand; if (l >= maxl || l >= LZ_NICE) break; }
                int nx = lz_prv[cand & 32767]; if (nx >= cand) break; cand = nx;
            }
        }
        if (bl >= 3) { put_match(b, bl, bd); for (int k = 0; k < bl; k++) lz_ins(raw, i + k, n); i += bl; }
        else { put_lit(b, raw[i]); lz_ins(raw, i, n); i++; }
        if (b->err) return;
    }
    put_lit(b, 256);
    if (b->nb) bw_bits(b, 0, 8 - b->nb);
}
int png_max_size(int w, int h) { u32 raw = (u32)h * (u32)(w * 3 + 1); return (int)(raw + raw / 8 + 512); }

static u8 fb_cur[MAXW * 3], fb_prev[MAXW * 3], fb_try[5][MAXW * 3];
/* Encode w x h pixels (0x??RRGGBB, alpha ignored) as an 8-bit RGB PNG. Returns the size or -1. */
int png_encode(const u32 *px, int w, int h, u8 *out, int cap) {
    if (w <= 0 || h <= 0 || w > MAXW || h > MAXH) return -1;
    if (!f_ready) enc_init();
    int rb = w * 3; u32 rawn = (u32)h * (u32)(rb + 1);
    u8 *raw = kmalloc(rawn);
    if (!raw) return -1;
    memset(fb_prev, 0, (u32)rb);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) { u32 c = px[y * w + x]; fb_cur[3 * x] = (u8)(c >> 16); fb_cur[3 * x + 1] = (u8)(c >> 8); fb_cur[3 * x + 2] = (u8)c; }
        int sum[5] = { 0, 0, 0, 0, 0 };
        for (int i = 0; i < rb; i++) {
            int c = fb_cur[i], a = i >= 3 ? fb_cur[i - 3] : 0, b = fb_prev[i], cc = i >= 3 ? fb_prev[i - 3] : 0;
            int p = a + b - cc, pa = iabs(p - a), pb = iabs(p - b), pc = iabs(p - cc);
            u8 v[5];
            v[0] = (u8)c; v[1] = (u8)(c - a); v[2] = (u8)(c - b); v[3] = (u8)(c - ((a + b) >> 1));
            v[4] = (u8)(c - ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : cc)));
            for (int f = 0; f < 5; f++) { fb_try[f][i] = v[f]; sum[f] += iabs((s8)v[f]); }
        }
        int best = 0; for (int f = 1; f < 5; f++) if (sum[f] < sum[best]) best = f;
        u8 *dst = raw + (u32)y * (u32)(rb + 1);
        dst[0] = (u8)best; memcpy(dst + 1, fb_try[best], (u32)rb);
        memcpy(fb_prev, fb_cur, (u32)rb);
    }
    static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (cap < 64) { kfree(raw); return -1; }
    memcpy(out, sig, 8);
    put32(out + 8, 13); memcpy(out + 12, "IHDR", 4);
    put32(out + 16, (u32)w); put32(out + 20, (u32)h);
    out[24] = 8; out[25] = 2; out[26] = 0; out[27] = 0; out[28] = 0;
    put32(out + 29, crc_update(0, out + 12, 17));
    /* IDAT: length at 33, type at 37, zlib stream from 41 */
    BW b; b.o = out; b.cap = (u32)cap - 16; b.n = 41; b.acc = 0; b.nb = 0; b.err = 0;
    out[41] = 0x78; out[42] = 0x01; b.n = 43;
    deflate_fixed(&b, raw, (int)rawn);
    if (b.err || b.n + 4 > b.cap) { kfree(raw); return -1; }
    put32(out + b.n, adler32(raw, rawn)); b.n += 4;
    kfree(raw);
    u32 dlen = b.n - 41;
    put32(out + 33, dlen); memcpy(out + 37, "IDAT", 4);
    put32(out + b.n, crc_update(0, out + 37, dlen + 4)); b.n += 4;
    put32(out + b.n, 0); memcpy(out + b.n + 4, "IEND", 4); put32(out + b.n + 8, 0xAE426082u); b.n += 12;
    return (int)b.n;
}
