#include "ktuyome.h"

/* ============================== icon artwork (plain SVG, stored in /system/icons) ============================== */
const char *icon_names[IC_COUNT] = { "notes", "editor", "calculator", "files", "settings", "about", "restart", "power", "taskmgr", "network", "browser", "discord", "zip", "viewer", "paint" };
const char *icon_svgs[IC_COUNT] = {
    "<svg viewBox='0 0 24 24'><path d='M6 2.8h8.6L20 8.2V19a2.2 2.2 0 0 1-2.2 2.2H6A2.2 2.2 0 0 1 3.8 19V5A2.2 2.2 0 0 1 6 2.8z' fill='#fff'/>"
    "<path d='M14.4 2.8V7a1.2 1.2 0 0 0 1.2 1.2H20z' fill='#FFC8B5'/>"
    "<path d='M7.5 12h9M7.5 15.2h9M7.5 18.4h5.5' stroke='#FF7043' stroke-width='1.6' stroke-linecap='round' fill='none'/></svg>",

    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.1' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='M8.5 6.5L3 12l5.5 5.5'/><path d='M15.5 6.5L21 12l-5.5 5.5'/><path d='M13.6 4.5l-3.2 15'/></svg>",

    "<svg viewBox='0 0 24 24'><rect x='4.2' y='2.4' width='15.6' height='19.2' rx='3' fill='#fff'/>"
    "<rect x='6.6' y='5' width='10.8' height='4.6' rx='1.2' fill='#00897B'/>"
    "<g fill='#00897B'><circle cx='8.4' cy='13' r='1.25'/><circle cx='12' cy='13' r='1.25'/><circle cx='15.6' cy='13' r='1.25'/>"
    "<circle cx='8.4' cy='16.2' r='1.25'/><circle cx='12' cy='16.2' r='1.25'/><circle cx='15.6' cy='16.2' r='1.25'/>"
    "<circle cx='8.4' cy='19.4' r='1.25'/><circle cx='12' cy='19.4' r='1.25'/><circle cx='15.6' cy='19.4' r='1.25'/></g></svg>",

    "<svg viewBox='0 0 24 24'><path d='M2.8 6.2A2.2 2.2 0 0 1 5 4h4.4c.6 0 1.1.25 1.5.65L12.2 6H19a2.2 2.2 0 0 1 2.2 2.2v9.6A2.2 2.2 0 0 1 19 20H5a2.2 2.2 0 0 1-2.2-2.2z' fill='#fff'/>"
    "<path d='M2.8 9.5h18.4' stroke='#7C4DFF' stroke-opacity='.25' stroke-width='1.2' fill='none'/></svg>",

    "<svg viewBox='0 0 24 24'><g fill='#fff'><circle cx='12' cy='12' r='7'/>"
    "<rect id='t' x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(45 12 12)'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(90 12 12)'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(135 12 12)'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(180 12 12)'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(225 12 12)'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(270 12 12)'/>"
    "<rect x='10.3' y='1.8' width='3.4' height='4.4' rx='1.1' transform='rotate(315 12 12)'/></g>"
    "<circle cx='12' cy='12' r='3' fill='#546E7A'/></svg>",

    "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='9.6' fill='#fff'/><circle cx='12' cy='7.8' r='1.4' fill='#0288D1'/>"
    "<rect x='10.8' y='10.6' width='2.4' height='6.8' rx='1.2' fill='#0288D1'/></svg>",

    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='M19.6 12a7.6 7.6 0 1 1-2.4-5.5'/><path d='M19.8 3.6v4.4h-4.4'/></svg>",

    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.3' stroke-linecap='round'>"
    "<path d='M7.4 6.6a8 8 0 1 0 9.2 0'/><path d='M12 2.8v8.4'/></svg>",

    "<svg viewBox='0 0 24 24'><rect x='2.6' y='3.4' width='18.8' height='13.6' rx='2.8' fill='#fff'/>"
    "<path d='M5.2 11.4h3.2l1.8-4 3 7.2 1.9-4.4h3.7' stroke='#D81B60' stroke-width='1.8' fill='none' stroke-linecap='round' stroke-linejoin='round'/>"
    "<path d='M8.6 20.6h6.8M12 17v3.6' stroke='#fff' stroke-width='2.1' stroke-linecap='round' fill='none'/></svg>",

    "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='9.6' fill='#fff'/>"
    "<g fill='none' stroke='#3949AB' stroke-width='1.6' stroke-linecap='round'><ellipse cx='12' cy='12' rx='4' ry='9.6'/>"
    "<path d='M2.4 12h19.2M3.9 7.2h16.2M3.9 16.8h16.2'/></g></svg>",

    /* IC_BROWSER — globe with a magnifying glass overlay */
    "<svg viewBox='0 0 24 24'>"
    "<circle cx='11' cy='11' r='8.2' fill='#fff'/>"
    "<g fill='none' stroke='#1565C0' stroke-width='1.5' stroke-linecap='round'>"
    "<ellipse cx='11' cy='11' rx='3.4' ry='8.2'/>"
    "<path d='M2.8 11h16.4M4.1 7h13.8M4.1 15h13.8'/>"
    "</g>"
    "<circle cx='17.5' cy='17.5' r='4' fill='#1565C0'/>"
    "<path d='M16 16l3 3' stroke='#fff' stroke-width='1.6' stroke-linecap='round'/>"
    "<circle cx='17' cy='17' r='2' fill='none' stroke='#fff' stroke-width='1.4'/>"
    "</svg>",

    /* IC_DISCORD — Discord-style chat bubble with controller shape */
    "<svg viewBox='0 0 24 24'>"
    "<path d='M5 4.5C5 3.7 5.7 3 6.5 3h11c0.8 0 1.5 0.7 1.5 1.5v11c0 0.8-0.7 1.5-1.5 1.5H9l-4 3.5V4.5z' fill='#5865F2'/>"
    "<g fill='#fff'><circle cx='9' cy='9' r='1.4'/><circle cx='15' cy='9' r='1.4'/></g>"
    "</svg>",

    /* IC_ZIP */
    "<svg viewBox='0 0 24 24'>"
    "<path d='M2.8 6.2A2.2 2.2 0 0 1 5 4h4.4c.6 0 1.1.25 1.5.65L12.2 6H19a2.2 2.2 0 0 1 2.2 2.2v9.6A2.2 2.2 0 0 1 19 20H5a2.2 2.2 0 0 1-2.2-2.2z' fill='#fff'/>"
    "<path d='M12 8v10' stroke='#7C4DFF' stroke-width='1.6' stroke-linecap='round' fill='none'/>"
    "<circle cx='12' cy='9' r='1.2' fill='#7C4DFF'/><circle cx='12' cy='12' r='1.2' fill='#7C4DFF'/><circle cx='12' cy='15' r='1.2' fill='#7C4DFF'/></svg>",

    /* IC_VIEWER */
    "<svg viewBox='0 0 24 24'><rect x='3' y='4' width='18' height='14' rx='2' fill='#fff'/><circle cx='9' cy='9' r='2' fill='#1565C0'/><path d='M3 15l5-4 4 3 5-5 5 4v3H3z' fill='#1565C0'/></svg>",

    /* IC_PAINT */
    "<svg viewBox='0 0 24 24'><path d='M12 3C7 3 3 7 3 12s4 9 9 9 9-4 9-9-4-9-9-9z' fill='#fff'/><circle cx='8' cy='9' r='1.6' fill='#FF7043'/><circle cx='12' cy='7' r='1.6' fill='#4FC3F7'/><circle cx='16' cy='9' r='1.6' fill='#4DD0A1'/><path d='M12 12l6 6' stroke='#7C4DFF' stroke-width='2' stroke-linecap='round'/></svg>",
};
const char *sample_svg =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 200 120'>\n"
    "  <defs>\n"
    "    <linearGradient id='sky' x1='0' y1='0' x2='0' y2='1'>\n"
    "      <stop offset='0' stop-color='#1b2a6b'/><stop offset='1' stop-color='#ff8a5c'/>\n"
    "    </linearGradient>\n"
    "    <radialGradient id='sun' cx='0.5' cy='0.5' r='0.5'>\n"
    "      <stop offset='0' stop-color='#fff6c9'/><stop offset='1' stop-color='#ffb347'/>\n"
    "    </radialGradient>\n"
    "  </defs>\n"
    "  <rect width='200' height='120' rx='10' fill='url(#sky)'/>\n"
    "  <circle cx='140' cy='62' r='24' fill='url(#sun)'/>\n"
    "  <path d='M0 120V84l36-28 28 22 30-36 36 40 28-20 42 30V120z' fill='#241a3d'/>\n"
    "  <path d='M0 120V100l40-18 40 16 50-22 70 24V120z' fill='#120d24'/>\n"
    "  <g fill='#fff' fill-opacity='.8'><circle cx='30' cy='20' r='1.2'/><circle cx='64' cy='12' r='1'/>\n"
    "  <circle cx='96' cy='26' r='1.3'/><circle cx='170' cy='16' r='1'/><circle cx='20' cy='44' r='1'/></g>\n"
    "</svg>\n";

/* ============================== forward declarations of app entry points ============================== */
void files_open(void); void files_close(void); int files_key(const Event *e); void files_draw(int x, int y, int w, int h);
void files_dialog(int action, int arg);
void set_open(void); void set_close(void); int set_key(const Event *e); void set_draw(int x, int y, int w, int h);
void set_tick(void);
void zip_open(void); void zip_close(void); int zip_key(const Event *e); void zip_draw(int x, int y, int w, int h);
void viewer_open(void); void viewer_close(void); int viewer_key(const Event *e); void viewer_draw(int x, int y, int w, int h);
void paint_open(void); void paint_close(void); int paint_key(const Event *e); void paint_draw(int x, int y, int w, int h);
/* ============================== multi-line text widget ============================== */
void tb_init(TextBuf *t, char *buf, int cap, int font) { memset(t, 0, sizeof *t); t->buf = buf; t->cap = cap; t->font = font; t->sel = -1; buf[0] = 0; }
void tb_set(TextBuf *t, const char *s, int len) {
    if (len > t->cap - 1) len = t->cap - 1;
    memcpy(t->buf, s, len); t->buf[len] = 0;
    t->len = len; t->cur = 0; t->scrolly = t->scrollx = 0; t->dirty = 0; t->follow = 1; t->sel = -1; t->drag = 0;
}
static int ls_(TextBuf *t, int i) { while (i > 0 && t->buf[i - 1] != '\n') i--; return i; }
static int le_(TextBuf *t, int i) { while (i < t->len && t->buf[i] != '\n') i++; return i; }
int tb_line(TextBuf *t) { int n = 0; for (int i = 0; i < t->cur; i++) if (t->buf[i] == '\n') n++; return n; }
int tb_col(TextBuf *t) { return t->cur - ls_(t, t->cur); }
int tb_lines(TextBuf *t) { int n = 1; for (int i = 0; i < t->len; i++) if (t->buf[i] == '\n') n++; return n; }
static int tb_ins(TextBuf *t, const char *s, int n) {
    if (t->len + n >= t->cap) return 0;
    memmove(t->buf + t->cur + n, t->buf + t->cur, t->len - t->cur + 1);
    memcpy(t->buf + t->cur, s, n); t->len += n; t->cur += n; t->dirty = 1; t->last_edit = (int)ticks();
    return 1;
}
static void tb_del(TextBuf *t, int from, int n) {
    memmove(t->buf + from, t->buf + from + n, t->len - from - n + 1);
    t->len -= n; t->dirty = 1; t->last_edit = (int)ticks();
}
static void move_vert(TextBuf *t, int dir) {
    int s = ls_(t, t->cur), col = t->cur - s;
    if (dir < 0) { if (s == 0) { t->cur = 0; return; } int ps = ls_(t, s - 1); t->cur = ps + imin(col, s - 1 - ps); }
    else { int e = le_(t, t->cur); if (e >= t->len) { t->cur = t->len; return; } int ns = e + 1; t->cur = ns + imin(col, le_(t, ns) - ns); }
}
/* ---- selection: the range between the anchor (sel) and the caret (cur) ---- */
static int tb_selrange(TextBuf *t, int *a, int *b) {
    if (t->sel < 0) return 0;
    int s = imin(t->sel, t->len);
    *a = imin(s, t->cur); *b = imax(s, t->cur);
    return *b > *a;
}
static int tb_delsel(TextBuf *t) {                                         /* removes the selection; 1 if there was one */
    int a, b, had = tb_selrange(t, &a, &b);
    t->sel = -1;
    if (!had) return 0;
    tb_del(t, a, b - a); t->cur = a;
    return 1;
}
static void tb_paste(TextBuf *t) {
    int n = clip_len();
    if (!n) { ui_flash("Clipboard is empty"); return; }
    tb_delsel(t);
    int take = imin(n, t->cap - 1 - t->len);
    if (take <= 0 || !tb_ins(t, clip_text(), take)) { ui_flash("No room left to paste"); return; }
    if (take < n) ui_flash("Pasted (cut off: no room for the rest)"); else clip_notify("Pasted", take);
}
static int is_word(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int word_left(TextBuf *t, int i) { while (i > 0 && !is_word(t->buf[i - 1])) i--; while (i > 0 && is_word(t->buf[i - 1])) i--; return i; }
static int word_right(TextBuf *t, int i) { while (i < t->len && !is_word(t->buf[i])) i++; while (i < t->len && is_word(t->buf[i])) i++; return i; }
static void tb_select_word(TextBuf *t, int pos) {                          /* double-click: the run of same-kind characters under pos */
    if (t->len == 0) { t->sel = -1; return; }
    int i = imin(pos, t->len - 1);
    if (t->buf[i] == '\n' && i > 0) i--;                                   /* clicked past the end of a line */
    char c = t->buf[i];
    int kind = c == '\n' ? 0 : is_word(c) ? 1 : c == ' ' ? 2 : 3, a = i, b = i + 1;
#define KIND(ch) ((ch) == '\n' ? 0 : is_word(ch) ? 1 : (ch) == ' ' ? 2 : 3)
    while (a > 0 && KIND(t->buf[a - 1]) == kind) a--;
    while (b < t->len && KIND(t->buf[b]) == kind) b++;
#undef KIND
    t->sel = a; t->cur = b;
}
int tb_key(TextBuf *t, const Event *e) {
    int k = e->key, shift = (e->mods & MOD_SHIFT) != 0, ctrl = (e->mods & MOD_CTRL) != 0;
    int ck = clip_key(e);
    if (ck) {
        int a, b;
        if (ck == CK_ALL) { t->sel = 0; t->cur = t->len; }
        else if (ck == CK_PASTE) tb_paste(t);
        else if (tb_selrange(t, &a, &b)) {
            clip_set(t->buf + a, b - a); clip_notify(ck == CK_CUT ? "Cut" : "Copied", clip_len());
            if (ck == CK_CUT) tb_delsel(t);
        } else ui_flash("Nothing selected");
        t->follow = 1; return 1;
    }
    if (e->mods & MOD_ALT) return 0;
    int nav = k >= K_UP && k <= K_PGDN;
    if (ctrl && !(k == K_LEFT || k == K_RIGHT || k == K_HOME || k == K_END)) return 0;
    if (nav) {                                                             /* movement; Shift extends the selection */
        int a, b, had = tb_selrange(t, &a, &b);
        if (shift) { if (t->sel < 0) t->sel = t->cur; }
        else if (had && !ctrl && (k == K_LEFT || k == K_RIGHT)) { t->cur = k == K_LEFT ? a : b; t->sel = -1; t->follow = 1; return 1; }
        else t->sel = -1;
        switch (k) {
            case K_LEFT: if (ctrl) t->cur = word_left(t, t->cur); else if (t->cur > 0) t->cur--; break;
            case K_RIGHT: if (ctrl) t->cur = word_right(t, t->cur); else if (t->cur < t->len) t->cur++; break;
            case K_UP: move_vert(t, -1); break;
            case K_DOWN: move_vert(t, 1); break;
            case K_PGUP: for (int i = 0; i < 12; i++) move_vert(t, -1); break;
            case K_PGDN: for (int i = 0; i < 12; i++) move_vert(t, 1); break;
            case K_HOME: t->cur = ctrl ? 0 : ls_(t, t->cur); break;
            case K_END: t->cur = ctrl ? t->len : le_(t, t->cur); break;
        }
        t->follow = 1; return 1;
    }
    switch (k) {
        case K_BS: if (!tb_delsel(t) && t->cur > 0) { tb_del(t, t->cur - 1, 1); t->cur--; } break;
        case K_DEL: if (!tb_delsel(t) && t->cur < t->len) tb_del(t, t->cur, 1); break;
        case K_ENTER: {
            tb_delsel(t);
            char ins[40]; int n = 1; ins[0] = '\n';
            if (t->autoindent) { int s = ls_(t, t->cur); while (n < 38 && s + n - 1 < t->cur && t->buf[s + n - 1] == ' ') { ins[n] = ' '; n++; } }
            tb_ins(t, ins, n); break; }
        case K_TAB: tb_delsel(t); tb_ins(t, "    ", 4); break;
        default:
            if (e->ch >= 32 && e->ch < 127) { char c = (char)e->ch; tb_delsel(t); tb_ins(t, &c, 1); } else return 0;
    }
    t->sel = -1;
    t->follow = 1;
    return 1;
}
/* character offset under a screen point (rows above/below the text area clamp to the nearest line) */
static int tb_hit(TextBuf *t, int mx, int my, int y, int lh, int rows, int tx, int drag) {
    int dy = my - y, row = dy >= 0 ? dy / lh : -((-dy + lh - 1) / lh);
    if (drag) row = iclamp(row, -1, rows);
    int line = imax(0, t->scrolly + row), off = 0;
    for (int l = 0; l < line && off < t->len; l++) { off = le_(t, off); if (off < t->len) off++; }
    int e = le_(t, off), c = off, px = tx - t->scrollx;
    while (c < e) { int cw = gfx_text_w(t->font, t->buf + c, 1); if (mx < px + cw / 2) break; px += cw; c++; }
    return c;
}
void tb_draw(TextBuf *t, int x, int y, int w, int h, int focus, int gutter) {
    const Font *F = &fonts[t->font];
    int lh = F->h + (t->font == F_MONO ? 3 : 7), rows = imax(1, h / lh);
    int gw = gutter ? 58 : 0, tx = x + gw + 10, tw = w - gw - 20;
    int total = tb_lines(t);
    if (g_wheel && ui_hover(x, y, w, h)) { t->scrolly += g_wheel * 3; t->follow = 0; }
    if (ui_click_in(x + gw, y, w - gw, h)) {                               /* click: caret; drag: select; double-click: word */
        int pos = tb_hit(t, g_click_x, g_click_y, y, lh, rows, tx, 0), now = (int)ticks();
        if (t->clk_n == pos && now - t->clk_t < 450 && !(g_click_mods & MOD_SHIFT)) { tb_select_word(t, pos); t->drag = 0; }
        else {
            if (g_click_mods & MOD_SHIFT) { if (t->sel < 0) t->sel = t->cur; } else t->sel = pos;
            t->cur = pos; t->drag = 1;
        }
        t->clk_t = now; t->clk_n = pos; t->follow = 1;
    } else if (t->drag) {
        if (mouse_btn & 1) { t->cur = tb_hit(t, mouse_x, mouse_y, y, lh, rows, tx, 1); t->follow = 1; ui_wake(); }
        else { t->drag = 0; if (t->sel == t->cur) t->sel = -1; }
    }
    int sa = 0, sb = 0, hasel = tb_selrange(t, &sa, &sb);
    int line = tb_line(t), cs = ls_(t, t->cur);
    int curpx = gfx_text_w(t->font, t->buf + cs, t->cur - cs);
    if (t->follow) {
        if (line < t->scrolly) t->scrolly = line;
        if (line >= t->scrolly + rows) t->scrolly = line - rows + 1;
        if (curpx - t->scrollx > tw - 24) t->scrollx = curpx - tw + 24;
        if (curpx < t->scrollx) t->scrollx = imax(0, curpx - 24);
        t->follow = 0;
    }
    t->scrolly = iclamp(t->scrolly, 0, imax(0, total - 1));
    int off = 0;
    for (int l = 0; l < t->scrolly && off < t->len; l++) { off = le_(t, off); if (off < t->len) off++; }
    for (int r = 0, ln = t->scrolly; r < rows; r++, ln++) {
        int e = le_(t, off), ly = y + r * lh;
        if (ln == line && focus) gfx_rrect(x + gw, ly - 1, w - gw, lh, 6, WHITE_A(14), WHITE_A(14), 0);
        if (gutter) {
            char nb[12]; kitoa(nb, ln + 1);
            gfx_text_r(F_MONO, x + gw - 12, ly + 1, nb, ln == line ? WHITE_A(220) : WHITE_A(80));
        }
        gfx_clip(x + gw, y, w - gw, h);
        if (hasel && sb > off && sa <= e) {                               /* selection highlight (a selected newline gets a short tail) */
            int s0 = imin(imax(sa, off), e), s1 = imin(sb, e), nl = sb > e && e < t->len;
            int xa = tx - t->scrollx + gfx_text_w(t->font, t->buf + off, s0 - off);
            int xb = tx - t->scrollx + gfx_text_w(t->font, t->buf + off, s1 - off) + (nl ? 8 : 0);
            if (xb > xa) gfx_fill(xa, ly - 1, xb - xa, lh, col_alpha(ACC, 140));
        }
        gfx_text_n(t->font, tx - t->scrollx, ly + 2, t->buf + off, e - off, 0xFFFFFFFF);
        if (focus && ln == line && (!g_blink || (int)ticks() - t->last_edit < 600))
            gfx_fill(tx - t->scrollx + curpx, ly, 2, lh - 1, ACC);
        gfx_noclip();
        if (e >= t->len) break;
        off = e + 1;
    }
    if (total > rows) {                                                   /* scrollbar */
        int bh = imax(24, h * rows / total), by = y + (h - bh) * t->scrolly / imax(1, total - rows);
        gfx_rrect(x + w - 6, by, 4, bh, 2, WHITE_A(70), WHITE_A(70), 0);
    }
}

/* ============================== single-line prompt ============================== */
void pr_open(Prompt *p, const char *title, const char *init, int id) {
    p->active = 1; p->id = id; p->selall = 0; kstrcpy(p->title, title, sizeof p->title);
    kstrcpy(p->buf, init, sizeof p->buf); p->len = (int)strlen(p->buf);
}
void pr_paste(Prompt *p) {
    char t[512]; int more = 0, n = clip_line(t, sizeof t, &more);
    if (!n) { ui_flash("Clipboard is empty"); return; }
    if (p->selall) { p->len = 0; p->buf[0] = 0; p->selall = 0; }
    int room = (int)sizeof p->buf - 1 - p->len, take = imin(n, room);
    memcpy(p->buf + p->len, t, take); p->len += take; p->buf[p->len] = 0;
    if (take < n) ui_flash("Pasted (cut off: field is full)");
    else if (more) ui_flash("Pasted the first line only");
    else clip_notify("Pasted", take);
}
int pr_key(Prompt *p, const Event *e) {
    int ck = clip_key(e);
    if (ck == CK_ALL) { p->selall = p->len > 0; return 0; }
    if (ck == CK_PASTE) { pr_paste(p); return 0; }
    if (ck) {                                                              /* copy / cut take the whole field */
        if (p->len) { clip_set(p->buf, p->len); clip_notify(ck == CK_CUT ? "Cut" : "Copied", clip_len()); }
        if (ck == CK_CUT) { p->len = 0; p->buf[0] = 0; }
        p->selall = 0; return 0;
    }
    if (e->key == K_ENTER) { if (p->len) { p->active = 0; return 1; } return 0; }
    if (e->key == K_ESC) { p->active = 0; return 2; }
    if (e->key == K_BS || e->key == K_DEL) {
        if (p->selall) { p->len = 0; p->buf[0] = 0; p->selall = 0; }
        else if (e->key == K_BS && p->len) p->buf[--p->len] = 0;
    }
    else if (e->ch >= 32 && e->ch < 127) {
        if (p->selall) { p->len = 0; p->buf[0] = 0; p->selall = 0; }
        if (p->len < (int)sizeof p->buf - 1) { p->buf[p->len++] = (char)e->ch; p->buf[p->len] = 0; }
    }
    return 0;
}
void pr_draw(Prompt *p, int x, int y, int w, int h) {
    gfx_fill(x - 40, y - 20, w + 80, h + 60, BLACK_A(140));
    int bw = imin(w, 560), bh = 170, bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
    gfx_shadow(bx, by + 8, bw, bh, 22, 18, 120);
    gfx_rrect(bx, by, bw, bh, 22, ARGB(255, 30, 36, 58), ARGB(255, 20, 25, 42), 0);
    gfx_rrect(bx, by, bw, bh, 22, WHITE_A(44), WHITE_A(44), 1);
    gfx_text(F_MDB, bx + 28, by + 22, p->title, 0xFFFFFFFF);
    gfx_rrect(bx + 28, by + 66, bw - 56, 46, 12, WHITE_A(22), WHITE_A(22), 0);
    gfx_rrect(bx + 28, by + 66, bw - 56, 46, 12, ACC, ACC, 2);
    gfx_clip(bx + 34, by + 66, bw - 68, 46);
    int tw = gfx_text_w(F_MD, p->buf, -1), off = imax(0, tw - (bw - 90));
    if (p->selall && tw > 0) gfx_fill(bx + 40 - off, by + 72, tw + 4, 34, col_alpha(ACC, 150));
    gfx_text(F_MD, bx + 42 - off, by + 77, p->buf, 0xFFFFFFFF);
    if (!g_blink && !p->selall) gfx_fill(bx + 42 - off + tw + 1, by + 76, 2, 26, ACC);
    gfx_noclip();
    gfx_text(F_SM, bx + 28, by + 128, "Enter to confirm   Esc to cancel   Ctrl+V to paste", WHITE_A(140));
}

/* ============================== calculator ============================== */
enum { C_DOT = 10, C_AC, C_NEG, C_PCT, C_DEL, C_DIV, C_MUL, C_SUB, C_ADD, C_EQ, C_SQRT, C_SQ, C_RECIP, C_PI };
static double c_acc; static char c_buf[24]; static int c_entry, c_op, c_err, c_fresh;
static u32 c_ops[4][40 * 40]; static int c_ops_ok;
static const char *c_op_svg[4] = {
    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.4' stroke-linecap='round'><path d='M5 12h14M12 5v14'/></svg>",
    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.4' stroke-linecap='round'><path d='M5 12h14'/></svg>",
    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.4' stroke-linecap='round'><path d='M6 6l12 12M18 6L6 18'/></svg>",
    "<svg viewBox='0 0 24 24' fill='none' stroke='#fff' stroke-width='2.4' stroke-linecap='round'><path d='M5 12h14'/><circle cx='12' cy='6' r='1.2' fill='#fff'/><circle cx='12' cy='18' r='1.2' fill='#fff'/></svg>" };

static void fmt_double(double v, char *out) {
    if (v != v || v > 1e15 || v < -1e15) { kstrcpy(out, "Error", 24); return; }
    int neg = v < 0; if (neg) v = -v;
    u64 ip = (u64)v; double fr = v - (double)ip;
    u32 f = (u32)(fr * 1e9 + 0.5);
    if (f >= 1000000000u) { ip++; f -= 1000000000u; }
    if (ip == 0 && f == 0) neg = 0;
    char tmp[24]; int n = 0, o = 0;
    u32 hi = (u32)(ip / 1000000000ull), lo = (u32)(ip % 1000000000ull);
    char ds[24]; int dn = 0;
    if (hi) { u32 x = hi; char r[12]; int rn = 0; while (x) { r[rn++] = '0' + x % 10; x /= 10; } while (rn) ds[dn++] = r[--rn];
              for (int i = 8; i >= 0; i--) { u32 d = lo; for (int k = 0; k < i; k++) d /= 10; ds[dn++] = '0' + d % 10; } }
    else { u32 x = lo; char r[12]; int rn = 0; if (!x) r[rn++] = '0'; while (x) { r[rn++] = '0' + x % 10; x /= 10; } while (rn) ds[dn++] = r[--rn]; }
    if (neg) out[o++] = '-';
    for (int i = 0; i < dn; i++) out[o++] = ds[i];
    if (f) {
        out[o++] = '.';
        for (int i = 8; i >= 0; i--) { u32 d = f; for (int k = 0; k < i; k++) d /= 10; tmp[n++] = '0' + d % 10; }
        while (n && tmp[n - 1] == '0') n--;
        for (int i = 0; i < n; i++) out[o++] = tmp[i];
    }
    out[o] = 0;
}
static double c_parse(const char *s) {
    double v = 0, sc = 0.1; int dot = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }                                       /* displayed values can be negative */
    for (; *s; s++) {
        if (*s == '.') dot = 1;
        else if (!dot) v = v * 10 + (*s - '0');
        else { v += (*s - '0') * sc; sc /= 10; }
    }
    return neg ? -v : v;
}
static double c_cur(void) { return c_entry ? c_parse(c_buf) : c_acc; }
static void c_show(double v) { fmt_double(v, c_buf); c_entry = 1; c_fresh = 1; if (c_buf[0] == 'E') c_err = 1; }
static double c_apply(double a, int op, double b) {
    switch (op) {
        case C_DIV: if (b == 0) { c_err = 1; return 0; } return a / b;
        case C_MUL: return a * b; case C_SUB: return a - b; case C_ADD: return a + b;
    }
    return b;
}
static void c_press_(int id) {
    if (c_err && id != C_AC) return;
    if (id <= 9 || id == C_DOT) {
        if (c_fresh || !c_entry) { c_buf[0] = 0; c_fresh = 0; }
        int n = (int)strlen(c_buf);
        if (id == C_DOT) { for (int i = 0; i < n; i++) if (c_buf[i] == '.') goto done; if (!n) { c_buf[n++] = '0'; } c_buf[n++] = '.'; }
        else if (n < 15) { if (n == 1 && c_buf[0] == '0') n = 0; c_buf[n++] = (char)('0' + id); }
        c_buf[n] = 0; c_entry = 1;
    } else switch (id) {
        case C_AC: c_acc = 0; c_buf[0] = 0; c_entry = 0; c_op = 0; c_err = 0; c_fresh = 0; break;
        case C_DEL: if (c_entry && !c_fresh) { int n = (int)strlen(c_buf); if (n > 0) c_buf[n - 1] = 0; if (!c_buf[0]) c_entry = 0; } break;
        case C_NEG: { double v = -c_cur(); if (v == 0) v = 0; c_show(v); c_fresh = 0; break; }
        case C_PCT: c_show(c_cur() / 100.0); break;
        case C_SQRT: { double v = c_cur(); if (v < 0) c_err = 1; c_show(v < 0 ? 0 : d_sqrt(v)); if (c_err) kstrcpy(c_buf, "Error", 24); break; }
        case C_SQ: { double v = c_cur(); c_show(v * v); break; }
        case C_RECIP: { double v = c_cur(); if (v == 0) { c_err = 1; kstrcpy(c_buf, "Error", 24); c_entry = 1; } else c_show(1.0 / v); break; }
        case C_PI: c_show(3.14159265358979); break;
        case C_DIV: case C_MUL: case C_SUB: case C_ADD:
            if (c_op && c_entry && !c_fresh) c_acc = c_apply(c_acc, c_op, c_parse(c_buf));
            else if (c_entry) c_acc = c_parse(c_buf);
            c_op = id; c_entry = 0; c_buf[0] = 0; c_fresh = 0;
            if (c_err) { c_entry = 1; kstrcpy(c_buf, "Error", 24); }
            break;
        case C_EQ:
            if (c_op && (c_entry || 1)) { double r = c_apply(c_acc, c_op, c_entry ? c_parse(c_buf) : c_acc); c_op = 0; c_acc = r; c_show(r); }
            break;
    }
done:;
}
static void c_press(int id) { int was = c_err; c_press_(id); if (c_err && !was) snd_play(SND_ERROR); }   /* error beep on divide by zero etc. */
static void calc_open(void) {
    if (!c_ops_ok) { for (int i = 0; i < 4; i++) svg_render(c_op_svg[i], (int)strlen(c_op_svg[i]), c_ops[i], 40, 0xFFFFFFFF); c_ops_ok = 1; }
}
static void calc_close(void) {}
static void calc_copy(void) {
    char val[32];
    if (c_err) { ui_flash("Nothing to copy"); return; }
    if (c_entry) kstrcpy(val, c_buf[0] ? c_buf : "0", sizeof val); else fmt_double(c_acc, val);
    clip_set(val, (int)strlen(val)); clip_notify("Copied", clip_len());
}
static void calc_paste(void) {                                             /* accepts -1,234.5 style text; stops at the first other character */
    char t[64], num[24]; int n = clip_line(t, sizeof t, 0), k = 0, neg = 0, dot = 0, digits = 0;
    for (int i = 0; i < n && k < 22; i++) {
        char c = t[i];
        if (c >= '0' && c <= '9') { num[k++] = c; digits++; }
        else if (c == '.' && !dot) { dot = 1; num[k++] = c; }
        else if (c == '-' && k == 0 && !neg) neg = 1;
        else if (c == '+' && k == 0) continue;
        else if (c == ',' || c == ' ' || c == '_') continue;
        else break;
    }
    if (!digits) { ui_flash("Clipboard does not hold a number"); return; }
    num[k] = 0;
    if (c_err) c_press(C_AC);
    double v = c_parse(num); if (neg) v = -v;
    c_show(v); c_fresh = 0;
    if (c_err) ui_flash("Number is too large"); else ui_flash("Pasted");
}
static int calc_key(const Event *e) {
    int ck = clip_key(e);
    if (ck == CK_COPY || ck == CK_CUT) { calc_copy(); if (ck == CK_CUT) c_press(C_AC); ui_wake(); return 1; }
    if (ck == CK_PASTE) { calc_paste(); ui_wake(); return 1; }
    if (e->mods & MOD_CTRL) return 0;
    int c = e->ch, k = e->key;
    if (c >= '0' && c <= '9') c_press(c - '0');
    else if (c == '.' || c == ',') c_press(C_DOT);
    else if (c == '+') c_press(C_ADD); else if (c == '-') c_press(C_SUB);
    else if (c == '*' || c == 'x') c_press(C_MUL); else if (c == '/') c_press(C_DIV);
    else if (c == '%') c_press(C_PCT); else if (c == '=' || k == K_ENTER) c_press(C_EQ);
    else if (k == K_BS) c_press(C_DEL); else if (k == K_DEL || c == 'c' || c == 'C') c_press(C_AC);
    else return 0;
    ui_wake(); return 1;
}
static void calc_draw(int x, int y, int w, int h) {
    int cw = imin(w, 640), cx = x + (w - cw) / 2, gap = 10, dh = 128;
    char val[32], expr[48]; expr[0] = 0;
    if (c_err) kstrcpy(val, "Error", 32); else if (c_entry) kstrcpy(val, c_buf[0] ? c_buf : "0", 32); else fmt_double(c_acc, val);
    if (c_op) { fmt_double(c_acc, expr); kstrcat(expr, c_op == C_DIV ? " /" : c_op == C_MUL ? " x" : c_op == C_SUB ? " -" : " +", 48); }
    gfx_rrect(cx, y, cw, dh, 22, WHITE_A(20), WHITE_A(10), 0);
    if (ui_click_in(cx, y, cw, dh)) calc_copy();                           /* click the display to copy it */
    gfx_text_r(F_MD, cx + cw - 26, y + 16, expr, WHITE_A(140));
    int f = gfx_text_w(F_XL, val, -1) > cw - 52 ? F_LG : F_XL;
    gfx_text_r(f, cx + cw - 26, y + dh - fonts[f].h - 12, val, 0xFFFFFFFF);
    int bh = imin(64, (h - dh - 14 - 4 * gap) / 5), bw = (cw - 4 * gap) / 5, by0 = y + dh + 14;
    static const char *lab[25] = { "AC", "+/-", "%", "DEL", "", "7", "8", "9", "sqrt", "", "4", "5", "6", "x^2", "", "1", "2", "3", "1/x", "", "0", "", ".", "pi", "=" };
    static const int ids[25] = { C_AC, C_NEG, C_PCT, C_DEL, C_DIV, 7, 8, 9, C_SQRT, C_MUL, 4, 5, 6, C_SQ, C_SUB, 1, 2, 3, C_RECIP, C_ADD, 0, -1, C_DOT, C_PI, C_EQ };
    for (int i = 0; i < 25; i++) {
        int r = i / 5, c = i % 5, id = ids[i]; if (id < 0) continue;
        int bx = cx + c * (bw + gap), by = by0 + r * (bh + gap), ww = bw;
        if (id == 0) ww = bw * 2 + gap;
        int op = id >= C_DIV && id <= C_ADD, eq = id == C_EQ, fn = id == C_AC || id == C_NEG || id == C_PCT || id == C_DEL || id >= C_SQRT;
        int hv = ui_hover(bx, by, ww, bh);
        u32 base = (op || eq) ? ACC : (fn ? WHITE_A(52) : WHITE_A(28));
        u32 col = hv ? col_lerp(base, 0xFFFFFFFF, op || eq ? 40 : 0) : base;
        if (hv && !(op || eq)) col = WHITE_A(fn ? 78 : 50);
        gfx_rrect(bx, by, ww, bh, 16, col, col, 0);
        if (op && id != C_ADD + 100) gfx_blit_pm(c_ops[id - C_DIV == 0 ? 3 : id == C_MUL ? 2 : id == C_SUB ? 1 : 0], 40, 40, bx + (ww - 40) / 2, by + (bh - 40) / 2, 255);
        else {
            char nb[4]; const char *t = lab[i];
            if (id <= 9) { nb[0] = '0' + id; nb[1] = 0; t = nb; }
            gfx_text_c(id <= 9 || eq ? F_LG : F_MDB, bx + ww / 2, by + (bh - fonts[id <= 9 || eq ? F_LG : F_MDB].h) / 2, t, 0xFFFFFFFF);
        }
        if (ui_click_in(bx, by, ww, bh)) c_press(id);
    }
}

/* ============================== notes ============================== */
#define NOTE_MAX 64
static int note_ids[NOTE_MAX], note_n, note_loaded = -1, note_scroll;
static char note_buf[FS_BLK + 1]; static TextBuf ntb; static int note_saved_flash;

static void notes_refresh(void) {
    int d = fs_lookup("/notes");
    if (d < 0) { fs_create("/notes", FS_DIR); d = fs_lookup("/notes"); }
    note_n = 0;
    int tmp[NOTE_MAX];
    for (int c = fs_child(d, -1); c >= 0 && note_n < NOTE_MAX; c = fs_child(d, c)) tmp[note_n++] = c;
    for (int i = 0; i < note_n; i++) note_ids[i] = tmp[note_n - 1 - i];     /* newest first */
}
void notes_save_now(void) {
    if (note_loaded > 0 && ntb.dirty) {
        if (fs_nodes[note_loaded].used) { fs_write(note_loaded, ntb.buf, ntb.len); fs_save(); }
        ntb.dirty = 0; note_saved_flash = (int)ticks();
    }
}
static void note_load(int node) {
    notes_save_now();
    note_loaded = node;
    if (node > 0) tb_set(&ntb, fs_data(node), (int)fs_nodes[node].size);
    else tb_set(&ntb, "", 0);
}
static void note_new(void) {
    notes_save_now();
    char path[40], num[8];
    for (int i = 1; i < 1000; i++) {
        kstrcpy(path, "/notes/note-", sizeof path); kitoa(num, i); kstrcat(path, num, sizeof path); kstrcat(path, ".txt", sizeof path);
        if (fs_lookup(path) < 0) break;
    }
    int n = fs_create(path, FS_FILE);
    if (n < 0) { ui_flash("Storage is full"); return; }
    notes_refresh(); note_load(n); ui_wake();
}
static void notes_open(void) {
    notes_refresh();
    int keep = note_loaded;
    note_loaded = -1;
    if (keep > 0 && fs_nodes[keep].used) note_load(keep);
    else if (note_n) note_load(note_ids[0]);
    else note_load(-1);
}
static void notes_close(void) { notes_save_now(); }
static void note_title(int node, char *out, int cap) {
    const char *d = fs_data(node); int n = (int)fs_nodes[node].size, i = 0;
    if (node == note_loaded) { d = ntb.buf; n = ntb.len; }
    while (i < n && (d[i] == ' ' || d[i] == '\n')) i++;
    int k = 0;
    while (i < n && d[i] != '\n' && k < 20) out[k++] = d[i++];
    out[k] = 0;
    if (!k) kstrcpy(out, "Empty note", cap);
}
static int notes_key(const Event *e) {
    if (e->mods & MOD_CTRL) {
        if (e->key == 'n') { note_new(); return 1; }
        if (e->key == 's') { notes_save_now(); ui_flash("Saved"); return 1; }
        if (e->key == 'd' && note_loaded > 0) { g_dlg_arg = note_loaded; ui_dialog("Delete note", "This note will be removed permanently.", "Cancel", "Delete", DLG_APP + 1); return 1; }
        if (e->key == K_UP || e->key == K_DOWN) {
            for (int i = 0; i < note_n; i++) if (note_ids[i] == note_loaded) {
                int j = iclamp(i + (e->key == K_UP ? -1 : 1), 0, note_n - 1); note_load(note_ids[j]); break; }
            return 1;
        }
    }
    return note_loaded > 0 ? tb_key(&ntb, e) : 0;
}
static void notes_draw(int x, int y, int w, int h) {
    int lw = 300, rowh = 68;
    if (ui_btn(x, y, lw, 46, "+  New note", 1)) note_new();
    int ly = y + 58, lh = h - 58, rows = lh / rowh;
    if (g_wheel && ui_hover(x, ly, lw, lh)) note_scroll = iclamp(note_scroll + g_wheel, 0, imax(0, note_n - rows));
    gfx_clip(x, ly, lw, lh);
    for (int i = note_scroll; i < note_n && i < note_scroll + rows + 1; i++) {
        int ry = ly + (i - note_scroll) * rowh, id = note_ids[i], on = id == note_loaded;
        u32 bg = on ? col_alpha(ACC, 90) : (ui_hover(x, ry, lw, rowh - 6) ? WHITE_A(30) : WHITE_A(14));
        gfx_rrect(x, ry, lw, rowh - 6, 14, bg, bg, 0);
        if (on) gfx_rrect(x, ry, lw, rowh - 6, 14, ACC, ACC, 2);
        char t[32], sz[24]; note_title(id, t, sizeof t);
        kitoa(sz, id == note_loaded ? ntb.len : (int)fs_nodes[id].size); kstrcat(sz, " chars", sizeof sz);
        gfx_text(F_MDB, x + 18, ry + 10, t, 0xFFFFFFFF);
        gfx_text(F_SM, x + 18, ry + 38, sz, WHITE_A(140));
        if (ui_click_in(x, ry, lw, rowh - 6)) note_load(id);
    }
    gfx_noclip();
    int ex = x + lw + 20, ew = w - lw - 20;
    ui_panel(ex, y, ew, h, WHITE_A(14));
    if (note_loaded > 0) {
        if (!ntb.len) gfx_text(F_MD, ex + 20, y + 20, "Start typing...", WHITE_A(90));
        tb_draw(&ntb, ex + 10, y + 12, ew - 20, h - 64, 1, 0);
        gfx_fill(ex + 20, y + h - 46, ew - 40, 1, WHITE_A(26));
        char st[48]; kitoa(st, ntb.len); kstrcat(st, " characters", sizeof st);
        gfx_text(F_SM, ex + 22, y + h - 34, st, WHITE_A(150));
        const char *sv = ntb.dirty ? "Editing..." : "Saved";
        gfx_text_r(F_SM, ex + ew - 150, y + h - 34, sv, ntb.dirty ? WHITE_A(150) : ACC);
        if (ui_btn(ex + ew - 120, y + h - 42, 104, 32, "Delete", 0)) {
            g_dlg_arg = note_loaded; ui_dialog("Delete note", "This note will be removed permanently.", "Cancel", "Delete", DLG_APP + 1);
        }
    } else gfx_text_c(F_MD, ex + ew / 2, y + h / 2 - 12, "No notes yet. Create one with  + New note.", WHITE_A(150));
}

/* ============================== text editor ============================== */
static char ed_buf[FS_BLK + 1]; static TextBuf etb; static char ed_path[96];
static int ed_preview, ed_modal, pk_sel, pk_n, pk_ids[FS_MAX], pk_scroll;
static Prompt ed_pr; static u32 ed_bmp[256 * 256]; static int ed_rend_edit = -1, ed_rend_len = -1;

void editor_open_path(const char *path) {
    int n = fs_lookup(path);
    if (n < 0 || fs_nodes[n].type != FS_FILE) return;
    if (png_is(path)) { ui_flash("PNG images open in Files (Enter on the file)"); return; }
    tb_set(&etb, fs_data(n), (int)fs_nodes[n].size);
    kstrcpy(ed_path, path, sizeof ed_path);
    ed_preview = ends_with(path, ".svg"); ed_rend_edit = -1;
}
static int ed_save(void) {
    if (!ed_path[0]) { pr_open(&ed_pr, "Save file as", "/docs/untitled.txt", 1); ed_modal = 1; return 0; }
    if (fs_write_text(ed_path, etb.buf, etb.len) < 0) { ui_flash("Could not save (check the path)"); return 0; }
    fs_save(); etb.dirty = 0;
    if (starts_with(ed_path, "/system/icons/")) ui_reload_icons();
    ui_flash("Saved"); return 1;
}
static void ed_new_force(void) { tb_set(&etb, "", 0); ed_path[0] = 0; ed_preview = 0; }
static void ed_new(void) {
    if (etb.dirty) {
        if (ed_path[0]) { if (!ed_save()) return; }
        else { ui_dialog("Discard changes?", "The untitled document has not been saved.", "Keep editing", "Discard", DLG_APP + 2); return; }
    }
    ed_new_force();
}
static void ed_picker(void) {
    pk_n = 0;
    for (int i = 1; i < FS_MAX; i++) if (fs_nodes[i].used && fs_nodes[i].type == FS_FILE) pk_ids[pk_n++] = i;
    pk_sel = 0; pk_scroll = 0; ed_modal = 2;
}
static void ed_open(void) {}
static void ed_close(void) { if (etb.dirty && ed_path[0]) ed_save(); }
static int ed_key(const Event *e) {
    if (ed_modal == 1) {
        int r = pr_key(&ed_pr, e);
        if (r == 1) {
            char p[100]; p[0] = 0;
            if (ed_pr.buf[0] != '/') kstrcpy(p, "/docs/", sizeof p);
            kstrcat(p, ed_pr.buf, sizeof p);
            kstrcpy(ed_path, p, sizeof ed_path); ed_modal = 0; ed_save();
            if (ends_with(ed_path, ".svg")) ed_preview = 1;
        } else if (r == 2) ed_modal = 0;
        return 1;
    }
    if (ed_modal == 2) {
        if (e->key == K_ESC) ed_modal = 0;
        else if (e->key == K_UP && pk_sel > 0) pk_sel--;
        else if (e->key == K_DOWN && pk_sel < pk_n - 1) pk_sel++;
        else if (e->key == K_ENTER && pk_n) { char p[96]; fs_path(pk_ids[pk_sel], p, sizeof p); editor_open_path(p); ed_modal = 0; }
        return 1;
    }
    if (e->mods & MOD_CTRL) {
        if (e->key == 's') { ed_save(); return 1; }
        if (e->key == 'o') { ed_picker(); return 1; }
        if (e->key == 'n') { ed_new(); return 1; }
    }
    if (e->key == K_F5) { ed_preview ^= 1; ed_rend_edit = -1; return 1; }
    return tb_key(&etb, e);
}
static void ed_draw(int x, int y, int w, int h) {
    /* toolbar */
    char name[110]; kstrcpy(name, ed_path[0] ? ed_path : "untitled", sizeof name);
    if (etb.dirty) kstrcat(name, " *", sizeof name);
    int bx = x + w - 4 * 104 + 8;
    gfx_rrect(x, y, bx - x - 14, 40, 12, WHITE_A(20), WHITE_A(20), 0);
    gfx_text(F_MD, x + 16, y + 8, name, 0xFFFFFFFF);
    if (ui_btn(bx, y + 2, 96, 36, "New", 0) && !ed_modal) ed_new();
    if (ui_btn(bx + 104, y + 2, 96, 36, "Open", 0) && !ed_modal) ed_picker();
    if (ui_btn(bx + 208, y + 2, 96, 36, "Save", 1) && !ed_modal) ed_save();
    if (ui_btn(bx + 312, y + 2, 96, 36, ed_preview ? "Hide" : "Preview", 0) && !ed_modal) { ed_preview ^= 1; ed_rend_edit = -1; }
    int top = y + 52, ah = h - 52 - 34;
    int ew = ed_preview ? w * 56 / 100 : w;
    ui_panel(x, top, ew, ah, ARGB(120, 6, 9, 20));
    tb_draw(&etb, x + 4, top + 8, ew - 8, ah - 16, ed_modal == 0, 1);
    if (ed_preview) {
        int px = x + ew + 14, pw = w - ew - 14;
        ui_panel(px, top, pw, ah, WHITE_A(12));
        int s = imin(imin(pw - 28, ah - 28), 256);
        int dx = px + (pw - s) / 2, dy = top + (ah - s) / 2;
        for (int cy = 0; cy < s; cy += 16) for (int cx = 0; cx < s; cx += 16) {      /* transparency checkerboard */
            u32 c = ((cx + cy) / 16) & 1 ? WHITE_A(30) : WHITE_A(14);
            gfx_fill(dx + cx, dy + cy, imin(16, s - cx), imin(16, s - cy), c);
        }
        if ((ed_rend_edit != etb.last_edit || ed_rend_len != etb.len) && (int)ticks() - etb.last_edit > 150) {
            svg_render(etb.buf, etb.len, ed_bmp, 256, 0xFFFFFFFF); ed_rend_edit = etb.last_edit; ed_rend_len = etb.len;
        }
        gfx_blit_scaled(ed_bmp, 256, 256, dx, dy, s, s, 255);
        gfx_text_c(F_SM, px + pw / 2, top + ah - 24, "SVG preview", WHITE_A(120));
    }
    char st[96], t[12]; st[0] = 0;
    kstrcat(st, "Ln ", sizeof st); kitoa(t, tb_line(&etb) + 1); kstrcat(st, t, sizeof st);
    kstrcat(st, ", Col ", sizeof st); kitoa(t, tb_col(&etb) + 1); kstrcat(st, t, sizeof st);
    kstrcat(st, "     ", sizeof st); kitoa(t, etb.len); kstrcat(st, t, sizeof st); kstrcat(st, " chars", sizeof st);
    gfx_text(F_SM, x + 6, y + h - 24, st, WHITE_A(150));
    gfx_text_r(F_SM, x + w - 6, y + h - 24, "Ctrl+S save   Ctrl+O open   Ctrl+N new   F5 SVG preview", WHITE_A(110));
    if (ed_modal == 1) pr_draw(&ed_pr, x, y, w, h);
    if (ed_modal == 2) {
        int bw = imin(w, 600), bh = imin(h, 440), qx = x + (w - bw) / 2, qy = y + (h - bh) / 2;
        gfx_fill(x - 40, y - 20, w + 80, h + 60, BLACK_A(140));
        gfx_rrect(qx, qy, bw, bh, 22, ARGB(255, 30, 36, 58), ARGB(255, 20, 25, 42), 0);
        gfx_rrect(qx, qy, bw, bh, 22, WHITE_A(44), WHITE_A(44), 1);
        gfx_text(F_MDB, qx + 28, qy + 20, "Open file", 0xFFFFFFFF);
        int rows = (bh - 80) / 34;
        if (pk_sel < pk_scroll) pk_scroll = pk_sel;
        if (pk_sel >= pk_scroll + rows) pk_scroll = pk_sel - rows + 1;
        for (int i = pk_scroll; i < pk_n && i < pk_scroll + rows; i++) {
            int ry = qy + 66 + (i - pk_scroll) * 34;
            char p[96]; fs_path(pk_ids[i], p, sizeof p);
            if (i == pk_sel) gfx_rrect(qx + 14, ry, bw - 28, 30, 10, col_alpha(ACC, 130), col_alpha(ACC, 130), 0);
            gfx_text(F_MD, qx + 28, ry + 3, p, 0xFFFFFFFF);
            if (ui_click_in(qx + 14, ry, bw - 28, 30)) { pk_sel = i; editor_open_path(p); ed_modal = 0; }
        }
    }
}

/* ============================== app table ============================== */
App apps[APP_COUNT];
void apps_init(void) {
    tb_init(&ntb, note_buf, sizeof note_buf, F_MD);
    tb_init(&etb, ed_buf, sizeof ed_buf, F_MONO); etb.autoindent = 1;
    apps[APP_NOTES]    = (App){ "Notes", IC_NOTES, notes_open, notes_close, notes_key, notes_draw };
    apps[APP_EDITOR]   = (App){ "Text Editor", IC_EDITOR, ed_open, ed_close, ed_key, ed_draw };
    apps[APP_CALC]     = (App){ "Calculator", IC_CALC, calc_open, calc_close, calc_key, calc_draw };
    apps[APP_FILES]    = (App){ "Files", IC_FILES, files_open, files_close, files_key, files_draw };
    apps[APP_SETTINGS] = (App){ "Settings", IC_SETTINGS, set_open, set_close, set_key, set_draw };
    apps[APP_TASKMGR]  = (App){ "Task Manager", IC_TASKMGR, tm_open, tm_close, tm_key, tm_draw };
    apps[APP_NETWORK]  = (App){ "Network", IC_NETWORK, na_open, na_close, na_key, na_draw };
    apps[APP_BROWSER]  = (App){ "Browser", IC_BROWSER, browser_open, browser_close, browser_key, browser_draw };
    apps[APP_DISCORD]   = (App){ "Discord", IC_DISCORD, discord_open, discord_close, discord_key, discord_draw };
    apps[APP_ZIP]       = (App){ "Zip", IC_ZIP, zip_open, zip_close, zip_key, zip_draw };
    apps[APP_VIEWER]    = (App){ "Image Viewer", IC_VIEWER, viewer_open, viewer_close, viewer_key, viewer_draw };
    apps[APP_PAINT]     = (App){ "Paint", IC_PAINT, paint_open, paint_close, paint_key, paint_draw };
}
u32 app_static_bytes(int app) {
    switch (app) {
        case APP_NOTES:  return (u32)(sizeof note_buf + sizeof note_ids + sizeof ntb);
        case APP_EDITOR: return (u32)(sizeof ed_buf + sizeof ed_bmp + sizeof ed_pr + sizeof etb + sizeof pk_ids);
        case APP_CALC:   return (u32)sizeof c_ops;
        case APP_TASKMGR: return 512;
        case APP_NETWORK: return netapp_static_bytes();               /* history ring and counters */
        case APP_BROWSER: return browser_static_bytes();
        case APP_DISCORD: return discord_static_bytes();
        case APP_ZIP:    return zip_static_bytes();
        case APP_VIEWER: return viewer_static_bytes();
        case APP_PAINT:  return paint_static_bytes();
        case APP_FILES:  return files_static_bytes();
        default:         return 0;
    }
}
void apps_tick(void) {
    if (ntb.dirty && note_loaded > 0 && (int)ticks() - ntb.last_edit > 1200) { notes_save_now(); ui_wake(); }
    set_tick();
    tm_tick();
    na_tick();
    browser_tick();
    discord_tick();
}
void apps_dialog_done(int action, int arg) {
    if (action == DLG_APP + 1) {
        if (arg == note_loaded) { note_loaded = -1; ntb.dirty = 0; }
        fs_delete(arg); fs_save(); notes_refresh();
        note_load(note_n ? note_ids[0] : -1); ui_flash("Note deleted");
    } else if (action == DLG_APP + 2) ed_new_force();
    else files_dialog(action, arg);
}
