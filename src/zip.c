#define _FORTIFY_SOURCE 0
#include "ktuyome.h"
#define tolower(c) (((c) >= 'A' && (c) <= 'Z') ? (c) + 32 : (c))
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "vendor/miniz/miniz.h"

/* ============================== Zip app ============================== */
static int z_dir, z_ids[FS_MAX + 1], z_n, z_sel, z_scroll;
static char z_search[64]; static int z_searching;

static int contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}
static void zip_refresh(void) {
    if (z_dir < 0 || !fs_nodes[z_dir].used || fs_nodes[z_dir].type != FS_DIR) z_dir = 0;
    z_n = 0;
    if (z_dir != 0) z_ids[z_n++] = -2;
    for (int pass = 0; pass < 2; pass++)
        for (int c = fs_child(z_dir, -1); c >= 0; c = fs_child(z_dir, c)) {
            if (z_search[0] && !contains_ci(fs_nodes[c].name, z_search)) continue;
            if ((fs_nodes[c].type == FS_DIR) == (pass == 0)) z_ids[z_n++] = c;
        }
    z_sel = iclamp(z_sel, 0, imax(0, z_n - 1));
}

void zip_open(void) { z_dir = 0; z_sel = 0; z_scroll = 0; zip_refresh(); }
void zip_close(void) {}

static void ensure_parents(const char *path) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last < 0) return;
    char tmp[256]; kstrcpy(tmp, path, sizeof tmp); tmp[last] = 0;
    char cur[256]; cur[0] = 0; int ci = 0;
    if (tmp[0] == '/') { cur[0] = '/'; cur[1] = 0; ci = 1; }
    for (int i = (tmp[0] == '/') ? 1 : 0; tmp[i]; i++) {
        if (ci < 255) cur[ci++] = tmp[i];
        cur[ci] = 0;
        if (tmp[i] == '/') {
            char dpath[256]; kstrcpy(dpath, cur, sizeof dpath);
            int l = (int)strlen(dpath);
            if (l > 1 && dpath[l - 1] == '/') dpath[l - 1] = 0;
            if (strcmp(dpath, "/") && dpath[0]) fs_create(dpath, FS_DIR);
        }
    }
    if (tmp[0] && strcmp(tmp, "/")) fs_create(tmp, FS_DIR);
}

static void zip_extract_sel(void) {
    if (z_sel < 0 || z_sel >= z_n) return;
    int id = z_ids[z_sel];
    if (id < 0) { ui_flash("Select a .zip file"); return; }
    if (!ends_with(fs_nodes[id].name, ".zip") && !ends_with(fs_nodes[id].name, ".ZIP")) {
        ui_flash("Select a .zip file"); return;
    }
    const char *data = fs_data(id);
    int size = (int)fs_nodes[id].size;
    mz_zip_archive za; mz_zip_zero_struct(&za);
    if (!mz_zip_reader_init_mem(&za, data, (size_t)size, 0)) { ui_flash("Not a valid zip archive"); return; }
    mz_uint n = mz_zip_reader_get_num_files(&za);
    char base[128]; fs_path(z_dir, base, sizeof base);
    int ok = 1;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { ok = 0; continue; }
        char target[256];
        kstrcpy(target, base, sizeof target);
        if (z_dir != 0) kstrcat(target, "/", sizeof target);
        kstrcat(target, st.m_filename, sizeof target);
        int isdir = st.m_is_directory;
        int len = (int)strlen(target);
        if (len > 0 && target[len - 1] == '/') { target[len - 1] = 0; isdir = 1; }
        if (isdir) {
            ensure_parents(target);
            fs_create(target, FS_DIR);
        } else {
            ensure_parents(target);
            size_t outsize = 0;
            void *buf = mz_zip_reader_extract_to_heap(&za, i, &outsize, 0);
            if (!buf && outsize > 0) { ok = 0; continue; }
            int node = fs_create(target, FS_FILE);
            if (node < 0) { if (buf) kfree(buf); ok = 0; continue; }
            if (outsize > 0 && buf) fs_write(node, buf, (int)outsize);
            if (buf) kfree(buf);
        }
    }
    mz_zip_reader_end(&za);
    fs_save();
    zip_refresh();
    ui_flash(ok ? "Extracted" : "Extracted with errors");
}

static void zip_add_node(mz_zip_archive *za, int node, const char *prefix, int *ok) {
    if (node <= 0 || !fs_nodes[node].used) return;
    char arcname[256];
    if (prefix && prefix[0]) { kstrcpy(arcname, prefix, sizeof arcname); kstrcat(arcname, "/", sizeof arcname); kstrcat(arcname, fs_nodes[node].name, sizeof arcname); }
    else kstrcpy(arcname, fs_nodes[node].name, sizeof arcname);
    if (fs_nodes[node].type == FS_DIR) {
        char dirn[256]; kstrcpy(dirn, arcname, sizeof dirn); kstrcat(dirn, "/", sizeof dirn);
        if (!mz_zip_writer_add_mem(za, dirn, "", 0, MZ_DEFAULT_COMPRESSION)) { *ok = 0; return; }
        for (int c = fs_child(node, -1); c >= 0; c = fs_child(node, c)) {
            zip_add_node(za, c, arcname, ok);
            if (!*ok) return;
        }
    } else {
        const char *data = fs_data(node);
        int size = (int)fs_nodes[node].size;
        if (!mz_zip_writer_add_mem(za, arcname, data, (size_t)size, MZ_DEFAULT_COMPRESSION)) { *ok = 0; }
    }
}

static void zip_zipup_sel(void) {
    if (z_sel < 0 || z_sel >= z_n) return;
    int id = z_ids[z_sel];
    if (id < 0) { ui_flash("Select a file or folder"); return; }
    char outname[40]; kstrcpy(outname, fs_nodes[id].name, sizeof outname); kstrcat(outname, ".zip", sizeof outname);
    char target[256]; char base[128]; fs_path(z_dir, base, sizeof base);
    if (z_dir != 0) { kstrcpy(target, base, sizeof target); kstrcat(target, "/", sizeof target); kstrcat(target, outname, sizeof target); }
    else { kstrcpy(target, "/", sizeof target); kstrcat(target, outname, sizeof target); }
    if (fs_lookup(target) >= 0) { ui_flash("That zip already exists"); return; }
    mz_zip_archive za; mz_zip_zero_struct(&za);
    if (!mz_zip_writer_init_heap(&za, 0, 16384)) { ui_flash("Could not start zip"); return; }
    int ok = 1;
    zip_add_node(&za, id, "", &ok);
    if (!ok) { mz_zip_writer_end(&za); ui_flash("Zip failed"); return; }
    void *buf = NULL; size_t sz = 0;
    if (!mz_zip_writer_finalize_heap_archive(&za, &buf, &sz)) { mz_zip_writer_end(&za); ui_flash("Zip finalize failed"); return; }
    mz_zip_writer_end(&za);
    int node = fs_create(target, FS_FILE);
    if (node < 0) { if (buf) kfree(buf); ui_flash("Could not create zip file"); return; }
    if (sz > 0 && buf) fs_write(node, buf, (int)sz);
    if (buf) kfree(buf);
    fs_save();
    zip_refresh();
    for (int i = 0; i < z_n; i++) if (z_ids[i] == node) z_sel = i;
    ui_flash("Zipped");
}

int zip_key(const Event *e) {
    if (z_searching) {
        int len = (int)strlen(z_search);
        if (e->key == K_ESC) { z_searching = 0; z_search[0] = 0; zip_refresh(); return 1; }
        if (e->key == K_ENTER) { z_searching = 0; return 1; }
        if (e->key == K_BS) { if (len > 0) { z_search[len - 1] = 0; zip_refresh(); } return 1; }
        if (e->ch >= 32 && e->ch < 127) {
            if (len < (int)sizeof z_search - 1) { z_search[len] = (char)e->ch; z_search[len + 1] = 0; zip_refresh(); }
            return 1;
        }
        return 1;
    }
    if ((e->mods & MOD_CTRL) && (e->key == 'f' || e->key == 'F')) { z_searching = 1; z_search[0] = 0; zip_refresh(); return 1; }
    if (e->ch == '/') { z_searching = 1; z_search[0] = 0; zip_refresh(); return 1; }
    switch (e->key) {
        case K_UP: if (z_sel > 0) z_sel--; return 1;
        case K_DOWN: if (z_sel < z_n - 1) z_sel++; return 1;
        case K_PGUP: z_sel = imax(0, z_sel - 8); return 1;
        case K_PGDN: z_sel = imin(z_n - 1, z_sel + 8); return 1;
        case K_HOME: z_sel = 0; return 1;
        case K_END: z_sel = imax(0, z_n - 1); return 1;
        case K_ENTER: {
            if (z_sel < 0 || z_sel >= z_n) return 1;
            int id = z_ids[z_sel];
            if (id == -2) { z_dir = fs_nodes[z_dir].parent; z_sel = 0; z_scroll = 0; zip_refresh(); return 1; }
            if (id >= 0 && fs_nodes[id].type == FS_DIR) { z_dir = id; z_sel = 0; z_scroll = 0; zip_refresh(); return 1; }
            zip_extract_sel(); return 1;
        }
        case K_BS: case K_LEFT:
            if (z_dir) { z_dir = fs_nodes[z_dir].parent; z_sel = 0; z_scroll = 0; zip_refresh(); }
            return 1;
    }
    if (e->ch == 'e' || e->ch == 'E') { zip_extract_sel(); return 1; }
    if (e->ch == 'z' || e->ch == 'Z') { zip_zipup_sel(); return 1; }
    return 0;
}

u32 zip_static_bytes(void) { return (u32)(sizeof z_ids); }

void zip_draw(int x, int y, int w, int h) {
    char path[128]; fs_path(z_dir, path, sizeof path);
    char title[200]; kstrcpy(title, path, sizeof title);
    if (z_search[0] || z_searching) { kstrcat(title, "   search: ", sizeof title); kstrcat(title, z_search, sizeof title); if (z_searching) kstrcat(title, "_", sizeof title); }
    gfx_rrect(x, y, w - 520, 40, 12, WHITE_A(20), WHITE_A(20), 0);
    gfx_text(F_MD, x + 16, y + 8, title, 0xFFFFFFFF);
    int bx = x + w - 508;
    if (ui_btn(bx, y + 2, 150, 36, "Extract", 1) ) zip_extract_sel();
    if (ui_btn(bx + 160, y + 2, 150, 36, "Zip up", 0)) zip_zipup_sel();
    if (ui_btn(bx + 320, y + 2, 120, 36, "Search", 0)) { z_searching = 1; z_search[0] = 0; zip_refresh(); }
    int top = y + 54, ah = h - 54, lw = w * 58 / 100, rh = 42, rows = ah / rh;
    ui_panel(x, top, lw, ah, WHITE_A(12));
    if (g_wheel && ui_hover(x, top, lw, ah)) z_scroll = iclamp(z_scroll + g_wheel * 2, 0, imax(0, z_n - rows));
    if (z_sel < z_scroll) z_scroll = z_sel;
    if (z_sel >= z_scroll + rows) z_scroll = z_sel - rows + 1;
    gfx_clip(x, top, lw, ah);
    for (int i = z_scroll; i < z_n && i < z_scroll + rows; i++) {
        int ry = top + 4 + (i - z_scroll) * rh, id = z_ids[i], on = i == z_sel;
        if (on) gfx_rrect(x + 6, ry, lw - 12, rh - 4, 12, col_alpha(ACC, 120), col_alpha(ACC, 120), 0);
        else if (ui_hover(x + 6, ry, lw - 12, rh - 4)) gfx_rrect(x + 6, ry, lw - 12, rh - 4, 12, WHITE_A(18), WHITE_A(18), 0);
        const char *nm = id == -2 ? ".." : fs_nodes[id].name;
        int isdir = id == -2 || (id >= 0 && fs_nodes[id].type == FS_DIR);
        int iszip = id >= 0 && ends_with(fs_nodes[id].name, ".zip");
        u32 c1 = isdir ? 0xFFFFC857 : (iszip ? 0xFF9CE29B : 0xFFC9D1E3);
        gfx_rrect(x + 20, ry + 9, 26, 20, 5, c1, col_lerp(c1, 0xFF000000, 40), 0);
        if (isdir) gfx_rrect(x + 20, ry + 6, 12, 6, 2, c1, c1, 0);
        gfx_text(F_MD, x + 58, ry + 6, nm, 0xFFFFFFFF);
        if (!isdir && id >= 0) {
            char sz[24]; int s = (int)fs_nodes[id].size;
            if (s >= 1024) { kitoa(sz, s / 1024); kstrcat(sz, " KB", sizeof sz); } else { kitoa(sz, s); kstrcat(sz, " B", sizeof sz); }
            gfx_text_r(F_SM, x + lw - 22, ry + 9, sz, WHITE_A(150));
        }
        if (ui_click_in(x + 6, ry, lw - 12, rh - 4)) { if (z_sel == i) { if (id == -2 || (id>=0 && fs_nodes[id].type==FS_DIR)) { /* open */ if (id==-2){ z_dir=fs_nodes[z_dir].parent; z_sel=0; z_scroll=0; zip_refresh(); } else { z_dir=id; z_sel=0; z_scroll=0; zip_refresh(); } } else zip_extract_sel(); } else z_sel = i; }
    }
    gfx_noclip();
    int px = x + lw + 14, pw = w - lw - 14;
    ui_panel(px, top, pw, ah, WHITE_A(12));
    gfx_text(F_MDB, px + 20, top + 16, "Zip archives", 0xFFFFFFFF);
    gfx_text(F_SM, px + 20, top + 48, "Enter or double-click a .zip to extract it", WHITE_A(150));
    gfx_text(F_SM, px + 20, top + 72, "Select a file/folder and press 'Z' to zip it up", WHITE_A(150));
    gfx_text(F_SM, px + 20, top + 96, "Select a .zip and press 'E' to extract it", WHITE_A(150));
    if (z_sel >= 0 && z_sel < z_n) {
        int id = z_ids[z_sel];
        if (id >= 0) {
            gfx_text(F_MD, px + 20, top + 130, fs_nodes[id].name, 0xFFFFFFFF);
            if (fs_nodes[id].type == FS_DIR) {
                int c = 0; for (int k = fs_child(id, -1); k >= 0; k = fs_child(id, k)) c++;
                char t[32]; kitoa(t, c); kstrcat(t, " items", sizeof t);
                gfx_text(F_SM, px + 20, top + 160, t, WHITE_A(150));
            } else if (ends_with(fs_nodes[id].name, ".zip")) {
                const char *data = fs_data(id);
                int size = (int)fs_nodes[id].size;
                mz_zip_archive za; mz_zip_zero_struct(&za);
                if (mz_zip_reader_init_mem(&za, data, (size_t)size, 0)) {
                    mz_uint n = mz_zip_reader_get_num_files(&za);
                    char t[48]; kstrcpy(t, "Entries: ", sizeof t); char nbuf[12]; kitoa(nbuf, n); kstrcat(t, nbuf, sizeof t);
                    gfx_text(F_SM, px + 20, top + 160, t, WHITE_A(150));
                    int yy = top + 186;
                    mz_uint show = n < 8 ? n : 8;
                    for (mz_uint i = 0; i < show; i++) {
                        char name[64]; mz_zip_reader_get_filename(&za, i, name, sizeof name);
                        gfx_text(F_SM, px + 20, yy, name, WHITE_A(200));
                        yy += 18;
                    }
                    if (n > show) gfx_text(F_SM, px + 20, yy, "...", WHITE_A(120));
                    mz_zip_reader_end(&za);
                } else {
                    gfx_text(F_SM, px + 20, top + 160, "Invalid archive", WHITE_A(150));
                }
            }
        }
    }
}