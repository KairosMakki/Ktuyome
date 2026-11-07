#include "ktuyome.h"

int W = 1280, H = 720;
u32 g_bb[MAXW * MAXH];
u32 g_wall[MAXW * MAXH];
u32 g_mbi_mem;
int g_brightness = 100;
static u32 *fb; static int fb_pitch;
static u32 *T; static int TW, TH, cx0, cy0, cx1, cy1;

int gfx_init(u32 mbi) {
    u32 *m = (u32 *)mbi;
    g_mbi_mem = (m[0] & 1) ? m[2] + 1024 : 65536;
    if (!(m[0] & (1 << 12))) return 0;
    u8 bpp = ((u8 *)mbi)[108], type = ((u8 *)mbi)[109];
    if (bpp != 32 || type != 1) return 0;
    fb = (u32 *)m[22]; fb_pitch = (int)m[24];
    W = imin((int)m[25], MAXW); H = imin((int)m[26], MAXH);
    gfx_target(g_bb, W, H);
    return 1;
}
void gfx_target(u32 *p, int w, int h) { T = p; TW = w; TH = h; cx0 = 0; cy0 = 0; cx1 = w; cy1 = h; }
void gfx_clip(int x, int y, int w, int h) {
    cx0 = imax(0, x); cy0 = imax(0, y); cx1 = imin(TW, x + w); cy1 = imin(TH, y + h);
}
void gfx_noclip(void) { cx0 = 0; cy0 = 0; cx1 = TW; cy1 = TH; }

u32 col_lerp(u32 a, u32 b, int t) {
    int c[4];
    for (int i = 0; i < 4; i++) {
        int s = i * 8, x = (a >> s) & 255, y = (b >> s) & 255;
        c[i] = x + (((y - x) * t) >> 8);
    }
    return (u32)c[0] | ((u32)c[1] << 8) | ((u32)c[2] << 16) | ((u32)c[3] << 24);
}
u32 col_alpha(u32 c, int a) { return (c & 0xFFFFFF) | ((u32)(((c >> 24) * a) / 255) << 24); }

static inline u32 blend(u32 d, u32 s, int a) {
    if (a >= 255) return s & 0xFFFFFF;
    if (a <= 0) return d;
    int ia = 255 - a;
    u32 rb = (d & 0xFF00FF) * ia + (s & 0xFF00FF) * a + 0x800080;
    rb = ((rb + ((rb >> 8) & 0xFF00FF)) >> 8) & 0xFF00FF;
    u32 g = ((d >> 8) & 255) * ia + ((s >> 8) & 255) * a + 128;
    g = ((g + (g >> 8)) >> 8) & 255;
    return rb | (g << 8);
}
static inline void span(u32 *row, int xa, int xb, u32 rgb, int a) {
    if (xa < cx0) xa = cx0;
    if (xb > cx1) xb = cx1;
    int n = xb - xa;
    if (n <= 0) return;
    u32 *p = row + xa;
    if (a >= 255) { __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(rgb) : "memory"); return; }
    while (n--) { *p = blend(*p, rgb, a); p++; }
}
static inline void px(u32 *row, int x, u32 rgb, int a) {
    if (x < cx0 || x >= cx1 || a <= 0) return;
    row[x] = blend(row[x], rgb, a);
}

void gfx_fill(int x, int y, int w, int h, u32 argb) {
    int a = argb >> 24;
    if (!a) return;
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int yy = y0; yy < y1; yy++) span(T + yy * TW, x, x + w, argb & 0xFFFFFF, a);
}
void gfx_vgrad(int x, int y, int w, int h, u32 c1, u32 c2) { gfx_rrect(x, y, w, h, 0, c1, c2, 0); }

static inline int cov(float d) { /* d = radius - distance, returns 0..255 coverage */
    float c = d + 0.5f;
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return (int)(c * 255);
}

void gfx_rrect(int x, int y, int w, int h, int r, u32 c1, u32 c2, int thick) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0) r = 0;
    int y0 = imax(y, cy0), y1 = imin(y + h, cy1);
    for (int yy = y0; yy < y1; yy++) {
        u32 c = (c1 == c2) ? c1 : col_lerp(c1, c2, h > 1 ? (yy - y) * 256 / (h - 1) : 0);
        int a = c >> 24; u32 rgb = c & 0xFFFFFF;
        if (!a) continue;
        u32 *row = T + yy * TW;
        int dy = yy - y, top = dy < r, bot = dy >= h - r;
        if (!top && !bot) {
            if (!thick) span(row, x, x + w, rgb, a);
            else { span(row, x, x + thick, rgb, a); span(row, x + w - thick, x + w, rgb, a); }
            continue;
        }
        if (r == 0) {
            if (!thick || dy < thick || dy >= h - thick) span(row, x, x + w, rgb, a);
            continue;
        }
        float fy = (float)yy + 0.5f - (float)(top ? y + r : y + h - r);
        float fy2 = fy * fy;
        int near_edge = thick && (dy < thick || dy >= h - thick);
        if (!thick || near_edge) span(row, x + r, x + w - r, rgb, a);
        for (int i = 0; i < r; i++) {
            float fx = (float)(r - i) - 0.5f;
            float d2 = fx * fx + fy2;
            float dist = k_sqrt(d2);
            int cv = cov((float)r - dist);
            if (thick) {
                int ci = (r - thick > 0) ? cov((float)(r - thick) - dist) : 0;
                cv -= ci;
            }
            if (cv <= 0) continue;
            int aa = a * cv / 255;
            px(row, x + i, rgb, aa);
            px(row, x + w - 1 - i, rgb, aa);
        }
    }
}
void gfx_circle(int cx, int cy, int r, u32 argb) { gfx_rrect(cx - r, cy - r, r * 2, r * 2, r, argb, argb, 0); }

void gfx_shadow(int x, int y, int w, int h, int r, int blur, int alpha) {
    int sum = blur * (blur + 1) / 2;
    for (int i = blur; i >= 1; i--) {
        int a = alpha * (blur - i + 1) / sum;
        if (a < 1) a = 1;
        gfx_rrect(x - i, y - i, w + 2 * i, h + 2 * i, r + i, BLACK_A(a), BLACK_A(a), 0);
    }
}

static u8 glow_lut[4096]; static int glow_ready;
void gfx_glow(int cx, int cy, int r, u32 rgb, int amax) {
    if (!glow_ready) {
        for (int i = 0; i < 4096; i++) { float t = k_sqrt(i / 4095.0f), v = (1 - t); glow_lut[i] = (u8)(v * v * 255); }
        glow_ready = 1;
    }
    int x0 = imax(cx - r, cx0) & ~1, x1 = imin(cx + r, cx1), y0 = imax(cy - r, cy0) & ~1, y1 = imin(cy + r, cy1);
    u32 r2 = (u32)r * r, inv = (4095u << 20) / r2;
    rgb &= 0xFFFFFF;
    for (int yy = y0; yy < y1; yy += 2) {                 /* evaluated per 2x2 block: smooth falloff, half the cost */
        int dy = yy + 1 - cy;
        for (int xx = x0; xx < x1; xx += 2) {
            int dx = xx + 1 - cx; u32 d2 = (u32)(dx * dx + dy * dy);
            if (d2 >= r2) continue;
            int a = (glow_lut[(d2 * inv) >> 20] * amax) >> 8;
            if (!a) continue;
            for (int j = 0; j < 2; j++) {
                if (yy + j < cy0 || yy + j >= cy1) continue;
                u32 *row = T + (yy + j) * TW;
                for (int i = 0; i < 2; i++)
                    if (xx + i >= cx0 && xx + i < cx1) row[xx + i] = blend(row[xx + i], rgb, a);
            }
        }
    }
}

/* ---------------- text ---------------- */
static inline const Glyph *gl(int f, int c) { if (c < 32 || c > 126) c = '?'; return &fonts[f].g[c - 32]; }
int gfx_text_w(int f, const char *s, int n) {
    int w = 0;
    for (int i = 0; s[i] && (n < 0 || i < n); i++) w += gl(f, (u8)s[i])->adv;
    return w;
}
void gfx_text_n(int f, int x, int y, const char *s, int n, u32 col) {
    const Font *F = &fonts[f];
    int a0 = col >> 24; u32 rgb = col & 0xFFFFFF; int base = y + F->asc;
    if (!a0 || base - F->asc > cy1 || base + F->h < cy0) return;
    for (int i = 0; s[i] && (n < 0 || i < n); i++) {
        const Glyph *g = gl(f, (u8)s[i]);
        if (x > cx1) break;
        if (g->w && x + g->bx + g->w >= cx0) {
            int gx = x + g->bx, gy = base + g->by; const u8 *d = F->d + g->off;
            for (int r = 0; r < g->h; r++) {
                int yy = gy + r;
                if (yy < cy0 || yy >= cy1) continue;
                u32 *row = T + yy * TW;
                for (int c = 0; c < g->w; c++) {
                    int a = d[r * g->w + c];
                    if (!a) continue;
                    int xx = gx + c;
                    if (xx < cx0 || xx >= cx1) continue;
                    a = (a * (a0 + 1)) >> 8;
                    row[xx] = blend(row[xx], rgb, a);
                }
            }
        }
        x += g->adv;
    }
}
void gfx_text(int f, int x, int y, const char *s, u32 col) { gfx_text_n(f, x, y, s, -1, col); }
void gfx_text_c(int f, int cx, int y, const char *s, u32 col) { gfx_text(f, cx - gfx_text_w(f, s, -1) / 2, y, s, col); }
void gfx_text_r(int f, int rx, int y, const char *s, u32 col) { gfx_text(f, rx - gfx_text_w(f, s, -1), y, s, col); }

/* ---------------- bitmaps (premultiplied ARGB) ---------------- */
static inline u32 over_pm(u32 d, u32 s, int ga) {      /* premultiplied src over opaque/any dst, global alpha ga */
    int sa = s >> 24, sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
    if (ga < 255) { sa = sa * ga / 255; sr = sr * ga / 255; sg = sg * ga / 255; sb = sb * ga / 255; }
    int ia = 255 - sa;
    int r = sr + (((d >> 16) & 255) * ia + 127) / 255;
    int g = sg + (((d >> 8) & 255) * ia + 127) / 255;
    int b = sb + ((d & 255) * ia + 127) / 255;
    return ((u32)imin(r, 255) << 16) | ((u32)imin(g, 255) << 8) | (u32)imin(b, 255);
}
void gfx_blit_pm(const u32 *src, int sw, int sh, int dx, int dy, int alpha) {
    for (int j = 0; j < sh; j++) {
        int yy = dy + j;
        if (yy < cy0 || yy >= cy1) continue;
        u32 *row = T + yy * TW;
        for (int i = 0; i < sw; i++) {
            int xx = dx + i;
            if (xx < cx0 || xx >= cx1) continue;
            u32 s = src[j * sw + i];
            if (s) row[xx] = over_pm(row[xx], s, alpha);
        }
    }
}
void gfx_blit_scaled(const u32 *src, int sw, int sh, int dx, int dy, int dw, int dh, int alpha) {
    if (dw <= 0 || dh <= 0) return;
    if (dw == sw && dh == sh) { gfx_blit_pm(src, sw, sh, dx, dy, alpha); return; }
    int stx = (sw << 16) / dw, sty = (sh << 16) / dh;
    int j0 = imax(0, cy0 - dy), j1 = imin(dh, cy1 - dy);
    int i0 = imax(0, cx0 - dx), i1 = imin(dw, cx1 - dx);
    for (int j = j0; j < j1; j++) {
        int sy = j * sty + (sty >> 1) - 32768;
        int y0 = sy >> 16, fy = (sy >> 8) & 255, y1 = y0 + 1;
        y0 = iclamp(y0, 0, sh - 1); y1 = iclamp(y1, 0, sh - 1);
        u32 *row = T + (dy + j) * TW;
        for (int i = i0; i < i1; i++) {
            int sx = i * stx + (stx >> 1) - 32768;
            int x0 = sx >> 16, fx = (sx >> 8) & 255, x1 = x0 + 1;
            x0 = iclamp(x0, 0, sw - 1); x1 = iclamp(x1, 0, sw - 1);
            u32 p00 = src[y0 * sw + x0], p10 = src[y0 * sw + x1], p01 = src[y1 * sw + x0], p11 = src[y1 * sw + x1];
            u32 o = 0;
            for (int s = 0; s < 32; s += 8) {
                int a = (p00 >> s) & 255, b = (p10 >> s) & 255, c = (p01 >> s) & 255, d = (p11 >> s) & 255;
                int t = a + (((b - a) * fx) >> 8), u = c + (((d - c) * fx) >> 8);
                o |= (u32)(t + (((u - t) * fy) >> 8)) << s;
            }
            if (o) row[dx + i] = over_pm(row[dx + i], o, alpha);
        }
    }
}

/* ---------------- present ---------------- */
void gfx_present(void) {
    if (g_brightness < 100) {
        u32 k = (u32)g_brightness * 256 / 100;
        for (int i = 0; i < W * H; i++) {
            u32 c = g_bb[i];
            g_bb[i] = (((c & 0xFF00FF) * k >> 8) & 0xFF00FF) | (((c & 0x00FF00) * k >> 8) & 0x00FF00);
        }
    }
    for (int y = 0; y < H; y++) memcpy((u8 *)fb + y * fb_pitch, g_bb + y * W, W * 4);
}

/* ---------------- wallpapers ---------------- */
void gfx_bake_wallpaper(int theme) {
    static const u32 top[3] = { 0xFF0B1A3C, 0xFF08171F, 0xFF1C0B12 };
    static const u32 bot[3] = { 0xFF03050E, 0xFF03080C, 0xFF060305 };
    gfx_target(g_wall, W, H);
    gfx_vgrad(0, 0, W, H, top[theme], bot[theme]);
    if (theme == 0) {
        gfx_glow(W * 18 / 100, H * 15 / 100, W * 70 / 100, 0x2A5BD7, 150);
        gfx_glow(W * 92 / 100, H * 90 / 100, W * 60 / 100, 0x5B2BD7, 90);
        gfx_glow(W * 60 / 100, H * 45 / 100, W * 40 / 100, 0x1B3C9A, 60);
    } else if (theme == 1) {
        gfx_glow(W * 78 / 100, H * 12 / 100, W * 60 / 100, 0x1FD1A5, 85);
        gfx_glow(W * 8 / 100, H * 92 / 100, W * 65 / 100, 0x7A4DFF, 100);
        gfx_glow(W * 45 / 100, H * 60 / 100, W * 40 / 100, 0x0E6E7A, 70);
    } else {
        gfx_glow(W * 85 / 100, H * 10 / 100, W * 60 / 100, 0xFF5A36, 85);
        gfx_glow(W * 12 / 100, H * 88 / 100, W * 60 / 100, 0xFF2E63, 75);
        gfx_glow(W * 50 / 100, H * 50 / 100, W * 35 / 100, 0x7A1D2E, 70);
    }
    static const u8 bayer[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {   /* ordered dither hides gradient banding */
        u32 c = g_wall[y * W + x]; int b = bayer[(y & 3) * 4 + (x & 3)] >> 3;
        u32 r = imin(255, (int)((c >> 16) & 255) + b), g = imin(255, (int)((c >> 8) & 255) + b), bl = imin(255, (int)(c & 255) + b);
        g_wall[y * W + x] = (r << 16) | (g << 8) | bl;
    }
    gfx_target(g_bb, W, H);
}
