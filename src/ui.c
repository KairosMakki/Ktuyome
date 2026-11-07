#include "ktuyome.h"
/* Ktuyome shell: boot splash, PS5-style home screen, app host, dialogs, widgets, main loop. */

/* ============================== settings ============================== */
Settings cfg = { 0x3153544B, 0, 0, 100, 2, 1, 0 };
const u32 accents[5] = { 0xFF3D8BFF, 0xFF8B5CFF, 0xFF19C3A6, 0xFFFF5C8A, 0xFFFFB020 };
const char *accent_names[5] = { "Blue", "Violet", "Teal", "Pink", "Amber" };
const char *wall_names[3] = { "Midnight", "Aurora", "Ember" };
static int baked_wall = -1;

void settings_apply(void) {
    cfg.accent = iclamp(cfg.accent, 0, 4); cfg.wall = iclamp(cfg.wall, 0, 2);
    cfg.bright = iclamp(cfg.bright, 30, 100); cfg.mspeed = iclamp(cfg.mspeed, 1, 4);
    g_brightness = cfg.bright; mouse_speed = cfg.mspeed;
    if (baked_wall != cfg.wall) { gfx_bake_wallpaper(cfg.wall); baked_wall = cfg.wall; }
}
void settings_load(void) {
    char buf[64]; int n = fs_read_text("/system/settings.cfg", buf, sizeof buf);
    if (n == (int)sizeof(Settings)) { Settings s; memcpy(&s, buf, sizeof s); if (s.magic == cfg.magic) cfg = s; }
}
void settings_save(void) {
    fs_write_text("/system/settings.cfg", (const char *)&cfg, sizeof cfg);
    fs_save();
}

/* ============================== animation ============================== */
typedef struct { int cur, tgt; } Anim;           /* 16.16 fixed point */
static void anim_step(Anim *a) { int d = a->tgt - a->cur; if (iabs(d) < 300) a->cur = a->tgt; else a->cur += d / 4; }
static int anim_busy(const Anim *a) { return a->cur != a->tgt; }

/* ============================== icons ============================== */
static u32 icon_big[IC_COUNT][128 * 128];
static u32 icon_small[IC_COUNT][48 * 48];
static u32 cursor_bmp[28 * 28], hint_cross[28 * 28], hint_circle[28 * 28], logo_bmp[128 * 128];
static char svgbuf[FS_BLK + 1];

static const char *logo_svg =
    "<svg viewBox='0 0 96 96'><defs><linearGradient id='g' x1='0' y1='0' x2='1' y2='1'>"
    "<stop offset='0' stop-color='#6EA8FF'/><stop offset='1' stop-color='#8B5CFF'/></linearGradient></defs>"
    "<rect x='6' y='6' width='84' height='84' rx='26' fill='url(#g)'/>"
    "<path d='M35 25v46M61 25L36 49M44 42l20 29' stroke='#fff' stroke-width='8.5' stroke-linecap='round' stroke-linejoin='round' fill='none'/></svg>";
static const char *cursor_svg =
    "<svg viewBox='0 0 24 24'><path d='M4 2.5v17l4.6-4.2 3 6.6 3-1.4-3-6.4h6.2z' fill='#fff' stroke='#10131c' stroke-width='1.5' stroke-linejoin='round'/></svg>";
static const char *cross_svg =
    "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='10.5' fill='none' stroke='#fff' stroke-width='1.6'/>"
    "<path d='M8.3 8.3l7.4 7.4M15.7 8.3l-7.4 7.4' stroke='#fff' stroke-width='1.9' stroke-linecap='round'/></svg>";
static const char *circle_svg =
    "<svg viewBox='0 0 24 24'><circle cx='12' cy='12' r='10.5' fill='none' stroke='#fff' stroke-width='1.6'/>"
    "<circle cx='12' cy='12' r='4.8' fill='none' stroke='#fff' stroke-width='1.9'/></svg>";

void ui_reload_icons(void) {
    char path[64];
    for (int i = 0; i < IC_COUNT; i++) {
        kstrcpy(path, "/system/icons/", sizeof path); kstrcat(path, icon_names[i], sizeof path); kstrcat(path, ".png", sizeof path);
        int pn = fs_lookup(path), done = 0;
        if (pn > 0 && fs_nodes[pn].type == FS_FILE && fs_nodes[pn].size) {      /* a PNG icon wins over the SVG */
            PngImage im;
            if (png_decode(fs_data(pn), (int)fs_nodes[pn].size, &im)) {
                png_fit(&im, icon_big[i], 128); png_fit(&im, icon_small[i], 48); png_free(&im); done = 1;
            }
        }
        if (done) continue;
        kstrcpy(path, "/system/icons/", sizeof path); kstrcat(path, icon_names[i], sizeof path); kstrcat(path, ".svg", sizeof path);
        int n = fs_read_text(path, svgbuf, sizeof svgbuf);
        const char *src = n > 0 ? svgbuf : icon_svgs[i];
        if (n <= 0) n = (int)strlen(src);
        svg_render(src, n, icon_big[i], 128, 0xFFFFFFFF);
        svg_render(src, n, icon_small[i], 48, 0xFFFFFFFF);
    }
}
static void render_static_icons(void) {
    svg_render(cursor_svg, (int)strlen(cursor_svg), cursor_bmp, 28, 0xFFFFFFFF);
    svg_render(cross_svg, (int)strlen(cross_svg), hint_cross, 28, 0xFFFFFFFF);
    svg_render(circle_svg, (int)strlen(circle_svg), hint_circle, 28, 0xFFFFFFFF);
    svg_render(logo_svg, (int)strlen(logo_svg), logo_bmp, 128, 0xFFFFFFFF);
}
void ui_icon(int icon, int x, int y, int size, int alpha) {
    if (size <= 56) gfx_blit_scaled(icon_small[icon], 48, 48, x, y, size, size, alpha);
    else gfx_blit_scaled(icon_big[icon], 128, 128, x, y, size, size, alpha);
}

/* ============================== state ============================== */
enum { ST_HOME, ST_APP };
typedef struct { const char *name, *sub; int icon; u32 c1, c2; int kind, arg; } Tile;
static const Tile tiles[2][7] = {
    { { "Notes", "Capture ideas. Saved automatically.", IC_NOTES, 0xFFFFB74D, 0xFFFF7043, 0, APP_NOTES },
      { "Text Editor", "Write and edit any file. Previews SVG.", IC_EDITOR, 0xFF4FC3F7, 0xFF3D5AFE, 0, APP_EDITOR },
      { "Calculator", "Quick arithmetic with a scientific column.", IC_CALC, 0xFF4DD0A1, 0xFF00897B, 0, APP_CALC },
      { "Files", "Browse, create and preview your files.", IC_FILES, 0xFFB388FF, 0xFF7C4DFF, 0, APP_FILES },
      { "Task Manager", "Live CPU, memory and storage monitor.", IC_TASKMGR, 0xFFF48FB1, 0xFFD81B60, 0, APP_TASKMGR },
      { "Network", "Ping, traceroute, DNS, ports and HTTP.", IC_NETWORK, 0xFF9FA8DA, 0xFF3949AB, 0, APP_NETWORK },
      { "Zip", "Create and extract zip archives.", IC_ZIP, 0xFF9CE29B, 0xFF388E3C, 0, APP_ZIP } },
    { { "Settings", "Display, input, date and time, storage.", IC_SETTINGS, 0xFF90A4AE, 0xFF546E7A, 0, APP_SETTINGS },
      { "Browser", "Text-mode web browser, HTTP and HTTPS.", IC_BROWSER, 0xFF64B5F6, 0xFF1565C0, 0, APP_BROWSER },
      { "Discord", "Chat client with WebSocket Gateway.", IC_DISCORD, 0xFF7986CB, 0xFF5865F2, 0, APP_DISCORD },
      { "About", "System information and hardware.", IC_ABOUT, 0xFF81D4FA, 0xFF0288D1, 3, 0 },
      { "Restart", "Save everything and restart.", IC_RESTART, 0xFFFFD54F, 0xFFFFA000, 1, 0 },
      { "Shut Down", "Save everything and power off.", IC_POWER, 0xFFFF8A80, 0xFFD32F2F, 2, 0 },
      { "Zip", "Create and extract zip archives.", IC_ZIP, 0xFF9CE29B, 0xFF388E3C, 0, APP_ZIP } },
};
static const char *tab_names[2] = { "Apps", "System" };
static const int ntiles[2] = { 7, 6 };

static int g_state = ST_HOME, tab, sel[2];
static Anim sel_anim, tab_anim, open_anim, fade_anim;
static int cur_app = -1, closing, tile_rc[7][4], tab_rc[2][4];
static int home_scroll = 0, home_scroll_max = 0, home_scroll_to_sel = 1;
static int arrow_left[4], arrow_right[4], has_left_arrow, has_right_arrow;
static int open_from[4];
static int app_opens[APP_COUNT]; static u32 frame_count;
int ui_foreground_app(void) { return cur_app; }
int ui_app_opens(int app) { return app >= 0 && app < APP_COUNT ? app_opens[app] : 0; }
u32 ui_frames(void) { return frame_count; }
int g_click, g_click_x, g_click_y, g_click_mods, g_wheel, g_blink, g_dlg_arg;
static int dirty = 1, clk_h, clk_m, clk_s, clk_day, clk_mon, clk_year, drag_id;
static char toast_msg[64]; static u32 toast_until;

void ui_wake(void) { dirty = 1; }
void ui_flash(const char *m) { kstrcpy(toast_msg, m, sizeof toast_msg); toast_until = ticks() + 1800; dirty = 1; }

/* ============================== widgets ============================== */
static int inside(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }
int ui_hover(int x, int y, int w, int h) { return inside(mouse_x, mouse_y, x, y, w, h); }
int ui_click_in(int x, int y, int w, int h) {
    if (g_click && inside(g_click_x, g_click_y, x, y, w, h)) { g_click = 0; return 1; }
    return 0;
}
int ui_btn(int x, int y, int w, int h, const char *label, int primary) {
    int hv = ui_hover(x, y, w, h);
    u32 c = primary ? (hv ? col_lerp(ACC, 0xFFFFFFFF, 40) : ACC) : (hv ? WHITE_A(64) : WHITE_A(34));
    gfx_rrect(x, y, w, h, h / 2 > 18 ? 18 : h / 2, c, c, 0);
    gfx_text_c(F_MDB, x + w / 2, y + (h - fonts[F_MDB].h) / 2, label, 0xFFFFFFFF);
    return ui_click_in(x, y, w, h);
}
int ui_toggle(int x, int y, int on) {
    gfx_rrect(x, y, 52, 28, 14, on ? ACC : WHITE_A(60), on ? ACC : WHITE_A(60), 0);
    gfx_circle(x + (on ? 38 : 14), y + 14, 11, 0xFFFFFFFF);
    return ui_click_in(x - 4, y - 4, 60, 36);
}
int ui_slider(int id, int x, int y, int w, int val, int lo, int hi) {
    int span = hi - lo;
    if (ui_click_in(x - 10, y - 8, w + 20, 32)) drag_id = id;
    if (drag_id == id) {
        if (mouse_btn & 1) val = lo + iclamp((mouse_x - x) * span / w, 0, span);
        else drag_id = 0;
    }
    int fx = (val - lo) * w / span;
    gfx_rrect(x, y + 8, w, 8, 4, WHITE_A(50), WHITE_A(50), 0);
    gfx_rrect(x, y + 8, imax(fx, 8), 8, 4, ACC, ACC, 0);
    gfx_shadow(x + fx - 11, y + 1, 22, 22, 11, 4, 70);
    gfx_circle(x + fx, y + 12, 11, 0xFFFFFFFF);
    return val;
}
void ui_panel(int x, int y, int w, int h, u32 fill) {
    gfx_rrect(x, y, w, h, 16, fill, fill, 0);
    gfx_rrect(x, y, w, h, 16, WHITE_A(26), WHITE_A(26), 1);
}

/* ============================== dialog ============================== */
static struct { int active, action, sel; char title[40], msg[120], b0[20], b1[20]; } dlg;
void ui_dialog(const char *title, const char *msg, const char *b0, const char *b1, int action) {
    dlg.active = 1; dlg.action = action; dlg.sel = action <= DLG_SHUTDOWN ? 1 : 0;
    kstrcpy(dlg.title, title, sizeof dlg.title); kstrcpy(dlg.msg, msg, sizeof dlg.msg);
    kstrcpy(dlg.b0, b0, sizeof dlg.b0); kstrcpy(dlg.b1, b1, sizeof dlg.b1);
    dirty = 1;
}
static void power_screen(const char *msg) {
    gfx_target(g_bb, W, H); gfx_noclip(); gfx_fill(0, 0, W, H, 0xFF000000);
    gfx_text_c(F_LG, W / 2, H / 2 - 20, msg, 0xFFFFFFFF);
    gfx_text_c(F_MD, W / 2, H / 2 + 24, "It is now safe to turn off your computer.", WHITE_A(150));
    g_brightness = 100; gfx_present();
}
static void dialog_confirm(void) {
    int a = dlg.action; dlg.active = 0; dirty = 1;
    if (a == DLG_RESTART) { fs_save(); reboot(); }
    else if (a == DLG_SHUTDOWN) { fs_save(); power_screen("Ktuyome has shut down"); power_off(); for (;;) __asm__ volatile("cli; hlt"); }
    else if (a == DLG_FORMAT) { fs_format(); ui_reload_icons(); settings_apply(); ui_flash("Storage reset to defaults"); }
    else apps_dialog_done(a, g_dlg_arg);
}
static void draw_dialog(void) {
    gfx_fill(0, 0, W, H, BLACK_A(150));
    int w = 560, h = 250, x = (W - w) / 2, y = (H - h) / 2;
    gfx_shadow(x, y + 10, w, h, 28, 24, 120);
    gfx_rrect(x, y, w, h, 28, ARGB(255, 28, 34, 54), ARGB(255, 18, 22, 38), 0);
    gfx_rrect(x, y, w, h, 28, WHITE_A(40), WHITE_A(40), 1);
    gfx_text_c(F_LG, x + w / 2, y + 34, dlg.title, 0xFFFFFFFF);
    gfx_text_c(F_MD, x + w / 2, y + 90, dlg.msg, WHITE_A(190));
    int bw = 220, by = y + h - 84;
    for (int i = 0; i < 2; i++) {
        int bx = x + 40 + i * (bw + 20);
        if (dlg.sel == i) gfx_rrect(bx - 4, by - 4, bw + 8, 56, 22, WHITE_A(230), WHITE_A(230), 2);
        if (ui_btn(bx, by, bw, 48, i ? dlg.b1 : dlg.b0, i == 1)) {
            if (i == 1) dialog_confirm(); else dlg.active = 0;
            dirty = 1;
        }
    }
}

/* ============================== app host ============================== */
void ui_open_app(int app) {
    if (cur_app >= 0 && !closing) return;
    cur_app = app; closing = 0; g_state = ST_APP; app_opens[app]++;
    for (int i = 0; i < 4; i++) open_from[i] = tile_rc[sel[tab]][i];
    if (open_from[2] == 0) { open_from[0] = W / 2 - 80; open_from[1] = H / 2 - 80; open_from[2] = open_from[3] = 160; }
    apps[app].open();
    { char _b[16]; kitoa(_b, home_scroll); fs_write_text("/system/home.txt", _b, (int)strlen(_b)); }
    { char _b[8]; kitoa(_b, tab); fs_write_text("/system/hometab.txt", _b, (int)strlen(_b)); }
    open_anim.cur = 0; open_anim.tgt = 65536; dirty = 1;
}
void ui_close_app(void) {
    if (cur_app < 0 || closing) return;
    apps[cur_app].close(); closing = 1; open_anim.tgt = 0; dirty = 1;
}
void ui_switch_app(int app) {
    if (cur_app < 0 || closing) { ui_open_app(app); return; }
    apps[cur_app].close(); cur_app = app; app_opens[app]++; apps[app].open(); dirty = 1;
}
void ui_logo(int x, int y, int size) { gfx_blit_scaled(logo_bmp, 128, 128, x, y, size, size, 255); }
static void activate_tile(void) {
    const Tile *t = &tiles[tab][sel[tab]];
    switch (t->kind) {
        case 0: ui_open_app(t->arg); break;
        case 1: ui_dialog("Restart", "Restart Ktuyome now?", "Cancel", "Restart", DLG_RESTART); break;
        case 2: ui_dialog("Shut Down", "Turn off the system?", "Cancel", "Shut Down", DLG_SHUTDOWN); break;
        case 3: settings_goto(4); ui_open_app(APP_SETTINGS); break;
    }
}

/* ============================== drawing ============================== */
static int hint_key(int x, int y, const char *key, const char *label) {
    int kw = gfx_text_w(F_SM, key, -1) + 18;
    gfx_rrect(x, y, kw, 26, 8, WHITE_A(24), WHITE_A(24), 0);
    gfx_rrect(x, y, kw, 26, 8, WHITE_A(110), WHITE_A(110), 1);
    gfx_text_c(F_SM, x + kw / 2, y + 3, key, 0xFFFFFFFF);
    gfx_text(F_SM, x + kw + 10, y + 3, label, WHITE_A(190));
    return x + kw + 10 + gfx_text_w(F_SM, label, -1) + 28;
}
static void draw_hints(void) {
    int y = H - 54, x = 88;
    if (g_state == ST_HOME || closing) {
        gfx_blit_pm(hint_cross, 28, 28, x, y - 1, 255); gfx_text(F_SM, x + 38, y + 3, "Select", WHITE_A(190)); x += 38 + 60;
        x = hint_key(x, y, "Tab", "Apps / System");
        x = hint_key(x, y, "Arrows", "Move"); (void)x;
    } else {
        gfx_blit_pm(hint_circle, 28, 28, x, y - 1, 255); gfx_text(F_SM, x + 38, y + 3, "Back (Esc)", WHITE_A(190));
    }
}
static const char *fmt_clock(char *out, int secs) {
    int h = clk_h, m = clk_m; char t[8]; out[0] = 0;
    const char *ap = "";
    if (!cfg.h24) { ap = h >= 12 ? " PM" : " AM"; h = h % 12; if (!h) h = 12; }
    if (cfg.h24 && h < 10) kstrcat(out, "0", 24);
    kitoa(t, h); kstrcat(out, t, 24); kstrcat(out, ":", 24);
    if (m < 10) kstrcat(out, "0", 24); kitoa(t, m); kstrcat(out, t, 24);
    if (secs) { kstrcat(out, ":", 24); if (clk_s < 10) kstrcat(out, "0", 24); kitoa(t, clk_s); kstrcat(out, t, 24); }
    kstrcat(out, ap, 24);
    return out;
}

static void draw_home(void) {
    int oy = imax(0, (H - 720) / 2);
    int base = 148, extra = 44, gap = 24, left = 88, top = 150 + oy;
    int cyc = top + (base + extra) / 2;
    int nt = ntiles[tab], selpos = sel_anim.cur, i0 = iclamp(selpos >> 16, 0, nt - 1), i1 = imin(i0 + 1, nt - 1);
    u32 gcol = col_lerp(tiles[tab][i0].c2, tiles[tab][i1].c2, (selpos & 65535) >> 8);
    gfx_glow(left + 200, cyc, 430, gcol & 0xFFFFFF, 80);

    /* top bar */
    int tx = left, tw[2], txs[2];
    for (int i = 0; i < 2; i++) { tw[i] = gfx_text_w(F_MDB, tab_names[i], -1); txs[i] = tx; tx += tw[i] + 44; }
    int tabf = tab_anim.cur;
    for (int i = 0; i < 2; i++) {
        int on = 256 - iabs(i * 256 - (tabf >> 8)); on = iclamp(on, 0, 256);
        u32 c = col_alpha(0xFFFFFFFF, 130 + (125 * on >> 8));
        gfx_text(F_MDB, txs[i], 34, tab_names[i], c);
        tab_rc[i][0] = txs[i] - 10; tab_rc[i][1] = 24; tab_rc[i][2] = tw[i] + 20; tab_rc[i][3] = 44;
    }
    int ux = txs[0] + (((txs[1] - txs[0]) * (tabf >> 8)) >> 8), uw = tw[0] + (((tw[1] - tw[0]) * (tabf >> 8)) >> 8);
    gfx_rrect(ux, 66, uw, 4, 2, 0xFFFFFFFF, 0xFFFFFFFF, 0);
    char clk[24]; fmt_clock(clk, cfg.secs);
    int axr = W - 88;
    gfx_rrect(axr - 40, 24, 40, 40, 20, accents[cfg.accent], 0xFF8B5CFF, 0);
    gfx_text_c(F_MDB, axr - 20, 33, "K", 0xFFFFFFFF);
    gfx_text_r(F_MDB, axr - 58, 34, clk, 0xFFFFFFFF);

    /* tiles */
    int sizes[7], pos[7], total = 0;
    for (int i = 0; i < nt; i++) {
        int foc = iclamp(65536 - iabs(i * 65536 - selpos), 0, 65536) >> 8;
        sizes[i] = base + (extra * foc >> 8);
    }
    pos[0] = left;
    for (int i = 1; i < nt; i++) pos[i] = pos[i - 1] + sizes[i - 1] + gap;
    if (nt > 0) total = pos[nt - 1] + sizes[nt - 1] - left;

    if (g_wheel) { home_scroll += g_wheel * 40; g_wheel = 0; }
    int visible = W - left - 90;
    if (visible < 100) visible = 100;
    home_scroll_max = total - visible;
    if (home_scroll_max < 0) home_scroll_max = 0;
    home_scroll = iclamp(home_scroll, 0, home_scroll_max);
    int ti = iclamp((selpos + 32768) >> 16, 0, nt - 1);
    if (home_scroll_to_sel && nt > 0) {
        int desired = pos[ti] - left;
        if (desired < 0) desired = 0;
        if (desired > home_scroll_max) desired = home_scroll_max;
        home_scroll = desired;
        home_scroll_to_sel = 0;
    }
    home_scroll = iclamp(home_scroll, 0, home_scroll_max);

    has_left_arrow = home_scroll > 0;
    has_right_arrow = home_scroll < home_scroll_max;
    if (has_left_arrow) { arrow_left[0] = left - 52; arrow_left[1] = cyc - 26; arrow_left[2] = 44; arrow_left[3] = 52; }
    if (has_right_arrow) { arrow_right[0] = W - 56; arrow_right[1] = cyc - 26; arrow_right[2] = 44; arrow_right[3] = 52; }

    for (int i = 0; i < nt; i++) {
        const Tile *t = &tiles[tab][i];
        int s = sizes[i], y = cyc - s / 2, foc = iclamp(65536 - iabs(i * 65536 - selpos), 0, 65536) >> 8;
        int r = s * 26 / base;
        int x = pos[i] - home_scroll;
        tile_rc[i][0] = x; tile_rc[i][1] = y; tile_rc[i][2] = s; tile_rc[i][3] = s;
        if (x + s < left || x > W) continue;
        gfx_shadow(x, y + 10, s, s, r, 16, 60 + (foc * 50 >> 8));
        gfx_rrect(x, y, s, s, r, t->c1, t->c2, 0);
        gfx_rrect(x, y, s, s, r, WHITE_A(52), WHITE_A(0), 0);
        int is = s * 58 / 100;
        ui_icon(t->icon, x + (s - is) / 2, y + (s - is) / 2, is, 255);
        if (foc > 20) {
            u32 ring = WHITE_A(foc * 235 >> 8);
            gfx_rrect(x - 6, y - 6, s + 12, s + 12, r + 6, ring, ring, 3);
        }
    }
    if (has_left_arrow) gfx_text(F_XL, arrow_left[0] + 8, arrow_left[1] + 4, "<", WHITE_A(220));
    if (has_right_arrow) gfx_text(F_XL, arrow_right[0] + 8, arrow_right[1] + 4, ">", WHITE_A(220));
    const Tile *st = &tiles[tab][ti];
    int ty = cyc + (base + extra) / 2 + 28;
    gfx_text(F_LG, left, ty, st->name, 0xFFFFFFFF);
    gfx_text(F_MD, left, ty + 42, st->sub, WHITE_A(160));

    /* live info card */
    int cy = ty + 104, ch = 128, cw = W - left * 2;
    if (cy + ch < H - 70) {
        gfx_rrect(left, cy, cw, ch, 26, WHITE_A(22), WHITE_A(12), 0);
        gfx_rrect(left, cy, cw, ch, 26, WHITE_A(36), WHITE_A(36), 1);
        int notes = 0, kb = 0; int nd = fs_lookup("/notes");
        for (int c = fs_child(nd, -1); c >= 0; c = fs_child(nd, c)) notes++;
        for (int i = 1; i < FS_MAX; i++) if (fs_nodes[i].used && fs_nodes[i].type == FS_FILE) kb += (int)fs_nodes[i].size;
        char v[4][32], t[16]; int up = (int)(ticks() / 1000);
        kitoa(v[0], notes);
        kitoa(v[1], kb / 1024); kstrcat(v[1], " KB", 32);
        kitoa(v[2], up / 60); kstrcat(v[2], "m ", 32); kitoa(t, up % 60); kstrcat(v[2], t, 32); kstrcat(v[2], "s", 32);
        kitoa(v[3], W); kstrcat(v[3], " x ", 32); kitoa(t, H); kstrcat(v[3], t, 32);
        const char *lab[4] = { "NOTES", "FILES SIZE", "UPTIME", "DISPLAY" };
        for (int i = 0; i < 4; i++) {
            int cx = left + 36 + i * (cw - 72) / 4;
            gfx_text(F_SM, cx, cy + 30, lab[i], WHITE_A(130));
            gfx_text(F_LG, cx, cy + 56, v[i], 0xFFFFFFFF);
        }
    }
}

static void draw_app_panel(void) {
    int f = open_anim.cur >> 8;                                    /* 0..256 */
    int px = 56, py = 96 + imax(0, (H - 720) / 4), pw = W - 112, ph = H - py - 84;
    int x = open_from[0] + (((px - open_from[0]) * f) >> 8), y = open_from[1] + (((py - open_from[1]) * f) >> 8);
    int w = open_from[2] + (((pw - open_from[2]) * f) >> 8), h = open_from[3] + (((ph - open_from[3]) * f) >> 8);
    const Tile *t = &tiles[tab][sel[tab]];
    gfx_shadow(x, y + 14, w, h, 30, 28, 130);
    if (f < 250) {
        u32 a = (u32)(255 - (f * 40 >> 8));
        gfx_rrect(x, y, w, h, 30, col_alpha(col_lerp(t->c1, 0xFF141A2C, f), a), col_alpha(col_lerp(t->c2, 0xFF0C1020, f), a), 0);
        return;
    }
    gfx_rrect(x, y, w, h, 30, ARGB(244, 22, 28, 46), ARGB(250, 12, 16, 30), 0);
    gfx_rrect(x, y, w, h, 30, WHITE_A(40), WHITE_A(40), 1);
    ui_icon(apps[cur_app].icon, x + 30, y + 20, 40, 255);
    gfx_text(F_LG, x + 86, y + 21, apps[cur_app].title, 0xFFFFFFFF);
    int hx = x + w - 30;
    int kw = gfx_text_w(F_SM, "Esc", -1) + 18; hx -= kw;
    gfx_rrect(hx, y + 28, kw, 26, 8, WHITE_A(24), WHITE_A(24), 0);
    gfx_rrect(hx, y + 28, kw, 26, 8, WHITE_A(110), WHITE_A(110), 1);
    gfx_text_c(F_SM, hx + kw / 2, y + 31, "Esc", 0xFFFFFFFF);
    gfx_text_r(F_SM, hx - 12, y + 31, "Back", WHITE_A(170));
    if (ui_click_in(hx - 70, y + 20, kw + 70, 42)) ui_close_app();
    gfx_fill(x + 30, y + 76, w - 60, 1, WHITE_A(30));
    gfx_clip(x + 4, y + 82, w - 8, h - 86);
    apps[cur_app].draw(x + 30, y + 92, w - 60, h - 92 - 26);
    gfx_noclip();
}

/* ============================== screenshots (F12) ============================== */
static int shot_pending; static u32 shot_last;
static void two(char *o, int v) { o[0] = (char)('0' + (v / 10) % 10); o[1] = (char)('0' + v % 10); }
static void take_screenshot(void) {                              /* g_bb holds the finished frame (no cursor, no toast) */
    int h, m, sec, d, mo, y;
    rtc_read(&h, &m, &sec, &d, &mo, &y);
    char name[40];                                               /* shot-YYYYMMDD-HHMMSS.png (file names are limited to 27 characters) */
    kstrcpy(name, "shot-", sizeof name);
    char t[8]; kitoa(t, y); kstrcat(name, t, sizeof name);
    char b[3]; b[2] = 0;
    two(b, mo); kstrcat(name, b, sizeof name); two(b, d); kstrcat(name, b, sizeof name);
    kstrcat(name, "-", sizeof name);
    two(b, h); kstrcat(name, b, sizeof name); two(b, m); kstrcat(name, b, sizeof name); two(b, sec); kstrcat(name, b, sizeof name);
    char path[64];
    kstrcpy(path, "/images/", sizeof path); kstrcat(path, name, sizeof path); int base = (int)strlen(path);
    kstrcat(path, ".png", sizeof path);
    for (int k = 2; k < 10 && fs_lookup(path) >= 0; k++) {       /* two shots in one second: add -2, -3 ... */
        path[base] = 0; kstrcat(path, "-", sizeof path); char sfx[2] = { (char)('0' + k), 0 }; kstrcat(path, sfx, sizeof path); kstrcat(path, ".png", sizeof path);
    }
    if (fs_create("/images", FS_DIR) < 0) { ui_flash("Screenshot failed: /images is not a folder"); return; }
    int cap = png_max_size(W, H);
    u8 *buf = kmalloc((u32)cap);
    if (!buf) { ui_flash("Screenshot failed: out of memory"); return; }
    int n = png_encode(g_bb, W, H, buf, cap);
    if (n <= 0) { kfree(buf); ui_flash("Screenshot failed: could not encode"); return; }
    int node = fs_create(path, FS_FILE);
    if (node < 0 || fs_write(node, buf, n) < 0) {
        if (node >= 0) fs_delete(node);
        kfree(buf); ui_flash("Screenshot failed: not enough storage"); return;
    }
    kfree(buf);
    fs_save(); files_invalidate();
    char msg[64]; kstrcpy(msg, "Saved ", sizeof msg); kstrcat(msg, path, sizeof msg);
    ui_flash(msg);
}

static void render(void) {
    gfx_target(g_bb, W, H); gfx_noclip();
    int shot = shot_pending; shot_pending = 0;
    memcpy(g_bb, g_wall, (u32)W * H * 4);
    int saved_click = g_click;
    if (dlg.active) g_click = 0;                                    /* modal: swallow clicks behind it */
    int appf = cur_app >= 0 ? open_anim.cur >> 8 : 0;
    if (appf < 256) draw_home();
    if (cur_app >= 0) {
        gfx_fill(0, 0, W, H, BLACK_A(150 * appf >> 8));
        draw_app_panel();
    }
    if (dlg.active) { g_click = saved_click; draw_dialog(); }
    draw_hints();
    if (toast_until > ticks() && !shot) {
        int w = gfx_text_w(F_MDB, toast_msg, -1) + 48;
        gfx_rrect((W - w) / 2, H - 120, w, 44, 22, ARGB(235, 30, 36, 58), ARGB(235, 30, 36, 58), 0);
        gfx_rrect((W - w) / 2, H - 120, w, 44, 22, WHITE_A(50), WHITE_A(50), 1);
        gfx_text_c(F_MDB, W / 2, H - 120 + 10, toast_msg, 0xFFFFFFFF);
    }
    if (fade_anim.cur > 0) gfx_fill(0, 0, W, H, BLACK_A(fade_anim.cur >> 8));
    if (shot) { take_screenshot(); shot_last = ticks(); }
    gfx_blit_pm(cursor_bmp, 28, 28, mouse_x - 4, mouse_y - 3, 255);
    gfx_present(); frame_count++;
}

/* ============================== boot splash ============================== */
static void splash_frame(int alpha, int bar) {
    gfx_target(g_bb, W, H); gfx_noclip();
    gfx_vgrad(0, 0, W, H, 0xFF05070F, 0xFF010204);
    gfx_glow(W / 2, H / 2 - 20, 380, 0x3050C0, alpha * 70 >> 8);
    gfx_blit_scaled(logo_bmp, 128, 128, W / 2 - 64, H / 2 - 120, 128, 128, alpha);
    gfx_text_c(F_XL, W / 2, H / 2 + 28, "Ktuyome", col_alpha(0xFFFFFFFF, alpha));
    int bw = 220, bx = (W - bw) / 2, by = H / 2 + 120;
    gfx_rrect(bx, by, bw, 4, 2, WHITE_A(alpha * 40 >> 8), WHITE_A(alpha * 40 >> 8), 0);
    gfx_rrect(bx, by, imax(4, bw * bar / 256), 4, 2, col_alpha(0xFFFFFFFF, alpha), col_alpha(0xFFFFFFFF, alpha), 0);
    gfx_present();
}

/* ============================== input ============================== */
static void home_key(const Event *e) {
    switch (e->key) {
        case K_LEFT:  if (sel[tab] > 0) { sel[tab]--; home_scroll_to_sel = 1; } break;
        case K_RIGHT: if (sel[tab] < ntiles[tab] - 1) { sel[tab]++; home_scroll_to_sel = 1; } break;
        case K_UP:    tab = 0; home_scroll_to_sel = 1; break;
        case K_DOWN:  tab = 1; home_scroll_to_sel = 1; break;
        case K_TAB:   tab ^= 1; home_scroll_to_sel = 1; break;
        case K_ENTER: activate_tile(); break;
        case K_F10:   ui_dialog("Shut Down", "Turn off the system?", "Cancel", "Shut Down", DLG_SHUTDOWN); break;
        default: return;
    }
    sel_anim.tgt = sel[tab] * 65536; tab_anim.tgt = tab * 65536;
}
static void handle_event(const Event *e) {
    if (e->type == EV_WHEEL) { g_wheel += e->wheel; return; }
    if (e->type == EV_KEY && e->key == K_F12) {                     /* screenshot, works everywhere (held-key repeats are ignored) */
        if (!shot_pending && (shot_last == 0 || ticks() - shot_last > 700)) shot_pending = 1;
        return;
    }
    if (dlg.active) {
        if (e->type == EV_KEY) {
            if (e->key == K_LEFT || e->key == K_RIGHT || e->key == K_TAB) dlg.sel ^= 1;
            else if (e->key == K_ENTER) { if (dlg.sel) dialog_confirm(); else dlg.active = 0; }
            else if (e->key == K_ESC) dlg.active = 0;
        } else if (e->type == EV_DOWN && e->btn == 1) { g_click = 1; g_click_x = e->x; g_click_y = e->y; g_click_mods = e->mods; }
        return;
    }
    if (e->type == EV_DOWN && e->btn == 1) { g_click = 1; g_click_x = e->x; g_click_y = e->y; g_click_mods = e->mods; }
    if (g_state == ST_HOME && !closing && cur_app < 0) {
        if (e->type == EV_MOVE) {
            for (int i = 0; i < ntiles[tab]; i++) if (inside(e->x, e->y, tile_rc[i][0], tile_rc[i][1], tile_rc[i][2], tile_rc[i][3]) && sel[tab] != i) {
                sel[tab] = i; sel_anim.tgt = i * 65536; }
        } else if (e->type == EV_DOWN && e->btn == 1) {
            if (has_left_arrow && inside(e->x, e->y, arrow_left[0], arrow_left[1], arrow_left[2], arrow_left[3])) {
                home_scroll = imax(0, home_scroll - 120); home_scroll_to_sel = 0; g_click = 0; { char _b[16]; kitoa(_b, home_scroll); fs_write_text("/system/home.txt", _b, (int)strlen(_b)); } ui_wake(); return; }
            if (has_right_arrow && inside(e->x, e->y, arrow_right[0], arrow_right[1], arrow_right[2], arrow_right[3])) {
                home_scroll = imin(home_scroll_max, home_scroll + 120); home_scroll_to_sel = 0; g_click = 0; { char _b[16]; kitoa(_b, home_scroll); fs_write_text("/system/home.txt", _b, (int)strlen(_b)); } ui_wake(); return; }
            for (int i = 0; i < 2; i++) if (inside(e->x, e->y, tab_rc[i][0], tab_rc[i][1], tab_rc[i][2], tab_rc[i][3])) {
                tab = i; tab_anim.tgt = i * 65536; sel_anim.tgt = sel[tab] * 65536; home_scroll_to_sel = 1; g_click = 0; return; }
            for (int i = 0; i < ntiles[tab]; i++) if (inside(e->x, e->y, tile_rc[i][0], tile_rc[i][1], tile_rc[i][2], tile_rc[i][3])) {
                sel[tab] = i; sel_anim.tgt = i * 65536; home_scroll_to_sel = 1; g_click = 0; activate_tile(); return; }
        } else if (e->type == EV_KEY) home_key(e);
    } else if (cur_app >= 0 && !closing && e->type == EV_KEY) {
        if (!apps[cur_app].key(e) && e->key == K_ESC) ui_close_app();
    }
}

void panic_screen(const char *msg, u32 a, u32 b) {
    gfx_target(g_bb, W, H); gfx_noclip(); g_brightness = 100;
    gfx_vgrad(0, 0, W, H, 0xFF3A0B14, 0xFF12040A);
    gfx_text(F_XL, 96, 140, ":(", 0xFFFFFFFF);
    gfx_text(F_LG, 96, 250, "Ktuyome ran into a problem and has stopped.", 0xFFFFFFFF);
    char t[40], line[100]; line[0] = 0;
    kstrcat(line, msg, sizeof line); kstrcat(line, "  (vector ", sizeof line); kitoa(t, (int)a); kstrcat(line, t, sizeof line);
    kstrcat(line, ", eip ", sizeof line);
    static const char hx[] = "0123456789abcdef"; char h[11] = "0x00000000";
    for (int i = 0; i < 8; i++) h[2 + i] = hx[(b >> (28 - 4 * i)) & 15];
    kstrcat(line, h, sizeof line); kstrcat(line, ")", sizeof line);
    gfx_text(F_MD, 96, 306, line, WHITE_A(200));
    gfx_text(F_MD, 96, 340, "Please restart the computer.", WHITE_A(160));
    gfx_present();
}

/* ============================== kernel entry ============================== */
void kmain(u32 magic, u32 mbi) {
    if (magic != 0x2BADB002 || !gfx_init(mbi)) { for (;;) __asm__ volatile("cli; hlt"); }
    mouse_x = W / 2; mouse_y = H / 2;
    arch_init();
    render_static_icons();
    splash_frame(0, 0);
    apps_init();
    fs_init();
    settings_load(); settings_apply();
    ui_reload_icons();
    { char _b[32]; if (fs_read_text("/system/home.txt", _b, sizeof _b) >= 0) { home_scroll = katoi(_b); home_scroll_to_sel = 0; } }
    { char _b[16]; if (fs_read_text("/system/hometab.txt", _b, sizeof _b) >= 0) { tab = katoi(_b); if (tab < 0 || tab > 1) tab = 0; } }
    u32 t0 = ticks();
    for (;;) {                                                      /* splash: fade in, hold, fade out */
        u32 t = ticks() - t0;
        if (t > 1900) break;
        int a = t < 500 ? (int)(t * 255 / 500) : (t > 1500 ? (int)((1900 - t) * 255 / 400) : 255);
        splash_frame(iclamp(a, 0, 255), (int)(t * 256 / 1900));
        __asm__ volatile("hlt");
    }
    fade_anim.cur = 255 << 8; fade_anim.tgt = 0;
    open_anim.cur = open_anim.tgt = 0;
    tab_anim.cur = tab_anim.tgt = 0; sel_anim.cur = sel_anim.tgt = 0;
    int h = 0, m = 0, s = 0, d = 0, mo = 0, y = 0;
    rtc_read(&h, &m, &s, &d, &mo, &y); clk_h = h; clk_m = m; clk_s = s; clk_day = d; clk_mon = mo; clk_year = y;
    u32 last_frame = 0, last_anim = ticks(), last_sec = 0;
    for (;;) {
        u64 lt0 = rdtsc(), lidle = 0;
        Event e;
        while (ev_poll(&e)) { handle_event(&e); dirty = 1; }
        u32 now = ticks();
        while (now - last_anim >= 16) {
            last_anim += 16;
            anim_step(&sel_anim); anim_step(&tab_anim); anim_step(&open_anim);
            if (fade_anim.cur > 0) { fade_anim.cur -= 255 * 256 / 24; if (fade_anim.cur < 0) fade_anim.cur = 0; }
        }
        if (closing && open_anim.cur == 0) { closing = 0; cur_app = -1; g_state = ST_HOME; }
        int busy = anim_busy(&sel_anim) || anim_busy(&tab_anim) || anim_busy(&open_anim) || fade_anim.cur > 0;
        if (now / 1000 != last_sec) {
            last_sec = now / 1000;
            rtc_read(&h, &m, &s, &d, &mo, &y); clk_h = h; clk_m = m; clk_s = s; clk_day = d; clk_mon = mo; clk_year = y;
            dirty = 1;
        }
        int bl = (now / 530) & 1;
        if (bl != g_blink) { g_blink = bl; if (cur_app >= 0) dirty = 1; }
        if (toast_until && now > toast_until) { toast_until = 0; dirty = 1; }
        apps_tick();
        if ((dirty || busy) && now - last_frame >= 14) {
            dirty = 0; render(); g_click = 0; g_wheel = 0; last_frame = ticks();
            if (busy) dirty = 1;
        } else { u64 h0 = rdtsc(); __asm__ volatile("hlt"); lidle = rdtsc() - h0; }
        tm_account(cur_app, (rdtsc() - lt0) - lidle, lidle);
    }
}
