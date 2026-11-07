#include "ktuyome.h"
/* Ktuyome SVG renderer.
 * Supported: svg viewBox/width/height, g, path (M L H V C S Q T A Z, abs+rel), rect (rx/ry), circle, ellipse,
 * line, polyline, polygon, transform (matrix/translate/scale/rotate), fill/stroke/stroke-width/opacity/
 * fill-opacity/stroke-opacity/fill-rule/stroke-linecap, style="" properties, currentColor, #rgb/#rrggbb/
 * rgb()/named colours, linearGradient + radialGradient (objectBoundingBox or userSpaceOnUse, multi-stop).
 * Anti-aliasing: exact signed-area coverage accumulation. Output is premultiplied ARGB. */

#define MAXPT 6000
#define MAXSP 96
#define MAXSZ 256
#define MAXGR 12
#define MAXST 6

typedef struct { float a, b, c, d, e, f; } Mat;
typedef struct { int type; u32 col; int grad; } Paint;   /* type 0 none, 1 solid, 2 gradient */
typedef struct {
    Mat m; Paint fill, stroke; float sw, op, fop, sop; int evenodd, cap_round;
} Style;
typedef struct {
    char id[24]; int radial, user, nst;
    float x1, y1, x2, y2, cx, cy, r; float off[MAXST]; u32 col[MAXST];
} Grad;

static float ptx[MAXPT], pty[MAXPT];
static int sp_start[MAXSP + 1], nsp, npt;
static float acc[(MAXSZ + 3) * (MAXSZ + 3)];
static Grad grads[MAXGR]; static int ngrad;
static u32 *g_out; static int g_size; static u32 g_tint;
static float bx0, by0, bx1, by1;           /* shape device bbox */

/* ---------------------------- small parsing helpers ---------------------------- */
static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ','; }
static float parse_num(const char **pp) {
    const char *p = *pp;
    while (*p && is_ws(*p)) p++;
    float sign = 1, v = 0;
    if (*p == '-') { sign = -1; p++; } else if (*p == '+') p++;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') { p++; float s = 0.1f; while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * s; s *= 0.1f; } }
    if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        p++; int es = 1, ev = 0;
        if (*p == '-') { es = -1; p++; } else if (*p == '+') p++;
        while (*p >= '0' && *p <= '9') ev = ev * 10 + (*p++ - '0');
        float m = 1; for (int i = 0; i < ev; i++) m *= 10;
        v = es > 0 ? v * m : v / m;
    }
    *pp = p;
    return sign * v;
}
static int has_num(const char *p) {
    while (*p && is_ws(*p)) p++;
    return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.';
}
static float parse_len(const char *s, float def) {    /* number with optional unit/% (percent -> fraction) */
    if (!s || !*s) return def;
    const char *p = s; float v = parse_num(&p);
    if (*p == '%') v /= 100.0f;
    return v;
}

/* find attribute `name` inside tag text [t, t+n); falls back to style="name:value" */
static int attr(const char *t, int n, const char *name, char *out, int cap) {
    int nl = (int)strlen(name);
    for (int i = 1; i + nl + 1 < n; i++) {
        if (!is_ws(t[i - 1]) || memcmp(t + i, name, nl) != 0) continue;
        int j = i + nl;
        while (j < n && is_ws(t[j]) && t[j] != ',') j++;
        if (t[j] != '=') continue;
        j++;
        while (j < n && is_ws(t[j])) j++;
        char q = t[j];
        if (q != '"' && q != '\'') continue;
        j++;
        int k = 0;
        while (j < n && t[j] != q && k < cap - 1) out[k++] = t[j++];
        out[k] = 0;
        return 1;
    }
    char sty[320];
    if (strcmp(name, "style") != 0 && attr(t, n, "style", sty, sizeof sty)) {
        int sl = (int)strlen(sty);
        for (int i = 0; i + nl < sl; i++) {
            if ((i == 0 || sty[i - 1] == ';' || sty[i - 1] == ' ') && memcmp(sty + i, name, nl) == 0 && sty[i + nl] == ':') {
                int j = i + nl + 1, k = 0;
                while (sty[j] == ' ') j++;
                while (sty[j] && sty[j] != ';' && k < cap - 1) out[k++] = sty[j++];
                out[k] = 0;
                return 1;
            }
        }
    }
    return 0;
}
static float fattr(const char *t, int n, const char *name, float def) {
    char b[48];
    return attr(t, n, name, b, sizeof b) ? parse_len(b, def) : def;
}

/* ---------------------------- colours ---------------------------- */
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
static const struct { const char *n; u32 c; } named[] = {
    {"white", 0xFFFFFF}, {"black", 0}, {"red", 0xFF0000}, {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
    {"orange", 0xFFA500}, {"gray", 0x808080}, {"grey", 0x808080}, {"purple", 0x800080}, {"pink", 0xFFC0CB},
    {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"lime", 0x00FF00}, {"navy", 0x000080}, {"teal", 0x008080},
    {"silver", 0xC0C0C0}, {"brown", 0xA52A2A}, {"gold", 0xFFD700}, {"crimson", 0xDC143C}, {"coral", 0xFF7F50},
};
static int parse_color(const char *s, u32 tint, u32 *out) {   /* returns 1 ok (out=ARGB), 0 none/invalid */
    while (*s == ' ') s++;
    if (starts_with(s, "none") || starts_with(s, "transparent")) return 0;
    if (starts_with(s, "currentColor")) { *out = tint; return 1; }
    if (*s == '#') {
        int d[8], n = 0;
        for (s++; n < 8 && hexv(*s) >= 0; s++) d[n++] = hexv(*s);
        if (n == 3) { *out = RGB(d[0] * 17, d[1] * 17, d[2] * 17); return 1; }
        if (n == 6) { *out = RGB(d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5]); return 1; }
        if (n == 8) { *out = ARGB(d[6] * 16 + d[7], d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5]); return 1; }
        return 0;
    }
    if (starts_with(s, "rgb")) {
        while (*s && *s != '(') s++;
        const char *p = s + 1; int v[3];
        for (int i = 0; i < 3; i++) {
            float f = parse_num(&p);
            if (*p == '%') { f = f * 2.55f; p++; }
            v[i] = iclamp((int)f, 0, 255);
        }
        *out = RGB(v[0], v[1], v[2]); return 1;
    }
    for (u32 i = 0; i < sizeof(named) / sizeof(named[0]); i++)
        if (starts_with(s, named[i].n)) { *out = 0xFF000000 | named[i].c; return 1; }
    return 0;
}

/* ---------------------------- matrices ---------------------------- */
static Mat mat_mul(Mat p, Mat l) {      /* apply l first, then p */
    Mat r;
    r.a = p.a * l.a + p.c * l.b;  r.b = p.b * l.a + p.d * l.b;
    r.c = p.a * l.c + p.c * l.d;  r.d = p.b * l.c + p.d * l.d;
    r.e = p.a * l.e + p.c * l.f + p.e; r.f = p.b * l.e + p.d * l.f + p.f;
    return r;
}
static Mat parse_transform(const char *s) {
    Mat m = {1, 0, 0, 1, 0, 0};
    while (*s) {
        while (*s && (is_ws(*s))) s++;
        if (!*s) break;
        char name[12]; int k = 0;
        while (*s && *s != '(' && k < 11) name[k++] = *s++;
        name[k] = 0;
        if (*s != '(') break;
        const char *p = s + 1; float v[6] = {0, 0, 0, 0, 0, 0}; int n = 0;
        while (*p && *p != ')' && n < 6) { if (!has_num(p)) break; v[n++] = parse_num(&p); }
        while (*p && *p != ')') p++;
        s = *p ? p + 1 : p;
        Mat t = {1, 0, 0, 1, 0, 0};
        if (!strcmp(name, "matrix") && n == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if (!strcmp(name, "translate")) { t.e = v[0]; t.f = n > 1 ? v[1] : 0; }
        else if (!strcmp(name, "scale")) { t.a = v[0]; t.d = n > 1 ? v[1] : v[0]; }
        else if (!strcmp(name, "rotate")) {
            float r = v[0] * 3.14159265f / 180.0f, c = k_cos(r), sn = k_sin(r);
            t.a = c; t.b = sn; t.c = -sn; t.d = c;
            if (n >= 3) { t.e = v[1] - c * v[1] + sn * v[2]; t.f = v[2] - sn * v[1] - c * v[2]; }
        }
        m = mat_mul(m, t);
    }
    return m;
}
static inline void xf(const Mat *m, float x, float y, float *ox, float *oy) {
    *ox = m->a * x + m->c * y + m->e; *oy = m->b * x + m->d * y + m->f;
}

/* ---------------------------- path building ---------------------------- */
static float cur_x, cur_y, start_x, start_y, last_cx, last_cy; static int cur_cmd_curve;
static int flat_n;     /* curve subdivision count */

static int need_new;
static void new_sub(void) { if (nsp < MAXSP) { sp_start[nsp++] = npt; } sp_start[nsp] = npt; }
static void add_pt(const Mat *m, float x, float y) {
    if (npt >= MAXPT || nsp == 0) return;
    xf(m, x, y, &ptx[npt], &pty[npt]); npt++; sp_start[nsp] = npt;
}
static void move_to(const Mat *m, float x, float y) { new_sub(); need_new = 0; add_pt(m, x, y); cur_x = start_x = x; cur_y = start_y = y; }
static void ensure_sub(const Mat *m) { if (!nsp || need_new) { new_sub(); need_new = 0; add_pt(m, cur_x, cur_y); } }
static void line_to(const Mat *m, float x, float y) { ensure_sub(m); add_pt(m, x, y); cur_x = x; cur_y = y; }
static void cubic_to(const Mat *m, float x1, float y1, float x2, float y2, float x, float y) {
    ensure_sub(m);
    float x0 = cur_x, y0 = cur_y;
    for (int i = 1; i <= flat_n; i++) {
        float t = (float)i / flat_n, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        add_pt(m, a * x0 + b * x1 + c * x2 + d * x, a * y0 + b * y1 + c * y2 + d * y);
    }
    last_cx = x2; last_cy = y2; cur_x = x; cur_y = y;
}
static void quad_to(const Mat *m, float x1, float y1, float x, float y) {
    ensure_sub(m);
    float x0 = cur_x, y0 = cur_y;
    for (int i = 1; i <= flat_n; i++) {
        float t = (float)i / flat_n, u = 1 - t;
        add_pt(m, u * u * x0 + 2 * u * t * x1 + t * t * x, u * u * y0 + 2 * u * t * y1 + t * t * y);
    }
    last_cx = x1; last_cy = y1; cur_x = x; cur_y = y;
}
static void arc_to(const Mat *m, float x1, float y1, float rx, float ry, float rot, int fa, int fs, float x2, float y2) {
    if (x1 == x2 && y1 == y2) return;
    rx = k_fabs(rx); ry = k_fabs(ry);
    if (rx < 1e-4f || ry < 1e-4f) { line_to(m, x2, y2); return; }
    ensure_sub(m);
    float phi = rot * 3.14159265f / 180.0f, cp = k_cos(phi), sp = k_sin(phi);
    float dx2 = (x1 - x2) / 2, dy2 = (y1 - y2) / 2;
    float x1p = cp * dx2 + sp * dy2, y1p = -sp * dx2 + cp * dy2;
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { float s = k_sqrt(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float coef = den > 0 && num > 0 ? k_sqrt(num / den) : 0;
    if (fa == fs) coef = -coef;
    float cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
    float cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    float th1 = k_atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    float dth = k_atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx) - th1;
    if (!fs && dth > 0) dth -= 6.2831853f;
    if (fs && dth < 0) dth += 6.2831853f;
    int n = (int)(k_fabs(dth) / 0.2f) + flat_n / 2 + 2; if (n > 90) n = 90;
    for (int i = 1; i <= n; i++) {
        float t = th1 + dth * i / n, ct = k_cos(t), st = k_sin(t);
        add_pt(m, cp * rx * ct - sp * ry * st + cx, sp * rx * ct + cp * ry * st + cy);
    }
    cur_x = x2; cur_y = y2;
}

static void parse_path(const Mat *m, const char *d) {
    char cmd = 0; const char *p = d;
    cur_x = cur_y = start_x = start_y = last_cx = last_cy = 0; need_new = 0;
    while (*p) {
        while (*p && is_ws(*p)) p++;
        if (!*p) break;
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; p++; if (c == 'z' || c == 'Z') {
                if (nsp && !need_new) { add_pt(m, start_x, start_y); cur_x = start_x; cur_y = start_y; need_new = 1; } cur_cmd_curve = 0; continue; } }
        else if (!cmd) break;
        else if (!has_num(p)) { p++; continue; }
        int rel = cmd >= 'a'; char C = cmd & ~32;
        float ox = rel ? cur_x : 0, oy = rel ? cur_y : 0, a, b, e, f, g, h;
        switch (C) {
            case 'M': a = parse_num(&p); b = parse_num(&p); move_to(m, a + ox, b + oy); cmd = rel ? 'l' : 'L'; cur_cmd_curve = 0; break;
            case 'L': a = parse_num(&p); b = parse_num(&p); line_to(m, a + ox, b + oy); cur_cmd_curve = 0; break;
            case 'H': a = parse_num(&p); line_to(m, a + ox, cur_y); cur_cmd_curve = 0; break;
            case 'V': a = parse_num(&p); line_to(m, cur_x, a + oy); cur_cmd_curve = 0; break;
            case 'C': a = parse_num(&p); b = parse_num(&p); e = parse_num(&p); f = parse_num(&p); g = parse_num(&p); h = parse_num(&p);
                cubic_to(m, a + ox, b + oy, e + ox, f + oy, g + ox, h + oy); cur_cmd_curve = 1; break;
            case 'S': { e = parse_num(&p); f = parse_num(&p); g = parse_num(&p); h = parse_num(&p);
                float x1 = cur_cmd_curve == 1 ? 2 * cur_x - last_cx : cur_x, y1 = cur_cmd_curve == 1 ? 2 * cur_y - last_cy : cur_y;
                cubic_to(m, x1, y1, e + ox, f + oy, g + ox, h + oy); cur_cmd_curve = 1; break; }
            case 'Q': a = parse_num(&p); b = parse_num(&p); e = parse_num(&p); f = parse_num(&p);
                quad_to(m, a + ox, b + oy, e + ox, f + oy); cur_cmd_curve = 2; break;
            case 'T': { e = parse_num(&p); f = parse_num(&p);
                float x1 = cur_cmd_curve == 2 ? 2 * cur_x - last_cx : cur_x, y1 = cur_cmd_curve == 2 ? 2 * cur_y - last_cy : cur_y;
                quad_to(m, x1, y1, e + ox, f + oy); cur_cmd_curve = 2; break; }
            case 'A': { a = parse_num(&p); b = parse_num(&p); e = parse_num(&p);
                while (*p && is_ws(*p)) p++; int fa = *p == '1'; p++;
                while (*p && is_ws(*p)) p++; int fs = *p == '1'; p++;
                f = parse_num(&p); g = parse_num(&p);
                arc_to(m, cur_x, cur_y, a, b, e, fa, fs, f + ox, g + oy); cur_cmd_curve = 0; break; }
            default: p++; break;
        }
    }
}

static void poly_points(const Mat *m, const char *s, int close_it) {
    int first = 1; const char *p = s;
    while (has_num(p)) {
        float x = parse_num(&p), y = parse_num(&p);
        if (first) { move_to(m, x, y); first = 0; } else line_to(m, x, y);
    }
    if (close_it && !first) line_to(m, start_x, start_y);
}
static void ellipse_path(const Mat *m, float cx, float cy, float rx, float ry) {
    int n = 20 + g_size / 6; if (n > 90) n = 90;
    for (int i = 0; i <= n; i++) {
        float t = 6.2831853f * i / n, x = cx + rx * k_cos(t), y = cy + ry * k_sin(t);
        if (!i) move_to(m, x, y); else add_pt(m, x, y);
    }
}
static void rect_path(const Mat *m, float x, float y, float w, float h, float rx, float ry) {
    if (rx <= 0 && ry > 0) rx = ry;
    if (ry <= 0 && rx > 0) ry = rx;
    if (rx > w / 2) rx = w / 2;
    if (ry > h / 2) ry = h / 2;
    if (rx <= 0 || ry <= 0) { move_to(m, x, y); line_to(m, x + w, y); line_to(m, x + w, y + h); line_to(m, x, y + h); line_to(m, x, y); return; }
    move_to(m, x + rx, y); line_to(m, x + w - rx, y);
    arc_to(m, x + w - rx, y, rx, ry, 0, 0, 1, x + w, y + ry); line_to(m, x + w, y + h - ry);
    arc_to(m, x + w, y + h - ry, rx, ry, 0, 0, 1, x + w - rx, y + h); line_to(m, x + rx, y + h);
    arc_to(m, x + rx, y + h, rx, ry, 0, 0, 1, x, y + h - ry); line_to(m, x, y + ry);
    arc_to(m, x, y + ry, rx, ry, 0, 0, 1, x + rx, y);
}

/* ---------------------------- rasterizer (signed-area accumulation) ---------------------------- */
static int aw, ah, ax0, ay0;
static void acc_line(float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1;
    if (y0 > y1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    float dxdy = (x1 - x0) / (y1 - y0), x = x0;
    int ys = y0 < 0 ? 0 : (int)y0;
    if (y0 < 0) x -= y0 * dxdy;
    int ye = (int)k_ceil(y1); if (ye > ah) ye = ah;
    for (int y = ys; y < ye; y++) {
        float *line = acc + y * (aw + 2);
        float yt = (float)y > y0 ? (float)y : y0, yb = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = yb - yt, xn = x + dxdy * dy, d = dy * dir;
        float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        if (xa < 0) xa = 0; if (xb < 0) xb = 0;
        if (xa > aw) xa = (float)aw; if (xb > aw) xb = (float)aw;
        float x0f = k_floor(xa); int x0i = (int)x0f; float x1c = k_ceil(xb); int x1i = (int)x1c;
        if (x1i <= x0i + 1) {
            float xmf = 0.5f * (xa + xb) - x0f;
            line[x0i] += d - d * xmf; line[x0i + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa), x0fr = xa - x0f;
            float a0 = 0.5f * s * (1 - x0fr) * (1 - x0fr);
            float x1fr = xb - x1c + 1, am = 0.5f * s * x1fr * x1fr;
            line[x0i] += d * a0;
            if (x1i == x0i + 2) line[x0i + 1] += d * (1 - a0 - am);
            else {
                float a1 = s * (1.5f - x0fr);
                line[x0i + 1] += d * (a1 - a0);
                for (int xi = x0i + 2; xi < x1i - 1; xi++) line[xi] += d * s;
                float a2 = a1 + (x1i - x0i - 3) * s;
                line[x1i - 1] += d * (1 - a2 - am);
            }
            line[x1i] += d * am;
        }
        x = xn;
    }
}
static void acc_poly(const float *xs, const float *ys, int n) {
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        acc_line(xs[i] - ax0, ys[i] - ay0, xs[j] - ax0, ys[j] - ay0);
    }
}
static void setup_bbox(float x0, float y0, float x1, float y1) {
    ax0 = (int)k_floor(x0) - 1; ay0 = (int)k_floor(y0) - 1;
    if (ax0 < -1) ax0 = -1; if (ay0 < -1) ay0 = -1;
    int ex = (int)k_ceil(x1) + 1, ey = (int)k_ceil(y1) + 1;
    if (ex > g_size) ex = g_size; if (ey > g_size) ey = g_size;
    aw = ex - ax0; ah = ey - ay0;
    if (aw > MAXSZ + 1) aw = MAXSZ + 1; if (ah > MAXSZ + 1) ah = MAXSZ + 1;
    if (aw < 1) aw = 1; if (ah < 1) ah = 1;
    memset(acc, 0, (aw + 2) * ah * sizeof(float));
}

static u32 grad_color(const Grad *g, float t) {
    if (t <= g->off[0]) return g->col[0];
    for (int i = 1; i < g->nst; i++) {
        if (t <= g->off[i]) {
            float span = g->off[i] - g->off[i - 1];
            int k = span > 1e-6f ? (int)((t - g->off[i - 1]) / span * 256) : 256;
            return col_lerp(g->col[i - 1], g->col[i], iclamp(k, 0, 256));
        }
    }
    return g->col[g->nst - 1];
}

/* convert accumulated coverage into the output, using a paint */
static void resolve(const Paint *pt, int evenodd, float opacity, const Mat *m) {
    const Grad *g = pt->type == 2 ? &grads[pt->grad] : NULL;
    float gx1 = 0, gy1 = 0, gdx = 1, gdy = 0, glen2 = 1, gcx = 0, gcy = 0, gr = 1;
    if (g) {
        float w = bx1 - bx0, h = by1 - by0;
        if (!g->radial) {
            float x1 = g->x1, y1 = g->y1, x2 = g->x2, y2 = g->y2;
            if (g->user) { xf(m, x1, y1, &x1, &y1); xf(m, x2, y2, &x2, &y2); }
            else { x1 = bx0 + x1 * w; y1 = by0 + y1 * h; x2 = bx0 + x2 * w; y2 = by0 + y2 * h; }
            gx1 = x1; gy1 = y1; gdx = x2 - x1; gdy = y2 - y1; glen2 = gdx * gdx + gdy * gdy; if (glen2 < 1e-6f) glen2 = 1;
        } else {
            if (g->user) { xf(m, g->cx, g->cy, &gcx, &gcy); gr = g->r * k_sqrt(k_fabs(m->a * m->d - m->b * m->c)); }
            else { gcx = bx0 + g->cx * w; gcy = by0 + g->cy * h; gr = g->r * (w > h ? w : h); }
            if (gr < 1e-3f) gr = 1;
        }
    }
    int op256 = (int)(opacity * 256);
    for (int y = 0; y < ah; y++) {
        int oy = y + ay0; if (oy < 0 || oy >= g_size) continue;
        float sum = 0; float *line = acc + y * (aw + 2);
        for (int x = 0; x < aw; x++) {
            sum += line[x];
            float c = k_fabs(sum);
            if (evenodd) { c = c - 2 * k_floor(c / 2); if (c > 1) c = 2 - c; } else if (c > 1) c = 1;
            int cv = (int)(c * 256);
            if (cv <= 0) continue;
            int ox = x + ax0; if (ox < 0 || ox >= g_size) continue;
            u32 col = pt->col;
            if (g) {
                float t;
                if (!g->radial) t = ((ox + 0.5f - gx1) * gdx + (oy + 0.5f - gy1) * gdy) / glen2;
                else { float dx = ox + 0.5f - gcx, dy = oy + 0.5f - gcy; t = k_sqrt(dx * dx + dy * dy) / gr; }
                col = grad_color(g, t < 0 ? 0 : (t > 1 ? 1 : t));
            }
            int a = (int)(col >> 24); a = (a * cv) >> 8; a = (a * op256) >> 8;
            if (a <= 0) continue;
            if (a > 255) a = 255;
            u32 r = (col >> 16) & 255, gg = (col >> 8) & 255, b = col & 255;
            r = (r * a + 127) / 255; gg = (gg * a + 127) / 255; b = (b * a + 127) / 255;
            u32 s = ((u32)a << 24) | (r << 16) | (gg << 8) | b;
            u32 *d = &g_out[oy * g_size + ox];
            u32 ia = 255 - a, dr = (*d >> 16) & 255, dg = (*d >> 8) & 255, db = *d & 255, da = *d >> 24;
            *d = ((a + (da * ia + 127) / 255) << 24) | ((r + (dr * ia + 127) / 255) << 16) |
                 ((gg + (dg * ia + 127) / 255) << 8) | (b + (db * ia + 127) / 255);
            (void)s;
        }
    }
}

static void paint_fill(const Style *st) {
    if (npt < 3 || st->fill.type == 0) return;
    bx0 = by0 = 1e9f; bx1 = by1 = -1e9f;
    for (int i = 0; i < npt; i++) {
        if (ptx[i] < bx0) bx0 = ptx[i]; if (ptx[i] > bx1) bx1 = ptx[i];
        if (pty[i] < by0) by0 = pty[i]; if (pty[i] > by1) by1 = pty[i];
    }
    if (bx1 < 0 || by1 < 0 || bx0 > g_size || by0 > g_size) return;
    setup_bbox(bx0, by0, bx1, by1);
    for (int s = 0; s < nsp; s++) {
        int a = sp_start[s], b = sp_start[s + 1];
        if (b - a >= 2) acc_poly(ptx + a, pty + a, b - a);
    }
    resolve(&st->fill, st->evenodd, st->op * st->fop, &st->m);
}

static void paint_stroke(const Style *st) {
    if (st->stroke.type == 0 || st->sw <= 0 || npt < 2) return;
    float scale = k_sqrt(k_fabs(st->m.a * st->m.d - st->m.b * st->m.c));
    float hw = st->sw * scale * 0.5f; if (hw < 0.35f) hw = 0.35f;
    bx0 = by0 = 1e9f; bx1 = by1 = -1e9f;
    for (int i = 0; i < npt; i++) {
        if (ptx[i] < bx0) bx0 = ptx[i]; if (ptx[i] > bx1) bx1 = ptx[i];
        if (pty[i] < by0) by0 = pty[i]; if (pty[i] > by1) by1 = pty[i];
    }
    float gx0 = bx0, gy0 = by0, gx1 = bx1, gy1 = by1;
    setup_bbox(bx0 - hw, by0 - hw, bx1 + hw, by1 + hw);
    int csegs = 8 + (int)(hw * 2); if (csegs > 28) csegs = 28;
    float cxs[32], cys[32];
    for (int s = 0; s < nsp; s++) {
        int a = sp_start[s], b = sp_start[s + 1];
        if (b - a < 1) continue;
        int closed = (b - a > 2) && (k_fabs(ptx[a] - ptx[b - 1]) < 0.01f && k_fabs(pty[a] - pty[b - 1]) < 0.01f);
        int nseg = b - a - 1;
        float pdx = 0, pdy = 0; int have_prev = 0;
        for (int i = 0; i <= nseg; i++) {
            int i0 = a + i, i1 = a + i + 1;
            float dx = 0, dy = 0;
            if (i < nseg) {
                dx = ptx[i1] - ptx[i0]; dy = pty[i1] - pty[i0];
                float len = k_sqrt(dx * dx + dy * dy);
                if (len > 1e-4f) {
                    dx /= len; dy /= len;
                    float nx = -dy * hw, ny = dx * hw;
                    float qx[4] = { ptx[i0] + nx, ptx[i1] + nx, ptx[i1] - nx, ptx[i0] - nx };
                    float qy[4] = { pty[i0] + ny, pty[i1] + ny, pty[i1] - ny, pty[i0] - ny };
                    float ar = 0;
                    for (int k = 0; k < 4; k++) { int j = (k + 1) & 3; ar += qx[k] * qy[j] - qx[j] * qy[k]; }
                    if (ar < 0) { float t; t = qx[0]; qx[0] = qx[3]; qx[3] = t; t = qy[0]; qy[0] = qy[3]; qy[3] = t;
                                  t = qx[1]; qx[1] = qx[2]; qx[2] = t; t = qy[1]; qy[1] = qy[2]; qy[2] = t; }
                    acc_poly(qx, qy, 4);
                } else { dx = pdx; dy = pdy; }
            }
            /* round join / cap at vertex i0 */
            int need = 0;
            if (i == 0 || i == nseg) need = closed ? (i == 0) : st->cap_round;
            else need = (dx * pdx + dy * pdy) < 0.985f;
            if (need && hw > 0.6f) {
                for (int k = 0; k < csegs; k++) {
                    float t = 6.2831853f * k / csegs;
                    cxs[k] = ptx[i0] + hw * k_cos(t); cys[k] = pty[i0] + hw * k_sin(t);
                }
                acc_poly(cxs, cys, csegs);
            }
            pdx = dx; pdy = dy; have_prev = 1;
        }
        (void)have_prev;
    }
    bx0 = gx0; by0 = gy0; bx1 = gx1; by1 = gy1;
    resolve(&st->stroke, 0, st->op * st->sop, &st->m);
}

/* ---------------------------- document walk ---------------------------- */
static void read_paint(const char *t, int n, const char *name, Paint *p, u32 tint) {
    char b[96];
    if (!attr(t, n, name, b, sizeof b)) return;
    if (starts_with(b, "url(")) {
        const char *q = b + 4; if (*q == '#') q++;
        char id[24]; int k = 0;
        while (*q && *q != ')' && *q != '\'' && *q != '"' && k < 23) id[k++] = *q++;
        id[k] = 0;
        for (int i = 0; i < ngrad; i++) if (!strcmp(grads[i].id, id)) { p->type = 2; p->grad = i; p->col = 0xFF000000; return; }
        p->type = 0; return;
    }
    u32 c;
    if (parse_color(b, tint, &c)) { p->type = 1; p->col = c; } else p->type = 0;
}
static void read_style(const char *t, int n, Style *st, u32 tint) {
    char b[160];
    if (attr(t, n, "transform", b, sizeof b)) st->m = mat_mul(st->m, parse_transform(b));
    read_paint(t, n, "fill", &st->fill, tint);
    read_paint(t, n, "stroke", &st->stroke, tint);
    st->sw = fattr(t, n, "stroke-width", st->sw);
    float o = fattr(t, n, "opacity", 1.0f); st->op *= o;
    st->fop = fattr(t, n, "fill-opacity", st->fop);
    st->sop = fattr(t, n, "stroke-opacity", st->sop);
    if (attr(t, n, "fill-rule", b, sizeof b)) st->evenodd = starts_with(b, "evenodd");
    if (attr(t, n, "stroke-linecap", b, sizeof b)) st->cap_round = !starts_with(b, "butt");
}

static int name_is(const char *t, const char *nm) {   /* t points just after '<' */
    int l = (int)strlen(nm);
    return memcmp(t, nm, l) == 0 && (is_ws(t[l]) || t[l] == '>' || t[l] == '/');
}

int svg_render(const char *svg, int len, u32 *out, int size, u32 tint) {
    if (size > MAXSZ) size = MAXSZ;
    g_out = out; g_size = size; g_tint = tint; ngrad = 0;
    memset(out, 0, size * size * 4);
    flat_n = 8 + size / 14;
    Style stack[10]; int sd = 0;
    Style base = { {1, 0, 0, 1, 0, 0}, {1, 0xFF000000, 0}, {0, 0, 0}, 1.0f, 1.0f, 1.0f, 1.0f, 0, 0 };
    stack[0] = base;
    float vbx = 0, vby = 0, vbw = 24, vbh = 24;
    int seen_svg = 0, in_defs = 0, skip = 0; char skip_name[16] = {0};
    Grad *cg = NULL;
    const char *p = svg, *end = svg + len;
    while (p < end) {
        if (*p != '<') { p++; continue; }
        if (starts_with(p, "<!--")) { while (p < end && !starts_with(p, "-->")) p++; p += 3; continue; }
        if (p[1] == '?' || p[1] == '!') { while (p < end && *p != '>') p++; p++; continue; }
        const char *t = p + 1;
        const char *q = t; char quote = 0;
        while (q < end && (quote || *q != '>')) { if (quote) { if (*q == quote) quote = 0; } else if (*q == '"' || *q == '\'') quote = *q; q++; }
        int n = (int)(q - t);
        int selfclose = n > 0 && t[n - 1] == '/';
        p = q + 1;
        if (*t == '/') {                                    /* closing tag */
            const char *nm = t + 1;
            if (skip) { if (name_is(nm, skip_name)) skip = 0; continue; }
            if (name_is(nm, "g") && sd > 0) sd--;
            else if (name_is(nm, "defs")) in_defs = 0;
            else if (name_is(nm, "linearGradient") || name_is(nm, "radialGradient")) cg = NULL;
            continue;
        }
        if (skip) continue;
        if (name_is(t, "svg") && !seen_svg) {
            seen_svg = 1;
            char b[96];
            float w = fattr(t, n, "width", 0), h = fattr(t, n, "height", 0);
            if (attr(t, n, "viewBox", b, sizeof b)) {
                const char *q2 = b; vbx = parse_num(&q2); vby = parse_num(&q2); vbw = parse_num(&q2); vbh = parse_num(&q2);
            } else if (w > 0 && h > 0) { vbw = w; vbh = h; }
            if (vbw <= 0) vbw = 24;
            if (vbh <= 0) vbh = 24;
            float s = (float)size / (vbw > vbh ? vbw : vbh);
            stack[0].m = (Mat){ s, 0, 0, s, -vbx * s + (size - vbw * s) / 2, -vby * s + (size - vbh * s) / 2 };
            read_style(t, n, &stack[0], tint);
            continue;
        }
        if (name_is(t, "defs")) { in_defs = selfclose ? 0 : 1; continue; }
        if (name_is(t, "linearGradient") || name_is(t, "radialGradient")) {
            if (ngrad < MAXGR) {
                cg = &grads[ngrad++]; memset(cg, 0, sizeof *cg);
                cg->radial = t[0] == 'r';
                char b[64];
                if (attr(t, n, "id", b, sizeof b)) kstrcpy(cg->id, b, sizeof cg->id);
                cg->user = attr(t, n, "gradientUnits", b, sizeof b) && starts_with(b, "userSpaceOnUse");
                cg->x1 = fattr(t, n, "x1", 0); cg->y1 = fattr(t, n, "y1", 0);
                cg->x2 = fattr(t, n, "x2", 1); cg->y2 = fattr(t, n, "y2", 0);
                cg->cx = fattr(t, n, "cx", 0.5f); cg->cy = fattr(t, n, "cy", 0.5f); cg->r = fattr(t, n, "r", 0.5f);
                if (selfclose) cg = NULL;
            }
            continue;
        }
        if (name_is(t, "stop")) {
            if (cg && cg->nst < MAXST) {
                char b[64]; u32 c = 0xFF000000; float off = fattr(t, n, "offset", 0);
                if (attr(t, n, "stop-color", b, sizeof b)) parse_color(b, tint, &c);
                float so = fattr(t, n, "stop-opacity", 1.0f);
                c = col_alpha(c, (int)(so * 255));
                if (cg->nst && off < cg->off[cg->nst - 1]) off = cg->off[cg->nst - 1];
                cg->off[cg->nst] = off; cg->col[cg->nst] = c; cg->nst++;
            }
            continue;
        }
        if (name_is(t, "clipPath") || name_is(t, "mask") || name_is(t, "pattern") || name_is(t, "filter") ||
            name_is(t, "style") || name_is(t, "title") || name_is(t, "desc") || name_is(t, "symbol") || name_is(t, "metadata")) {
            if (!selfclose) {
                const char *nm = t; int k = 0; while (!is_ws(nm[k]) && nm[k] != '>' && k < 15) { skip_name[k] = nm[k]; k++; }
                skip_name[k] = 0; skip = 1;
            }
            continue;
        }
        if (name_is(t, "g")) {
            if (!selfclose && sd < 9) { stack[sd + 1] = stack[sd]; sd++; read_style(t, n, &stack[sd], tint); }
            continue;
        }
        if (in_defs) continue;
        Style st = stack[sd]; char b[2400];
        int shape = 1; nsp = 0; npt = 0; sp_start[0] = 0;
        read_style(t, n, &st, tint);
        if (name_is(t, "path")) { if (attr(t, n, "d", b, sizeof b)) parse_path(&st.m, b); }
        else if (name_is(t, "rect")) rect_path(&st.m, fattr(t, n, "x", 0), fattr(t, n, "y", 0), fattr(t, n, "width", 0), fattr(t, n, "height", 0), fattr(t, n, "rx", 0), fattr(t, n, "ry", 0));
        else if (name_is(t, "circle")) { float r = fattr(t, n, "r", 0); ellipse_path(&st.m, fattr(t, n, "cx", 0), fattr(t, n, "cy", 0), r, r); }
        else if (name_is(t, "ellipse")) ellipse_path(&st.m, fattr(t, n, "cx", 0), fattr(t, n, "cy", 0), fattr(t, n, "rx", 0), fattr(t, n, "ry", 0));
        else if (name_is(t, "line")) { move_to(&st.m, fattr(t, n, "x1", 0), fattr(t, n, "y1", 0)); line_to(&st.m, fattr(t, n, "x2", 0), fattr(t, n, "y2", 0)); st.fill.type = 0; }
        else if (name_is(t, "polygon") || name_is(t, "polyline")) {
            if (attr(t, n, "points", b, sizeof b)) poly_points(&st.m, b, name_is(t, "polygon"));
        } else shape = 0;
        if (shape && npt > 0) { paint_fill(&st); paint_stroke(&st); }
    }
    return seen_svg;
}
