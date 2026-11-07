#define _FORTIFY_SOURCE 0
#include "ktuyome.h"
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define NDEBUG
#include "vendor/miniz/miniz.h"
#define tolower(c) (((c) >= 'A' && (c) <= 'Z') ? (c) + 32 : (c))

/* ============================== Files ============================== */
static int f_dir, f_ids[FS_MAX + 1], f_n, f_sel, f_scroll, f_prev = -2, f_prev_size = -1;
static char f_search[64]; static int f_searching;
static int f_selected[FS_MAX];
static Prompt f_pr; static u32 f_bmp[256 * 256];
static int f_pw, f_ph, f_pok;                                  /* PNG preview cache: source size, decoded ok */
static PngImage f_view; static int f_view_id = -1;            /* full-size PNG viewer (Enter on a .png) */
static int fc_node = -1, fc_parent, fc_cut; static char fc_name[28];      /* file clipboard: what Ctrl+C / Ctrl+X picked */

static int contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}
static void files_refresh(void) {
    if (f_dir < 0 || !fs_nodes[f_dir].used || fs_nodes[f_dir].type != FS_DIR) f_dir = 0;
    f_n = 0;
    if (f_dir != 0) f_ids[f_n++] = -2;                                  /* ".." */
    for (int pass = 0; pass < 2; pass++)                                /* folders first */
        for (int c = fs_child(f_dir, -1); c >= 0; c = fs_child(f_dir, c)) {
            if (f_search[0] && !contains_ci(fs_nodes[c].name, f_search)) continue;
            if ((fs_nodes[c].type == FS_DIR) == (pass == 0)) f_ids[f_n++] = c;
        }
    f_sel = iclamp(f_sel, 0, imax(0, f_n - 1)); f_prev = -2;
    memset(f_selected, 0, sizeof f_selected);
}
u32 files_static_bytes(void) { return (u32)(sizeof f_bmp + sizeof f_ids + sizeof f_pr); }
static void view_close(void) { png_free(&f_view); f_view_id = -1; }
void files_open(void) { view_close(); files_refresh(); }
void files_close(void) { view_close(); }
void files_invalidate(void) { files_refresh(); }
static void files_activate(int i) {
    if (i < 0 || i >= f_n) return;
    int id = f_ids[i];
    if (id == -2) { f_dir = fs_nodes[f_dir].parent; f_sel = 0; files_refresh(); return; }
    if (fs_nodes[id].type == FS_DIR) { f_dir = id; f_sel = 0; f_scroll = 0; files_refresh(); return; }
    char p[96]; fs_path(id, p, sizeof p);
    if (png_is(fs_nodes[id].name)) {                                    /* images open in the viewer */
        view_close();
        if (png_decode(fs_data(id), (int)fs_nodes[id].size, &f_view)) f_view_id = id;
        else ui_flash("Cannot open this PNG (unsupported or damaged)");
        return;
    }
    editor_open_path(p); ui_switch_app(APP_EDITOR);
}
static void files_up(void) { if (f_dir) { f_dir = fs_nodes[f_dir].parent; f_sel = 0; f_scroll = 0; files_refresh(); } }
static void files_delete_sel(void) {
    int any = 0;
    for (int i = 0; i < FS_MAX; i++) if (f_selected[i]) { any = 1; break; }
    if (!any) {
        if (f_sel < 0 || f_sel >= f_n || f_ids[f_sel] < 0) return;
        char msg[100]; kstrcpy(msg, "Delete \"", sizeof msg); kstrcat(msg, fs_nodes[f_ids[f_sel]].name, sizeof msg); kstrcat(msg, "\"? This cannot be undone.", sizeof msg);
        g_dlg_arg = f_ids[f_sel]; ui_dialog("Delete item", msg, "Cancel", "Delete", DLG_APP + 3);
        return;
    }
    /* delete all selected immediately */
    for (int i = 0; i < FS_MAX; i++) if (f_selected[i] && fs_nodes[i].used) { fs_delete(i); }
    memset(f_selected, 0, sizeof f_selected);
    fs_save(); files_refresh();
    ui_flash("Deleted selected");
}
void files_dialog(int action, int arg) {
    if (action == DLG_APP + 3) {
        char p[96]; fs_path(arg, p, sizeof p); int ic = starts_with(p, "/system/icons/");
        if (arg == f_view_id) view_close();
        fs_delete(arg); fs_save(); files_refresh(); if (ic) ui_reload_icons();
        ui_flash("Deleted");
    }
}
static void files_make(int folder, const char *name) {
    char p[110]; fs_path(f_dir, p, sizeof p);
    if (f_dir) kstrcat(p, "/", sizeof p);
    kstrcat(p, name, sizeof p);
    int n = fs_create(p, folder ? FS_DIR : FS_FILE);
    if (n < 0) { ui_flash("Could not create (name taken or storage full)"); return; }
    fs_save(); files_refresh();
    for (int i = 0; i < f_n; i++) if (f_ids[i] == n) f_sel = i;
    ui_flash(folder ? "Folder created" : "File created");
}
/* ---- copy / cut / paste ---- */
static int child_named(int dir, const char *name) {
    for (int c = fs_child(dir, -1); c >= 0; c = fs_child(dir, c)) if (!strcmp(fs_nodes[c].name, name)) return c;
    return -1;
}
static void unique_name(int dir, const char *base, char *out) {            /* "a.txt", then "a copy.txt"... kept within 27 characters */
    if (child_named(dir, base) < 0) { kstrcpy(out, base, 28); return; }
    int bl = (int)strlen(base), dot = bl;
    while (dot > 0 && base[dot - 1] != '.') dot--;
    int ext = dot > 1 ? bl - (dot - 1) : 0;                                /* length of ".ext" (0 when there is none or the name starts with the dot) */
    int stem = bl - ext;
    for (int i = 1; i < 100; i++) {
        char suf[16]; kstrcpy(suf, " copy", sizeof suf);
        if (i > 1) { char n[8]; kitoa(n, i); kstrcat(suf, " ", sizeof suf); kstrcat(suf, n, sizeof suf); }
        int room = 27 - (int)strlen(suf) - ext, sl = imin(stem, imax(1, room));
        char t[32]; memcpy(t, base, sl); t[sl] = 0;
        kstrcat(t, suf, sizeof t); if (ext) kstrcat(t, base + stem, sizeof t);
        if (child_named(dir, t) < 0) { kstrcpy(out, t, 28); return; }
    }
    kstrcpy(out, base, 28);
}
static int under(int n, int anc) { for (int c = n, g = 0; c >= 0 && g < 32; c = fs_nodes[c].parent, g++) if (c == anc) return 1; return 0; }
static int copy_tree(int src, int dstdir, const char *name, int depth) {   /* returns the new node or -1 (partial copies are removed) */
    if (depth > 8) return -1;
    char p[128]; fs_path(dstdir, p, sizeof p);
    if (dstdir) kstrcat(p, "/", sizeof p);
    kstrcat(p, name, sizeof p);
    int n = fs_create(p, fs_nodes[src].type);
    if (n < 0) return -1;
    if (fs_nodes[src].type == FS_FILE) {
        if (fs_nodes[src].size && fs_write(n, fs_data(src), (int)fs_nodes[src].size) < 0) { fs_delete(n); return -1; }
    } else {
        for (int c = fs_child(src, -1); c >= 0; c = fs_child(src, c))
            if (copy_tree(c, n, fs_nodes[c].name, depth + 1) < 0) { fs_delete(n); return -1; }
    }
    return n;
}
static void files_clip(int cut) {
    if (f_sel < 0 || f_sel >= f_n || f_ids[f_sel] < 0) { ui_flash("Select a file or folder first"); return; }
    int id = f_ids[f_sel]; char p[96];
    fc_node = id; fc_parent = fs_nodes[id].parent; fc_cut = cut; kstrcpy(fc_name, fs_nodes[id].name, sizeof fc_name);
    fs_path(id, p, sizeof p); clip_set(p, (int)strlen(p));                   /* the path is also on the text clipboard */
    ui_flash(cut ? "Cut: open a folder and press Ctrl+V to move it" : "Copied: open a folder and press Ctrl+V to paste it");
}
static void files_paste(void) {
    int src = fc_node;
    if (src <= 0 || !fs_nodes[src].used || fs_nodes[src].parent != fc_parent || strcmp(fs_nodes[src].name, fc_name)) { fc_node = -1; ui_flash("Nothing to paste: copy a file or folder first"); return; }
    if (fs_nodes[src].type == FS_DIR && under(f_dir, src)) { ui_flash("Can't paste a folder into itself"); return; }
    char name[28]; int n;
    if (fc_cut) {
        if (fc_parent == f_dir) { ui_flash("It is already in this folder"); return; }
        unique_name(f_dir, fs_nodes[src].name, name);
        kstrcpy(fs_nodes[src].name, name, 28); fs_nodes[src].parent = (s16)f_dir; n = src; fc_node = -1;
    } else {
        unique_name(f_dir, fs_nodes[src].name, name);
        n = copy_tree(src, f_dir, name, 0);
        if (n < 0) { ui_flash("Could not paste (storage full or too many items)"); return; }
    }
    fs_save(); files_refresh();
    { char dp[96]; fs_path(f_dir, dp, sizeof dp); if (starts_with(dp, "/system/icons")) ui_reload_icons(); }
    for (int i = 0; i < f_n; i++) if (f_ids[i] == n) f_sel = i;
    ui_flash(fc_node < 0 && fc_cut ? "Moved" : "Pasted");
}
static void files_zip_add_node(mz_zip_archive *za, int node, const char *prefix, int *ok) {
    if (node <= 0 || !fs_nodes[node].used) return;
    char arcname[256];
    if (prefix && prefix[0]) { kstrcpy(arcname, prefix, sizeof arcname); kstrcat(arcname, "/", sizeof arcname); kstrcat(arcname, fs_nodes[node].name, sizeof arcname); }
    else kstrcpy(arcname, fs_nodes[node].name, sizeof arcname);
    if (fs_nodes[node].type == FS_DIR) {
        char dirn[256]; kstrcpy(dirn, arcname, sizeof dirn); kstrcat(dirn, "/", sizeof dirn);
        if (!mz_zip_writer_add_mem(za, dirn, "", 0, MZ_DEFAULT_COMPRESSION)) { *ok = 0; return; }
        for (int c = fs_child(node, -1); c >= 0; c = fs_child(node, c)) {
            files_zip_add_node(za, c, arcname, ok);
            if (!*ok) return;
        }
    } else {
        const char *data = fs_data(node);
        int size = (int)fs_nodes[node].size;
        if (!mz_zip_writer_add_mem(za, arcname, data, (size_t)size, MZ_DEFAULT_COMPRESSION)) { *ok = 0; }
    }
}
static void files_zip_selected(void) {
    int ids[FS_MAX], ni = 0;
    for (int i = 0; i < FS_MAX; i++) if (f_selected[i] && fs_nodes[i].used) ids[ni++] = i;
    if (ni == 0) {
        if (f_sel >= 0 && f_sel < f_n && f_ids[f_sel] >= 0) ids[ni++] = f_ids[f_sel];
        else { ui_flash("Select files first"); return; }
    }
    char target[256]; char base[128]; fs_path(f_dir, base, sizeof base);
    char outname[32]; kstrcpy(outname, "selected.zip", sizeof outname);
    if (f_dir != 0) { kstrcpy(target, base, sizeof target); kstrcat(target, "/", sizeof target); kstrcat(target, outname, sizeof target); }
    else { kstrcpy(target, "/", sizeof target); kstrcat(target, outname, sizeof target); }
    if (fs_lookup(target) >= 0) { ui_flash("selected.zip already exists"); return; }
    mz_zip_archive za; mz_zip_zero_struct(&za);
    if (!mz_zip_writer_init_heap(&za, 0, 16384)) { ui_flash("Could not start zip"); return; }
    int ok = 1;
    for (int i = 0; i < ni; i++) {
        files_zip_add_node(&za, ids[i], "", &ok);
        if (!ok) break;
    }
    if (!ok) { mz_zip_writer_end(&za); ui_flash("Zip failed"); return; }
    void *buf = NULL; size_t sz = 0;
    if (!mz_zip_writer_finalize_heap_archive(&za, &buf, &sz)) { mz_zip_writer_end(&za); ui_flash("Zip finalize failed"); return; }
    mz_zip_writer_end(&za);
    int node = fs_create(target, FS_FILE);
    if (node < 0) { if (buf) kfree(buf); ui_flash("Could not create zip file"); return; }
    if (sz > 0 && buf) fs_write(node, buf, (int)sz);
    if (buf) kfree(buf);
    memset(f_selected, 0, sizeof f_selected);
    fs_save(); files_refresh();
    ui_flash("Zipped selected");
}
int files_key(const Event *e) {
    if (f_view.px) { view_close(); return 1; }                          /* any key closes the viewer */
    if (f_pr.active) {
        int r = pr_key(&f_pr, e);
        if (r == 1) files_make(f_pr.id == 2, f_pr.buf);
        return 1;
    }
    if (f_searching) {
        int len = (int)strlen(f_search);
        if (e->key == K_ESC) { f_searching = 0; f_search[0] = 0; files_refresh(); return 1; }
        if (e->key == K_ENTER) { f_searching = 0; return 1; }
        if (e->key == K_BS) { if (len > 0) { f_search[len - 1] = 0; files_refresh(); } return 1; }
        if (e->ch >= 32 && e->ch < 127) {
            if (len < (int)sizeof f_search - 1) { f_search[len] = (char)e->ch; f_search[len + 1] = 0; files_refresh(); }
            return 1;
        }
        return 1;
    }
    if ((e->mods & MOD_CTRL) && (e->key == 'f' || e->key == 'F')) { f_searching = 1; f_search[0] = 0; files_refresh(); return 1; }
    if (e->ch == '/') { f_searching = 1; f_search[0] = 0; files_refresh(); return 1; }
    int ck = clip_key(e);
    if (ck == CK_COPY) { files_clip(0); return 1; }
    if (ck == CK_CUT) { files_clip(1); return 1; }
    if (ck == CK_PASTE) { files_paste(); return 1; }
    if ((e->mods & MOD_CTRL) && e->key == 'n') { pr_open(&f_pr, "New file name", "", 1); return 1; }
    if (e->key == K_F7) { pr_open(&f_pr, "New folder name", "", 2); return 1; }
    switch (e->key) {
        case K_UP: if (f_sel > 0) f_sel--; return 1;
        case K_DOWN: if (f_sel < f_n - 1) f_sel++; return 1;
        case K_PGUP: f_sel = imax(0, f_sel - 8); return 1;
        case K_PGDN: f_sel = imin(f_n - 1, f_sel + 8); return 1;
        case K_HOME: f_sel = 0; return 1;
        case K_END: f_sel = imax(0, f_n - 1); return 1;
        case K_ENTER: files_activate(f_sel); return 1;
        case K_BS: case K_LEFT: if (f_dir) { files_up(); return 1; } return e->key == K_BS;
        case K_RIGHT: files_activate(f_sel); return 1;
        case K_DEL: files_delete_sel(); return 1;
    }
    if ((e->mods & MOD_CTRL) && e->key == ' ') {
        if (f_sel >= 0 && f_sel < f_n && f_ids[f_sel] >= 0) {
            int id = f_ids[f_sel]; f_selected[id] = !f_selected[id];
        }
        return 1;
    }
    return 0;
}
static void view_draw(int x, int y, int w, int h) {
    char t[48], n[16];
    gfx_rrect(x, y, w, 40, 12, WHITE_A(20), WHITE_A(20), 0);
    gfx_text(F_MD, x + 16, y + 8, (f_view_id > 0 && fs_nodes[f_view_id].used) ? fs_nodes[f_view_id].name : "", 0xFFFFFFFF);
    t[0] = 0; kitoa(n, f_view.w); kstrcat(t, n, sizeof t); kstrcat(t, " x ", sizeof t); kitoa(n, f_view.h); kstrcat(t, n, sizeof t);
    gfx_text_r(F_MD, x + w - 16, y + 8, t, WHITE_A(170));
    int top = y + 52, ah = h - 52 - 26, aw = w;
    ui_panel(x, top, aw, ah, WHITE_A(12));
    int mw = aw - 24, mh = ah - 24, dw = f_view.w, dh = f_view.h;
    if (dw > mw || dh > mh) {                                           /* shrink to fit, keep the aspect ratio */
        if (dw * mh > dh * mw) { dh = imax(1, dh * mw / dw); dw = mw; } else { dw = imax(1, dw * mh / dh); dh = mh; }
    }
    int dx = x + (aw - dw) / 2, dy = top + (ah - dh) / 2;
    for (int cy = 0; cy < dh; cy += 16) for (int cx = 0; cx < dw; cx += 16)
        gfx_fill(dx + cx, dy + cy, imin(16, dw - cx), imin(16, dh - cy), ((cx + cy) / 16) & 1 ? WHITE_A(30) : WHITE_A(14));
    gfx_blit_scaled(f_view.px, f_view.w, f_view.h, dx, dy, dw, dh, 255);
    gfx_text_r(F_SM, x + w - 6, y + h - 24, "Any key or click closes the viewer", WHITE_A(110));
    if (ui_click_in(x, y, w, h)) view_close();
}
void files_draw(int x, int y, int w, int h) {
    if (f_view.px) { view_draw(x, y, w, h); return; }
    char path[110]; fs_path(f_dir, path, sizeof path);
    char title[160]; kstrcpy(title, path, sizeof title);
    if (f_search[0] || f_searching) { kstrcat(title, "   search: ", sizeof title); kstrcat(title, f_search, sizeof title); if (f_searching) kstrcat(title, "_", sizeof title); }
    gfx_rrect(x, y, w - 760, 40, 12, WHITE_A(20), WHITE_A(20), 0);
    gfx_text(F_MD, x + 16, y + 8, title, 0xFFFFFFFF);
    int bx = x + w - 760;
    if (ui_btn(bx, y + 2, 140, 36, "New file", 1) && !f_pr.active) pr_open(&f_pr, "New file name", "", 1);
    if (ui_btn(bx + 150, y + 2, 160, 36, "New folder", 0) && !f_pr.active) pr_open(&f_pr, "New folder name", "", 2);
    if (ui_btn(bx + 320, y + 2, 148, 36, "Delete", 0) && !f_pr.active) files_delete_sel();
    if (ui_btn(bx + 478, y + 2, 120, 36, "Search", 0) && !f_pr.active) { f_searching = 1; f_search[0] = 0; files_refresh(); }
    if (ui_btn(bx + 606, y + 2, 136, 36, "Zip sel", 0) && !f_pr.active) files_zip_selected();
    int top = y + 54, ah = h - 54, lw = w * 58 / 100, rh = 42, rows = ah / rh;
    ui_panel(x, top, lw, ah, WHITE_A(12));
    if (g_wheel && ui_hover(x, top, lw, ah) && !f_pr.active) f_scroll = iclamp(f_scroll + g_wheel * 2, 0, imax(0, f_n - rows));
    if (f_sel < f_scroll) f_scroll = f_sel;
    if (f_sel >= f_scroll + rows) f_scroll = f_sel - rows + 1;
    gfx_clip(x, top, lw, ah);
    for (int i = f_scroll; i < f_n && i < f_scroll + rows; i++) {
        int ry = top + 4 + (i - f_scroll) * rh, id = f_ids[i], on = i == f_sel;
        if (on) gfx_rrect(x + 6, ry, lw - 12, rh - 4, 12, col_alpha(ACC, 120), col_alpha(ACC, 120), 0);
        else if (ui_hover(x + 6, ry, lw - 12, rh - 4)) gfx_rrect(x + 6, ry, lw - 12, rh - 4, 12, WHITE_A(18), WHITE_A(18), 0);
        const char *nm = id == -2 ? ".." : fs_nodes[id].name;
        int isdir = id == -2 || fs_nodes[id].type == FS_DIR;
        u32 c1 = isdir ? 0xFFFFC857 : (ends_with(nm, ".svg") ? 0xFF7AD7F0 : (png_is(nm) ? 0xFF9CE29B : 0xFFC9D1E3));
        gfx_rrect(x + 20, ry + 9, 26, 20, 5, c1, col_lerp(c1, 0xFF000000, 40), 0);
        if (isdir) gfx_rrect(x + 20, ry + 6, 12, 6, 2, c1, c1, 0);
        gfx_text(F_MD, x + 58, ry + 6, nm, 0xFFFFFFFF);
        if (id >= 0 && f_selected[id]) gfx_text(F_MD, x + lw - 60, ry + 6, "[x]", 0xFF9CE29B);
        if (!isdir) {
            char sz[24]; int s = (int)fs_nodes[id].size;
            if (s >= 1024) { kitoa(sz, s / 1024); kstrcat(sz, " KB", sizeof sz); } else { kitoa(sz, s); kstrcat(sz, " B", sizeof sz); }
            gfx_text_r(F_SM, x + lw - 22, ry + 9, sz, WHITE_A(150));
        }
        if (ui_click_in(x + 6, ry, lw - 12, rh - 4)) { if (f_sel == i) files_activate(i); else f_sel = i; }
    }
    gfx_noclip();
    /* preview pane */
    int px = x + lw + 14, pw = w - lw - 14;
    ui_panel(px, top, pw, ah, WHITE_A(12));
    if (f_sel >= 0 && f_sel < f_n) {
        int id = f_ids[f_sel];
        if (id >= 0) {
            Node *nd = &fs_nodes[id]; char p[96]; fs_path(id, p, sizeof p);
            gfx_text(F_MDB, px + 20, top + 16, nd->name, 0xFFFFFFFF);
            gfx_text(F_SM, px + 20, top + 44, p, WHITE_A(130));
            if (nd->type == FS_DIR) {
                int c = 0; for (int k = fs_child(id, -1); k >= 0; k = fs_child(id, k)) c++;
                char t[32]; kitoa(t, c); kstrcat(t, c == 1 ? " item" : " items", sizeof t);
                gfx_text(F_MD, px + 20, top + 80, t, WHITE_A(200));
            } else if (ends_with(nd->name, ".svg")) {
                if (f_prev != id || f_prev_size != (int)nd->size) { svg_render(fs_data(id), (int)nd->size, f_bmp, 256, 0xFFFFFFFF); f_prev = id; f_prev_size = (int)nd->size; }
                int s = imin(imin(pw - 40, ah - 110), 256), dx = px + (pw - s) / 2, dy = top + 76;
                for (int cy = 0; cy < s; cy += 16) for (int cx = 0; cx < s; cx += 16)
                    gfx_fill(dx + cx, dy + cy, imin(16, s - cx), imin(16, s - cy), ((cx + cy) / 16) & 1 ? WHITE_A(30) : WHITE_A(14));
                gfx_blit_scaled(f_bmp, 256, 256, dx, dy, s, s, 255);
            } else if (png_is(nd->name)) {
                if (f_prev != id || f_prev_size != (int)nd->size) {
                    PngImage im;
                    f_pok = png_decode(fs_data(id), (int)nd->size, &im);
                    if (f_pok) { f_pw = im.w; f_ph = im.h; png_fit(&im, f_bmp, 256); png_free(&im); }
                    f_prev = id; f_prev_size = (int)nd->size;
                }
                if (!f_pok) gfx_text(F_MD, px + 20, top + 80, "Unsupported or damaged PNG", WHITE_A(200));
                else {
                    int s = imin(imin(pw - 40, ah - 110), 256), dx = px + (pw - s) / 2, dy = top + 76;
                    for (int cy = 0; cy < s; cy += 16) for (int cx = 0; cx < s; cx += 16)
                        gfx_fill(dx + cx, dy + cy, imin(16, s - cx), imin(16, s - cy), ((cx + cy) / 16) & 1 ? WHITE_A(30) : WHITE_A(14));
                    gfx_blit_scaled(f_bmp, 256, 256, dx, dy, s, s, 255);
                    char t[40], nn[12]; t[0] = 0; kitoa(nn, f_pw); kstrcat(t, nn, sizeof t); kstrcat(t, " x ", sizeof t); kitoa(nn, f_ph); kstrcat(t, nn, sizeof t);
                    gfx_text_c(F_SM, px + pw / 2, dy + s + 10, t, WHITE_A(150));
                    gfx_text_c(F_SM, px + pw / 2, dy + s + 32, "Enter opens the viewer", WHITE_A(110));
                }
            } else {
                const char *d = fs_data(id); int n = (int)nd->size, off = 0;
                gfx_clip(px + 10, top + 70, pw - 20, ah - 80);
                for (int ln = 0; ln < 12 && off < n; ln++) {
                    int e = off; while (e < n && d[e] != '\n') e++;
                    gfx_text_n(F_MONO, px + 20, top + 78 + ln * 21, d + off, e - off, WHITE_A(210));
                    off = e + 1;
                }
                gfx_noclip();
            }
        } else gfx_text(F_MD, px + 20, top + 16, "Parent folder", WHITE_A(150));
    }
    gfx_text(F_SM, x + 4, y + h + 2, "", 0);
    if (f_pr.active) pr_draw(&f_pr, x, y, w, h);
}

/* ============================== Settings ============================== */
static int s_page, s_col, s_row, s_dirty, s_last;
static const char *s_pages[5] = { "Display", "Input", "Date and Time", "Storage", "About" };
static const int s_rows[5] = { 3, 1, 4, 2, 0 };
void settings_goto(int page) { s_page = iclamp(page, 0, 4); s_col = 0; s_row = 0; }
void set_open(void) {}
void set_close(void) { if (s_dirty) { settings_save(); s_dirty = 0; } }
void set_tick(void) { if (s_dirty && (int)ticks() - s_last > 900) { settings_save(); s_dirty = 0; } }
static void changed(void) { settings_apply(); s_dirty = 1; s_last = (int)ticks(); ui_wake(); }

static void adjust(int dir) {
    int h, m, s, d, mo, y;
    switch (s_page * 10 + s_row) {
        case 0: cfg.accent = (cfg.accent + dir + 5) % 5; break;
        case 1: cfg.wall = (cfg.wall + dir + 3) % 3; break;
        case 2: cfg.bright = iclamp(cfg.bright + dir * 5, 30, 100); break;
        case 10: cfg.mspeed = iclamp(cfg.mspeed + dir, 1, 4); break;
        case 20: cfg.h24 = !cfg.h24; break;
        case 21: cfg.secs = !cfg.secs; break;
        case 22: rtc_read(&h, &m, &s, &d, &mo, &y); rtc_set_time((h + dir + 24) % 24, m); break;
        case 23: rtc_read(&h, &m, &s, &d, &mo, &y); rtc_set_time(h, (m + dir + 60) % 60); break;
        default: return;
    }
    changed();
}
int set_key(const Event *e) {
    switch (e->key) {
        case K_UP: if (!s_col) { s_page = imax(0, s_page - 1); s_row = 0; } else s_row = imax(0, s_row - 1); return 1;
        case K_DOWN: if (!s_col) { s_page = imin(4, s_page + 1); s_row = 0; } else s_row = imin(imax(0, s_rows[s_page] - 1), s_row + 1); return 1;
        case K_RIGHT: if (!s_col) { if (s_rows[s_page]) s_col = 1; } else adjust(1); return 1;
        case K_LEFT: if (s_col) adjust(-1); return 1;
        case K_ENTER:
            if (!s_col) { if (s_rows[s_page]) s_col = 1; }
            else if (s_page == 2 && s_row < 2) adjust(1);
            else if (s_page == 3) { if (s_row == 0) { fs_save(); ui_flash("Saved to disk"); } else ui_dialog("Reset storage", "Erase all files and restore defaults?", "Cancel", "Reset", DLG_FORMAT); }
            return 1;
        case K_ESC: if (s_col) { s_col = 0; return 1; } return 0;
    }
    return 0;
}
static void label(int x, int y, const char *t) { gfx_text(F_MDB, x, y, t, 0xFFFFFFFF); }
static void focus_ring(int row, int x, int y, int w, int h) {
    if (s_col && s_row == row) gfx_rrect(x - 8, y - 8, w + 16, h + 16, 16, WHITE_A(200), WHITE_A(200), 2);
}
static void info_row(int x, int y, int w, const char *k, const char *v) {
    gfx_text(F_MD, x, y, k, WHITE_A(140));
    gfx_text(F_MD, x + 190, y, v, 0xFFFFFFFF); (void)w;
}
void set_draw(int x, int y, int w, int h) {
    int lw = 240;
    for (int i = 0; i < 5; i++) {
        int ry = y + i * 56, on = i == s_page;
        if (on) gfx_rrect(x, ry, lw, 48, 14, col_alpha(ACC, s_col ? 70 : 140), col_alpha(ACC, s_col ? 70 : 140), 0);
        else if (ui_hover(x, ry, lw, 48)) gfx_rrect(x, ry, lw, 48, 14, WHITE_A(20), WHITE_A(20), 0);
        gfx_text(F_MD, x + 20, ry + 11, s_pages[i], 0xFFFFFFFF);
        if (ui_click_in(x, ry, lw, 48)) { s_page = i; s_col = 0; s_row = 0; }
    }
    int cx = x + lw + 30, cw = w - lw - 30;
    ui_panel(cx - 10, y - 4, cw + 10, h + 8, WHITE_A(12));
    cx += 20; cw -= 40; int cy = y + 16;
    if (s_page == 0) {
        label(cx, cy, "Accent colour");
        for (int i = 0; i < 5; i++) {
            int sx = cx + 26 + i * 66, sy = cy + 70;
            gfx_circle(sx, sy, 22, accents[i]);
            if (i == cfg.accent) gfx_rrect(sx - 28, sy - 28, 56, 56, 28, 0xFFFFFFFF, 0xFFFFFFFF, 3);
            if (ui_click_in(sx - 26, sy - 26, 52, 52)) { cfg.accent = i; changed(); }
        }
        gfx_text(F_MD, cx + 360, cy + 56, accent_names[cfg.accent], WHITE_A(180));
        focus_ring(0, cx, cy + 36, 340, 68);
        cy += 140; label(cx, cy, "Wallpaper");
        static const u32 wt[3] = { 0xFF0B1A3C, 0xFF08171F, 0xFF1C0B12 }, wg[3] = { 0x2A5BD7, 0x1FD1A5, 0xFF5A36 };
        for (int i = 0; i < 3; i++) {
            int wx = cx + i * 196, wy = cy + 40;
            gfx_clip(wx, wy, 180, 104);
            gfx_rrect(wx, wy, 180, 104, 14, wt[i], 0xFF020305, 0);
            gfx_glow(wx + 140, wy + 20, 120, wg[i], 140);
            gfx_noclip();
            if (i == cfg.wall) gfx_rrect(wx - 5, wy - 5, 190, 114, 18, ACC, ACC, 3);
            gfx_text_c(F_SM, wx + 90, wy + 78, wall_names[i], 0xFFFFFFFF);
            if (ui_click_in(wx, wy, 180, 104)) { cfg.wall = i; changed(); }
        }
        focus_ring(1, cx, cy + 40, 580, 104);
        cy += 176; label(cx, cy, "Brightness");
        int nv = ui_slider(1, cx, cy + 42, 420, cfg.bright, 30, 100);
        if (nv != cfg.bright) { cfg.bright = nv; changed(); }
        char t[12]; kitoa(t, cfg.bright); kstrcat(t, "%", sizeof t);
        gfx_text(F_MD, cx + 444, cy + 40, t, WHITE_A(200));
        focus_ring(2, cx, cy + 40, 420, 28);
    } else if (s_page == 1) {
        label(cx, cy, "Pointer speed");
        int nv = ui_slider(2, cx, cy + 42, 420, cfg.mspeed, 1, 4);
        if (nv != cfg.mspeed) { cfg.mspeed = nv; changed(); }
        gfx_text(F_SM, cx, cy + 74, "Slow", WHITE_A(130)); gfx_text_r(F_SM, cx + 420, cy + 74, "Fast", WHITE_A(130));
        focus_ring(0, cx, cy + 40, 420, 28);
        cy += 130; label(cx, cy, "Devices");
        info_row(cx, cy + 44, cw, "Keyboard", "PS/2 keyboard (US layout) - connected");
        info_row(cx, cy + 80, cw, "Pointer", mouse_ok ? (mouse_has_wheel ? "PS/2 mouse with scroll wheel - connected" : "PS/2 mouse - connected") : "No PS/2 mouse detected");
        info_row(cx, cy + 116, cw, "Storage", disk_present() ? "ATA PIO disk - connected" : "No ATA disk detected");
    } else if (s_page == 2) {
        int hh, mm, ss, dd, mo, yy; rtc_read(&hh, &mm, &ss, &dd, &mo, &yy);
        char t[40], n[8]; t[0] = 0;
        kitoa(n, hh); if (hh < 10) kstrcat(t, "0", sizeof t); kstrcat(t, n, sizeof t); kstrcat(t, ":", sizeof t);
        kitoa(n, mm); if (mm < 10) kstrcat(t, "0", sizeof t); kstrcat(t, n, sizeof t); kstrcat(t, ":", sizeof t);
        kitoa(n, ss); if (ss < 10) kstrcat(t, "0", sizeof t); kstrcat(t, n, sizeof t);
        gfx_text(F_XL, cx, cy - 6, t, 0xFFFFFFFF);
        char dt[32]; kitoa(dt, yy); kstrcat(dt, "-", sizeof dt); if (mo < 10) kstrcat(dt, "0", sizeof dt); kitoa(n, mo); kstrcat(dt, n, sizeof dt);
        kstrcat(dt, "-", sizeof dt); if (dd < 10) kstrcat(dt, "0", sizeof dt); kitoa(n, dd); kstrcat(dt, n, sizeof dt);
        gfx_text(F_MD, cx + 300, cy + 20, dt, WHITE_A(170));
        cy += 100;
        label(cx, cy, "24-hour clock");     if (ui_toggle(cx + 400, cy - 2, cfg.h24)) { cfg.h24 = !cfg.h24; changed(); }
        focus_ring(0, cx + 400, cy - 2, 52, 28);
        cy += 56; label(cx, cy, "Show seconds"); if (ui_toggle(cx + 400, cy - 2, cfg.secs)) { cfg.secs = !cfg.secs; changed(); }
        focus_ring(1, cx + 400, cy - 2, 52, 28);
        cy += 64; label(cx, cy, "Set hour");
        if (ui_btn(cx + 300, cy - 6, 56, 36, "-", 0)) { rtc_set_time((hh + 23) % 24, mm); ui_wake(); }
        if (ui_btn(cx + 400, cy - 6, 56, 36, "+", 0)) { rtc_set_time((hh + 1) % 24, mm); ui_wake(); }
        gfx_text_c(F_MDB, cx + 378, cy, "", WHITE_A(0)); focus_ring(2, cx + 300, cy - 6, 156, 36);
        cy += 56; label(cx, cy, "Set minute");
        if (ui_btn(cx + 300, cy - 6, 56, 36, "-", 0)) { rtc_set_time(hh, (mm + 59) % 60); ui_wake(); }
        if (ui_btn(cx + 400, cy - 6, 56, 36, "+", 0)) { rtc_set_time(hh, (mm + 1) % 60); ui_wake(); }
        focus_ring(3, cx + 300, cy - 6, 156, 36);
    } else if (s_page == 3) {
        int used = fs_blocks_used(); char t[64], n[16];
        label(cx, cy, "Internal storage");
        gfx_rrect(cx, cy + 46, cw - 20, 18, 9, WHITE_A(40), WHITE_A(40), 0);
        gfx_rrect(cx, cy + 46, imax(18, (cw - 20) * used / FS_NBLK), 18, 9, ACC, ACC, 0);
        t[0] = 0; kitoa(n, used * 16); kstrcat(t, n, sizeof t); kstrcat(t, " KB used of ", sizeof t); kitoa(n, FS_NBLK * 16); kstrcat(t, n, sizeof t); kstrcat(t, " KB", sizeof t);
        gfx_text(F_MD, cx, cy + 76, t, WHITE_A(200));
        t[0] = 0; kitoa(n, fs_count_files()); kstrcat(t, n, sizeof t); kstrcat(t, " files", sizeof t);
        gfx_text(F_MD, cx, cy + 106, t, WHITE_A(140));
        if (fs_has_disk()) { t[0] = 0; kstrcat(t, "Persistent disk: ", sizeof t); kitoa(n, (int)(disk_sectors() / 2048)); kstrcat(t, n, sizeof t); kstrcat(t, " MB", sizeof t); }
        else kstrcpy(t, "No disk found - files are lost when the system powers off.", sizeof t);
        gfx_text(F_MD, cx, cy + 136, t, fs_has_disk() ? WHITE_A(140) : 0xFFFFC857);
        if (ui_btn(cx, cy + 190, 200, 46, "Save now", 1)) { fs_save(); ui_flash("Saved to disk"); }
        focus_ring(0, cx, cy + 190, 200, 46);
        if (ui_btn(cx + 220, cy + 190, 240, 46, "Reset storage...", 0)) ui_dialog("Reset storage", "Erase all files and restore defaults?", "Cancel", "Reset", DLG_FORMAT);
        focus_ring(1, cx + 220, cy + 190, 240, 46);
    } else {
        ui_logo(cx, cy, 96);
        gfx_text(F_LG, cx + 120, cy + 6, "Ktuyome " KT_VERSION, 0xFFFFFFFF);
        gfx_text(F_MD, cx + 120, cy + 48, "A small, fast 32-bit operating system", WHITE_A(150));
        char t[64], n[16];
        cy += 124;
        info_row(cx, cy, cw, "Kernel", "x86 (i386), Multiboot, monolithic");
        info_row(cx, cy + 36, cw, "Processor", cpu_brand());
        t[0] = 0; kitoa(n, (int)(mem_total_kb() / 1024)); kstrcat(t, n, sizeof t); kstrcat(t, " MB", sizeof t);
        info_row(cx, cy + 72, cw, "Memory", t);
        t[0] = 0; kitoa(n, W); kstrcat(t, n, sizeof t); kstrcat(t, " x ", sizeof t); kitoa(n, H); kstrcat(t, n, sizeof t); kstrcat(t, ", 32-bit colour", sizeof t);
        info_row(cx, cy + 108, cw, "Display", t);
        info_row(cx, cy + 144, cw, "Graphics", "Software renderer, SVG, PNG, antialiased text");
        t[0] = 0; int up = (int)(ticks() / 1000); kitoa(n, up / 60); kstrcat(t, n, sizeof t); kstrcat(t, " min ", sizeof t); kitoa(n, up % 60); kstrcat(t, n, sizeof t); kstrcat(t, " s", sizeof t);
        info_row(cx, cy + 180, cw, "Uptime", t);
    }
}
