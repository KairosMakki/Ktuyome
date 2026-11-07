#include "ktuyome.h"
/* Task Manager: live CPU / memory / storage monitor and a process list.
 *
 * Ktuyome has no preemptive scheduler: one app is in the foreground and the rest keep their state resident.
 * The monitor therefore reports what is really measurable:
 *   - CPU load from the TSC: cycles spent outside `hlt` versus total cycles. Busy cycles are attributed to the
 *     foreground app (or to "System" while the home screen is showing).
 *   - Memory from the linker: the kernel image, including every statically allocated buffer, versus installed RAM.
 */

extern char _kend[];                                    /* end of .bss, set by linker.ld */
#define HIST 60
#define NPROC (APP_COUNT + 2)                           /* idle + system + one row per app */

/* ---- accounting (written by the main loop, sampled once a second) ---- */
static u64 acc_busy[APP_COUNT + 1], acc_idle;           /* index APP_COUNT = shell / home screen */
static u64 snap_busy[APP_COUNT + 1], snap_idle;
static u32 snap_ticks, snap_frames, started;

static u8  hist[HIST];                                  /* CPU % history, oldest first */
static int cpu10, mhz, fps;                             /* current load in tenths of a percent, TSC MHz, frames/s */
static int proc10[APP_COUNT + 1];                       /* per-owner share of all cycles, tenths of a percent */
static int idle10;

void tm_account(int app, u64 busy, u64 idle) {
    acc_busy[app >= 0 && app < APP_COUNT ? app : APP_COUNT] += busy;
    acc_idle += idle;
}

static void tm_sample(u32 now) {
    u32 dt = now - snap_ticks;
    u64 db[APP_COUNT + 1], di = acc_idle - snap_idle, total = di;
    for (int i = 0; i <= APP_COUNT; i++) { db[i] = acc_busy[i] - snap_busy[i]; total += db[i]; snap_busy[i] = acc_busy[i]; }
    snap_idle = acc_idle;
    if (total == 0 || dt == 0) return;
    u64 t = total >> 10;                                /* scale so every ratio fits in 32 bits */
    if (t == 0) t = 1;
    u64 busy = total - di;
    cpu10 = (int)iclamp((int)(((busy >> 10) * 1000) / t), 0, 1000);
    idle10 = 1000 - cpu10;
    for (int i = 0; i <= APP_COUNT; i++) proc10[i] = (int)iclamp((int)(((db[i] >> 10) * 1000) / t), 0, 1000);
    mhz = (int)(total / dt / 1000);                     /* cycles per ms / 1000 */
    u32 fr = ui_frames(); fps = (int)((fr - snap_frames) * 1000 / dt); snap_frames = fr;
    snap_ticks = now;
    for (int i = 0; i < HIST - 1; i++) hist[i] = hist[i + 1];
    hist[HIST - 1] = (u8)((cpu10 + 5) / 10);
}

void tm_tick(void) {
    u32 now = ticks();
    if (!started) { started = 1; snap_ticks = now; snap_frames = ui_frames(); return; }
    if (now - snap_ticks >= 1000) {
        tm_sample(now);
        if (ui_foreground_app() == APP_TASKMGR) ui_wake();
    }
}

/* ---- view state ---- */
static int page, psel = 2;
void tm_open(void) { ui_wake(); }
void tm_close(void) {}

int tm_key(const Event *e) {
    switch (e->key) {
        case K_TAB: case K_RIGHT: page = 1 - page; return 1;
        case K_LEFT: page = 1 - page; return 1;
        case K_UP: if (page) psel = imax(0, psel - 1); return 1;
        case K_DOWN: if (page) psel = imin(NPROC - 1, psel + 1); return 1;
        case K_ENTER:
            if (page && psel >= 2 && psel - 2 != APP_TASKMGR) ui_switch_app(psel - 2);
            return 1;
    }
    return 0;
}

/* ---- formatting helpers ---- */
static void cat_int(char *d, int v, int cap) { char t[16]; kitoa(t, v); kstrcat(d, t, cap); }
static void fmt_pct(char *d, int v10, int cap) {         /* tenths of a percent -> "12.3%" */
    d[0] = 0; cat_int(d, v10 / 10, cap); kstrcat(d, ".", cap); cat_int(d, v10 % 10, cap); kstrcat(d, "%", cap);
}
static void fmt_kb(char *d, u32 kb, int cap) {           /* kilobytes -> "12.3 MB" / "640 KB" */
    d[0] = 0;
    if (kb >= 1024) { cat_int(d, (int)(kb / 1024), cap); kstrcat(d, ".", cap); cat_int(d, (int)((kb % 1024) * 10 / 1024), cap); kstrcat(d, " MB", cap); }
    else { cat_int(d, (int)kb, cap); kstrcat(d, " KB", cap); }
}
static void card(int x, int y, int w, int h) {
    gfx_rrect(x, y, w, h, 18, WHITE_A(18), WHITE_A(10), 0);
    gfx_rrect(x, y, w, h, 18, WHITE_A(32), WHITE_A(32), 1);
}
static void bar(int x, int y, int w, int h, int val, int max, u32 col) {
    gfx_rrect(x, y, w, h, h / 2, WHITE_A(36), WHITE_A(36), 0);
    if (val > 0) gfx_rrect(x, y, imax(h, (int)((long long)w * val / imax(max, 1))), h, h / 2, col, col, 0);
}

/* ---- Performance page ---- */
static void mini_card(int x, int y, int w, int h, const char *lab, const char *val) {
    card(x, y, w, h);
    gfx_text(F_SM, x + 20, y + 14, lab, WHITE_A(140));
    gfx_text(F_LG, x + 20, y + 40, val, 0xFFFFFFFF);
}
static void draw_cpu(int x, int y, int w, int h) {
    card(x, y, w, h);
    char t[48];
    gfx_text(F_MDB, x + 22, y + 16, "CPU", 0xFFFFFFFF);
    fmt_pct(t, cpu10, sizeof t);
    gfx_text_r(F_XL, x + w - 22, y + 8, t, 0xFFFFFFFF);
    gfx_text_n(F_SM, x + 22, y + 46, cpu_brand(), (int)imin((int)strlen(cpu_brand()), 44), WHITE_A(140));
    int gx = x + 22, gy = y + 84, gw = w - 44, gh = h - 84 - 40;
    gfx_rrect(gx, gy, gw, gh, 10, BLACK_A(70), BLACK_A(70), 0);
    for (int q = 1; q < 4; q++) gfx_fill(gx + 6, gy + gh * q / 4, gw - 12, 1, WHITE_A(22));
    int cw = (gw - 12) / HIST, ox = gx + 6 + ((gw - 12) - cw * HIST) / 2, gh2 = gh - 8;
    for (int i = 0; i < HIST; i++) {
        int bh = hist[i] * gh2 / 100, bx = ox + i * cw, by = gy + 4 + gh2 - bh;
        if (bh > 0) {
            gfx_fill(bx, by, cw, bh, col_alpha(ACC, 90));
            gfx_fill(bx, by, cw, 2, ACC);
        }
    }
    int sum = 0; for (int i = 0; i < HIST; i++) sum += hist[i];
    t[0] = 0; kstrcat(t, "Last 60 seconds   avg ", sizeof t); cat_int(t, sum / HIST, sizeof t); kstrcat(t, "%", sizeof t);
    gfx_text(F_SM, gx, gy + gh + 10, t, WHITE_A(130));
    t[0] = 0; cat_int(t, mhz, sizeof t); kstrcat(t, " MHz", sizeof t);
    gfx_text_r(F_SM, gx + gw, gy + gh + 10, t, WHITE_A(130));
}
static void draw_mem(int x, int y, int w, int h) {
    card(x, y, w, h);
    u32 total = mem_total_kb();
    u32 kimg = (u32)((u32)_kend - 0x100000) / 1024;
    u32 fb = (u32)(sizeof g_bb + sizeof g_wall) / 1024, apps_kb = 0;
    for (int i = 0; i < APP_COUNT; i++) apps_kb += app_static_bytes(i) / 1024;
    u32 sys = kimg > fb + apps_kb ? kimg - fb - apps_kb : 0;
    sys += 1024;                                          /* first MB: BIOS, VGA and boot structures */
    u32 used = fb + apps_kb + sys; if (used > total) used = total;
    u32 freek = total - used;
    char t[48], n[24];
    gfx_text(F_MDB, x + 22, y + 16, "Memory", 0xFFFFFFFF);
    fmt_kb(n, used, sizeof n); t[0] = 0; kstrcat(t, n, sizeof t); kstrcat(t, " / ", sizeof t); fmt_kb(n, total, sizeof n); kstrcat(t, n, sizeof t);
    gfx_text_r(F_LG, x + w - 22, y + 14, t, 0xFFFFFFFF);
    int bx = x + 22, by = y + 66, bw = w - 44, bh = 26;
    gfx_rrect(bx, by, bw, bh, 13, WHITE_A(36), WHITE_A(36), 0);
    const u32 col[3] = { 0xFFFFB74D, 0xFF4DD0A1, 0 };
    u32 seg[3] = { sys, apps_kb, fb };
    u32 c2[3] = { 0xFFFFB74D, 0xFF4DD0A1, ACC };
    (void)col;
    gfx_clip(bx, by, bw, bh);
    int px = bx;
    for (int i = 0; i < 3; i++) {
        int sw = (int)((long long)bw * seg[i] / imax((int)total, 1));
        if (seg[i] && sw < 2) sw = 2;
        if (sw > 0) gfx_fill(px, by, sw, bh, c2[i]);
        px += sw;
    }
    gfx_noclip();
    gfx_rrect(bx, by, bw, bh, 13, WHITE_A(50), WHITE_A(50), 1);
    const char *lab[4] = { "System and reserved", "App data", "Display buffers", "Free" };
    u32 val[4] = { sys, apps_kb, fb, freek };
    u32 dot[4] = { c2[0], c2[1], c2[2], WHITE_A(70) };
    int rows = h > 200 ? 4 : 4, ry = by + 44;
    for (int i = 0; i < rows; i++) {
        int yy = ry + i * 30;
        gfx_circle(bx + 8, yy + 11, 6, dot[i]);
        gfx_text(F_MD, bx + 26, yy + 1, lab[i], WHITE_A(200));
        fmt_kb(n, val[i], sizeof n); gfx_text_r(F_MD, bx + bw, yy + 1, n, 0xFFFFFFFF);
    }
}
static void draw_perf(int x, int y, int w, int h) {
    int gap = 16, lw = w * 58 / 100, rw = w - lw - gap, top = h * 69 / 100, bot = h - top - gap;
    draw_cpu(x, y, lw, top);
    draw_mem(x + lw + gap, y, rw, top);
    char t[48], n[16];
    int mw = (lw - gap * 2) / 3, my = y + top + gap;
    int up = (int)(ticks() / 1000);
    t[0] = 0; cat_int(t, up / 3600, sizeof t); kstrcat(t, "h ", sizeof t); cat_int(t, up / 60 % 60, sizeof t); kstrcat(t, "m ", sizeof t); cat_int(t, up % 60, sizeof t); kstrcat(t, "s", sizeof t);
    mini_card(x, my, mw, bot, "UPTIME", t);
    t[0] = 0; cat_int(t, fps, sizeof t);
    mini_card(x + mw + gap, my, mw, bot, "FRAMES / SECOND", t);
    t[0] = 0; cat_int(t, W, sizeof t); kstrcat(t, " x ", sizeof t); cat_int(t, H, sizeof t);
    mini_card(x + 2 * (mw + gap), my, mw, bot, "DISPLAY", t);
    /* storage */
    int sx = x + lw + gap;
    card(sx, my, rw, bot);
    int used = fs_blocks_used();
    gfx_text(F_SM, sx + 20, my + 14, "STORAGE", WHITE_A(140));
    t[0] = 0; cat_int(t, used * 16, sizeof t); kstrcat(t, " / ", sizeof t); cat_int(t, FS_NBLK * 16, sizeof t); kstrcat(t, " KB", sizeof t);
    gfx_text_r(F_SM, sx + rw - 20, my + 14, t, WHITE_A(200));
    bar(sx + 20, my + 42, rw - 40, 14, used, FS_NBLK, ACC);
    t[0] = 0; cat_int(t, fs_count_files(), sizeof t); kstrcat(t, " files", sizeof t);
    if (fs_has_disk()) { kstrcat(t, "   disk ", sizeof t); cat_int(t, (int)(disk_sectors() / 2048), sizeof t); kstrcat(t, " MB", sizeof t); }
    else kstrcat(t, "   no disk", sizeof t);
    (void)n;
    gfx_text(F_SM, sx + 20, my + 66, t, fs_has_disk() ? WHITE_A(140) : 0xFFFFC857);
}

/* ---- Processes page ---- */
static void draw_procs(int x, int y, int w, int h) {
    (void)h;
    card(x, y, w, 36 + NPROC * 34 + 8);
    int cName = x + 24, cStat = x + w * 36 / 100, cCpu = x + w * 53 / 100, cMem = x + w * 82 / 100, cOpen = x + w - 24;
    gfx_text(F_SM, cName, y + 12, "NAME", WHITE_A(130));
    gfx_text(F_SM, cStat, y + 12, "STATUS", WHITE_A(130));
    gfx_text(F_SM, cCpu, y + 12, "CPU", WHITE_A(130));
    gfx_text_r(F_SM, cMem + 90, y + 12, "MEMORY", WHITE_A(130));
    gfx_text_r(F_SM, cOpen, y + 12, "OPENED", WHITE_A(130));
    gfx_fill(x + 16, y + 34, w - 32, 1, WHITE_A(26));
    int fg = ui_foreground_app();
    u32 kimg = (u32)((u32)_kend - 0x100000) / 1024;
    u32 apps_kb = 0; for (int i = 0; i < APP_COUNT; i++) apps_kb += app_static_bytes(i) / 1024;
    for (int r = 0; r < NPROC; r++) {
        int ry = y + 38 + r * 34;
        int app = r - 2, sel = r == psel;
        const char *name, *status; int cpu, ic = -1; u32 kb; int opens = -1;
        if (r == 0) { name = "System Idle"; status = "Halted (hlt)"; cpu = idle10; kb = 0; }
        else if (r == 1) { name = "System"; status = "Kernel and shell"; cpu = proc10[APP_COUNT]; kb = kimg > apps_kb ? kimg - apps_kb : 0; }
        else {
            name = apps[app].title; ic = apps[app].icon; cpu = proc10[app]; kb = app_static_bytes(app) / 1024; opens = ui_app_opens(app);
            status = app == fg ? "Foreground" : (opens > 0 ? "Resident" : "Not started");
        }
        if (sel) gfx_rrect(x + 10, ry, w - 20, 30, 10, col_alpha(ACC, 120), col_alpha(ACC, 120), 0);
        else if (ui_hover(x + 10, ry, w - 20, 30)) gfx_rrect(x + 10, ry, w - 20, 30, 10, WHITE_A(16), WHITE_A(16), 0);
        if (ui_click_in(x + 10, ry, w - 20, 30)) { psel = r; if (app >= 0 && app != APP_TASKMGR) ui_switch_app(app); return; }
        int nx = cName;
        if (ic >= 0) { ui_icon(ic, cName, ry + 3, 24, 255); nx += 36; }
        gfx_text(F_MD, nx, ry + 4, name, 0xFFFFFFFF);
        gfx_text(F_MD, cStat, ry + 4, status, app == fg && app >= 0 ? 0xFF8BE9B0 : WHITE_A(170));
        char t[24];
        fmt_pct(t, cpu, sizeof t); gfx_text(F_MD, cCpu, ry + 4, t, 0xFFFFFFFF);
        bar(cCpu + 76, ry + 11, 120, 8, cpu, 1000, ACC);
        if (kb) { char n[24]; fmt_kb(n, kb, sizeof n); gfx_text_r(F_MD, cMem + 90, ry + 4, n, 0xFFFFFFFF); }
        else gfx_text_r(F_MD, cMem + 90, ry + 4, "-", WHITE_A(110));
        if (opens >= 0) { t[0] = 0; cat_int(t, opens, sizeof t); gfx_text_r(F_MD, cOpen, ry + 4, t, WHITE_A(200)); }
        else gfx_text_r(F_MD, cOpen, ry + 4, "-", WHITE_A(110));
    }
}

void tm_draw(int x, int y, int w, int h) {
    const char *tabs[2] = { "Performance", "Processes" };
    int tx = x;
    for (int i = 0; i < 2; i++) {
        int tw = gfx_text_w(F_MDB, tabs[i], -1) + 40;
        if (i == page) gfx_rrect(tx, y, tw, 40, 20, col_alpha(ACC, 150), col_alpha(ACC, 150), 0);
        else if (ui_hover(tx, y, tw, 40)) gfx_rrect(tx, y, tw, 40, 20, WHITE_A(20), WHITE_A(20), 0);
        gfx_text_c(F_MDB, tx + tw / 2, y + 8, tabs[i], 0xFFFFFFFF);
        if (ui_click_in(tx, y, tw, 40)) page = i;
        tx += tw + 10;
    }
    gfx_text_r(F_SM, x + w, y + 11, page ? "Tab switch view   Up/Down select   Enter switch to app" : "Tab switch view", WHITE_A(120));
    if (page) draw_procs(x, y + 54, w, h - 54);
    else draw_perf(x, y + 54, w, h - 54);
}
