#include "ktuyome.h"

static int v_dir, v_ids[FS_MAX+1], v_n, v_sel, v_loaded = -1;
static PngImage v_img; static int v_zoom = 100, v_panx, v_pany;

static void viewer_refresh(void) {
    if (v_dir < 0 || !fs_nodes[v_dir].used || fs_nodes[v_dir].type != FS_DIR) v_dir = 0;
    v_n = 0;
    int imgdir = fs_lookup("/images");
    if (v_dir != 0 && v_dir != imgdir) v_ids[v_n++] = -2;
    for (int c = fs_child(v_dir, -1); c >= 0; c = fs_child(v_dir, c)) {
        if (fs_nodes[c].type == FS_FILE && png_is(fs_nodes[c].name)) v_ids[v_n++] = c;
    }
    v_sel = iclamp(v_sel, 0, imax(0, v_n-1));
}

void viewer_open(void) {
    int imgdir = fs_lookup("/images");
    if (imgdir < 0) { fs_create("/images", FS_DIR); imgdir = fs_lookup("/images"); }
    v_dir = imgdir >= 0 ? imgdir : 0;
    v_sel = 0; v_zoom = 100; v_panx = v_pany = 0; viewer_refresh();
}
void viewer_close(void) { png_free(&v_img); v_loaded = -1; }

int viewer_key(const Event *e) {
    switch (e->key) {
        case K_UP:
            if (e->mods & MOD_CTRL) { v_pany -= 20; return 1; }
            if (v_sel > 0) v_sel--; return 1;
        case K_DOWN:
            if (e->mods & MOD_CTRL) { v_pany += 20; return 1; }
            if (v_sel < v_n-1) v_sel++; return 1;
        case K_ENTER: {
            if (v_sel < 0 || v_sel >= v_n) return 1;
            int id = v_ids[v_sel];
            if (id == -2) {
                int imgdir = fs_lookup("/images");
                if (v_dir != imgdir && v_dir != 0) { v_dir = fs_nodes[v_dir].parent; v_sel=0; viewer_refresh(); }
                return 1;
            }
            if (id >= 0) {
                png_free(&v_img); v_loaded = -1;
                if (png_decode(fs_data(id), (int)fs_nodes[id].size, &v_img)) v_loaded = id;
                else ui_flash("Cannot open image");
            }
            return 1;
        }
        case K_BS: if (v_dir) { v_dir = fs_nodes[v_dir].parent; v_sel=0; viewer_refresh(); } return 1;
        case '+': case '=': v_zoom = iclamp(v_zoom + 10, 10, 400); return 1;
        case '-': v_zoom = iclamp(v_zoom - 10, 10, 400); return 1;
        case K_LEFT: if (e->mods & MOD_CTRL) { v_panx -= 20; return 1; } { int imgdir = fs_lookup("/images"); if (v_dir && v_dir != imgdir) { v_dir = fs_nodes[v_dir].parent; v_sel=0; viewer_refresh(); } } return 1;
        case K_RIGHT: if (e->mods & MOD_CTRL) { v_panx += 20; return 1; } return 1;
    }
    if (e->type == EV_WHEEL) { v_zoom = iclamp(v_zoom + (e->wheel > 0 ? 10 : -10), 10, 400); return 1; }
    return 0;
}

u32 viewer_static_bytes(void) { return (u32)(sizeof v_ids + sizeof v_img.px); }

void viewer_draw(int x, int y, int w, int h) {
    char path[128]; fs_path(v_dir, path, sizeof path);
    gfx_rrect(x, y, w, 40, 12, WHITE_A(20), WHITE_A(20), 0);
    gfx_text(F_MD, x+16, y+8, path, 0xFFFFFFFF);
    int top = y+54, ah = h-54, lw = w*40/100, rh=42, rows=ah/rh;
    ui_panel(x, top, lw, ah, WHITE_A(12));
    /* list */
    int scroll = 0;
    if (v_sel >= rows) scroll = v_sel - rows + 1;
    gfx_clip(x, top, lw, ah);
    for (int i = scroll; i < v_n && i < scroll+rows; i++) {
        int ry = top + 4 + (i-scroll)*rh, id = v_ids[i], on = i==v_sel;
        if (on) gfx_rrect(x+6, ry, lw-12, rh-4, 12, col_alpha(ACC,120), col_alpha(ACC,120), 0);
        const char *nm = id==-2 ? ".." : fs_nodes[id].name;
        gfx_text(F_MD, x+20, ry+6, nm, 0xFFFFFFFF);
        if (ui_click_in(x+6, ry, lw-12, rh-4)) { if (v_sel==i) { /* open */ if (id==-2){ v_dir=fs_nodes[v_dir].parent; v_sel=0; viewer_refresh(); } else if (id>=0){ png_free(&v_img); v_loaded=-1; if (png_decode(fs_data(id),(int)fs_nodes[id].size,&v_img)) v_loaded=id; else ui_flash("Cannot open"); } } else v_sel=i; }
    }
    gfx_noclip();
    int px = x+lw+14, pw = w-lw-14;
    ui_panel(px, top, pw, ah, WHITE_A(12));
    if (v_loaded >= 0 && v_img.px) {
        int dw = v_img.w * v_zoom / 100, dh = v_img.h * v_zoom / 100;
        if (dw < 1) dw = 1; if (dh < 1) dh = 1;
        int dx = px + (pw - dw)/2 + v_panx, dy = top + (ah - dh)/2 + v_pany;
        gfx_blit_scaled(v_img.px, v_img.w, v_img.h, dx, dy, dw, dh, 255);
        char t[40], n[12]; t[0]=0; kitoa(n, v_img.w); kstrcat(t,n,sizeof t); kstrcat(t," x ",sizeof t); kitoa(n,v_img.h); kstrcat(t,n,sizeof t);
        gfx_text_c(F_SM, px+pw/2, top+ah-20, t, WHITE_A(150));
    } else {
        gfx_text_c(F_MD, px+pw/2, top+ah/2, "Select a PNG and press Enter", WHITE_A(150));
    }
    gfx_text(F_SM, x+4, y+h-24, "+/- zoom  arrows pan", WHITE_A(110));
}