#include "ktuyome.h"

#define PW 320
#define PH 240
static u32 canvas[PW*PH];
static int paint_color = 0xFF000000 + 0xFF0000; /* red */
static int paint_tool = 0; /* 0 brush, 1 erase */
static int paint_size = 4;

void paint_open(void) {
    for (int i = 0; i < PW*PH; i++) canvas[i] = 0xFFFFFFFF; /* white */
}
void paint_close(void) {}

int paint_key(const Event *e) {
    if (e->ch == 'b') paint_tool = 0;
    if (e->ch == 'e') paint_tool = 1;
    if (e->ch == 's') {
        /* export */
        int cap = png_max_size(PW, PH);
        u8 *buf = (u8*)kmalloc(cap);
        if (!buf) { ui_flash("No memory"); return 1; }
        int n = png_encode(canvas, PW, PH, buf, cap);
        if (n > 0) {
            /* find unique filename */
            char path[64]; int idx=1;
            for (;;) {
                kstrcpy(path, "/images/paint", sizeof path);
                char num[8]; kitoa(num, idx); kstrcat(path, num, sizeof path); kstrcat(path, ".png", sizeof path);
                if (fs_lookup(path) < 0) break;
                idx++;
            }
            int node = fs_create(path, FS_FILE);
            if (node >= 0) { fs_write(node, buf, n); fs_save(); ui_flash("Saved PNG"); }
            else ui_flash("Could not save");
        } else ui_flash("Encode failed");
        kfree(buf);
        return 1;
    }
    if (e->key == K_UP) paint_size = iclamp(paint_size+1,1,20);
    if (e->key == K_DOWN) paint_size = iclamp(paint_size-1,1,20);
    return 0;
}

u32 paint_static_bytes(void) { return (u32)sizeof canvas; }

void paint_draw(int x, int y, int w, int h) {
    gfx_rrect(x, y, w, 40, 12, WHITE_A(20), WHITE_A(20), 0);
    gfx_text(F_MD, x+16, y+8, "Paint  (b brush, e erase, s save PNG, arrows size)", 0xFFFFFFFF);
    int cx = x + (w - PW)/2, cy = y + 50;
    /* draw canvas */
    for (int yy = 0; yy < PH; yy++)
        for (int xx = 0; xx < PW; xx++)
            g_bb[(cy+yy)*W + (cx+xx)] = canvas[yy*PW+xx];
    /* painting with mouse */
    if (mouse_btn & 1) {
        int mx = mouse_x - cx, my = mouse_y - cy;
        if (mx >= 0 && mx < PW && my >= 0 && my < PH) {
            for (int dy = -paint_size; dy <= paint_size; dy++)
                for (int dx = -paint_size; dx <= paint_size; dx++) {
                    int px = mx+dx, py = my+dy;
                    if (px>=0 && px<PW && py>=0 && py<PH)
                        canvas[py*PW+px] = (paint_tool==1) ? 0xFFFFFFFF : paint_color;
                }
        }
    }
    /* color swatches */
    u32 cols[6] = {0xFF000000,0xFFFF0000,0xFF00FF00,0xFF0000FF,0xFFFFFF00,0xFF00FFFF};
    for (int i = 0; i < 6; i++) {
        int sx = x + 20 + i*40, sy = y + 50 + PH + 20;
        gfx_rrect(sx, sy, 32, 32, 8, cols[i], cols[i], 0);
        if (ui_click_in(sx, sy, 32, 32)) paint_color = cols[i];
    }
    gfx_text(F_SM, x+20, y+50+PH+60, paint_tool==0?"Tool: brush":"Tool: erase", WHITE_A(150));
}