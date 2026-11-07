#include "ktuyome.h"
/* KTFS: a small hierarchical filesystem. Files live in 16 KiB blocks; a file larger than one block takes a
 * run of contiguous blocks (its block count is always derived from its size, so the on-disk node format is
 * unchanged since 1.3). The whole tree is serialised to the primary ATA disk when saved. Without a disk it is RAM-only. */

Node fs_nodes[FS_MAX];
static u8 pool[FS_NBLK][FS_BLK] __attribute__((aligned(512)));
static u8 blk_used[FS_NBLK];
static int disk_ok;
#define KTFS_MAGIC 0x3153544Bu   /* "KTS1" */
#define NODE_SECTORS ((FS_MAX * sizeof(Node) + 511) / 512)

int fs_has_disk(void) { return disk_ok; }

static int need_blocks(u32 size) { return size ? (int)((size + FS_BLK - 1) / FS_BLK) : 1; }   /* a file that owns a block owns at least one */
static int blk_run(int n) {                                   /* claim n contiguous free blocks; first block or -1 */
    for (int i = 0, run = 0; i < FS_NBLK; i++) {
        run = blk_used[i] ? 0 : run + 1;
        if (run == n) { int s = i - n + 1; for (int k = s; k <= i; k++) blk_used[k] = 1; return s; }
    }
    return -1;
}
static void blk_release(int first, int n) { for (int k = first; k < first + n && k < FS_NBLK; k++) blk_used[k] = 0; }
static int blk_alloc(void) { return blk_run(1); }
int fs_blocks_used(void) { int n = 0; for (int i = 0; i < FS_NBLK; i++) n += blk_used[i]; return n; }
int fs_count_files(void) { int n = 0; for (int i = 0; i < FS_MAX; i++) if (fs_nodes[i].used && fs_nodes[i].type == FS_FILE) n++; return n; }

int fs_child(int dir, int prev) {
    for (int i = prev + 1; i < FS_MAX; i++)
        if (i && fs_nodes[i].used && fs_nodes[i].parent == dir) return i;
    return -1;
}
static int find_in(int dir, const char *name, int len) {
    for (int i = 1; i < FS_MAX; i++) {
        Node *n = &fs_nodes[i];
        if (n->used && n->parent == dir && (int)strlen(n->name) == len && memcmp(n->name, name, len) == 0) return i;
    }
    return -1;
}
int fs_lookup(const char *path) {
    int cur = 0;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char *s = path;
        while (*path && *path != '/') path++;
        cur = find_in(cur, s, (int)(path - s));
        if (cur < 0) return -1;
    }
    return cur;
}
int fs_create(const char *path, int type) {
    int ex = fs_lookup(path);
    if (ex >= 0) return fs_nodes[ex].type == type ? ex : -1;
    int len = (int)strlen(path), sl = len - 1;
    while (sl > 0 && path[sl] != '/') sl--;
    if (path[len - 1] == '/') return -1;
    char parent[96];
    kstrcpy(parent, path, sl + 1 > 96 ? 96 : sl + 1);
    if (sl == 0) kstrcpy(parent, "/", 96);
    int p = fs_lookup(parent);
    if (p < 0 || fs_nodes[p].type != FS_DIR) return -1;
    const char *name = path + sl + (path[sl] == '/' ? 1 : 0);
    if (!*name || strlen(name) > 27) return -1;
    for (int i = 1; i < FS_MAX; i++) if (!fs_nodes[i].used) {
        Node *n = &fs_nodes[i];
        memset(n, 0, sizeof *n);
        kstrcpy(n->name, name, 28); n->parent = (s16)p; n->type = (u8)type; n->used = 1; n->blk = -1;
        return i;
    }
    return -1;
}
int fs_write(int n, const void *data, int len) {
    Node *nd = &fs_nodes[n];
    if (n <= 0 || n >= FS_MAX || !nd->used || nd->type != FS_FILE) return -1;
    if (len < 0) len = 0;
    if (len > FS_NBLK * FS_BLK) len = FS_NBLK * FS_BLK;
    int want = need_blocks((u32)len);
    if (nd->blk >= 0) {
        int have = need_blocks(nd->size);
        if (want < have) { blk_release(nd->blk + want, have - want); }            /* shrink in place */
        else if (want > have) {                                                   /* grow: move to a bigger run */
            blk_release(nd->blk, have);
            int nb = blk_run(want);
            if (nb < 0) { for (int k = nd->blk; k < nd->blk + have; k++) blk_used[k] = 1; return -1; }
            nd->blk = nb;
        }
    } else if (len > 0) {
        nd->blk = blk_run(want);
        if (nd->blk < 0) return -1;
    }
    if (len > 0) memmove(pool[nd->blk], data, (u32)len);
    nd->size = (u32)len;
    return len;
}
const char *fs_data(int n) { return fs_nodes[n].blk >= 0 ? (const char *)pool[fs_nodes[n].blk] : ""; }
int fs_delete(int n) {
    if (n <= 0 || n >= FS_MAX || !fs_nodes[n].used) return -1;
    if (fs_nodes[n].type == FS_DIR) { int c; while ((c = fs_child(n, -1)) >= 0) fs_delete(c); }
    if (fs_nodes[n].blk >= 0) blk_release(fs_nodes[n].blk, need_blocks(fs_nodes[n].size));
    memset(&fs_nodes[n], 0, sizeof(Node));
    return 0;
}
void fs_path(int n, char *out, int cap) {
    if (n <= 0) { kstrcpy(out, "/", cap); return; }
    char tmp[128]; int stack[16], d = 0;
    for (int c = n; c > 0 && d < 16; c = fs_nodes[c].parent) stack[d++] = c;
    tmp[0] = 0;
    while (d--) { kstrcat(tmp, "/", sizeof tmp); kstrcat(tmp, fs_nodes[stack[d]].name, sizeof tmp); }
    kstrcpy(out, tmp, cap);
}
int fs_read_text(const char *path, char *out, int cap) {
    int n = fs_lookup(path);
    if (n < 0 || fs_nodes[n].type != FS_FILE) return -1;
    int len = imin((int)fs_nodes[n].size, cap - 1);
    memcpy(out, fs_data(n), len); out[len] = 0;
    return len;
}
int fs_write_text(const char *path, const char *s, int len) {
    int n = fs_create(path, FS_FILE);
    return n < 0 ? -1 : fs_write(n, s, len);
}

/* ---------------- persistence ---------------- */
void fs_save(void) {
    if (!disk_ok) return;
    u8 hdr[512]; memset(hdr, 0, 512);
    ((u32 *)hdr)[0] = KTFS_MAGIC; ((u32 *)hdr)[1] = 1;
    disk_write(0, hdr, 1);
    disk_write(1, fs_nodes, NODE_SECTORS);
    u32 lba = 1 + NODE_SECTORS;
    for (int i = 0; i < FS_MAX; i++) {
        Node *n = &fs_nodes[i];
        if (n->used && n->type == FS_FILE && n->size && n->blk >= 0) {
            int ns = (n->size + 511) / 512;
            if (lba + ns >= disk_sectors()) break;
            disk_write(lba, pool[n->blk], ns); lba += ns;
        }
    }
}
static int load_disk(void) {
    u8 hdr[512];
    if (!disk_read(0, hdr, 1) || ((u32 *)hdr)[0] != KTFS_MAGIC) return 0;
    if (!disk_read(1, fs_nodes, NODE_SECTORS)) return 0;
    if (!fs_nodes[0].used || fs_nodes[0].type != FS_DIR) return 0;
    u32 lba = 1 + NODE_SECTORS;
    for (int i = 0; i < FS_MAX; i++) {
        Node *n = &fs_nodes[i];
        if (!n->used) continue;
        n->blk = -1;
        if (n->type == FS_FILE && n->size) {
            if (n->size > (u32)(FS_NBLK * FS_BLK)) n->size = (u32)(FS_NBLK * FS_BLK);
            n->blk = blk_run(need_blocks(n->size));
            if (n->blk < 0) { n->size = 0; continue; }
            int ns = (n->size + 511) / 512;
            if (!disk_read(lba, pool[n->blk], ns)) return 0;
            lba += ns;
        }
    }
    return 1;
}

static const char *readme =
"Welcome to Ktuyome\n"
"=================\n\n"
"Home screen\n"
"  Arrow keys / mouse    move between tiles\n"
"  Enter / click         open the selected tile\n"
"  Tab                   switch between Apps and System\n"
"  Esc                   go back (closes the open app)\n\n"
"Apps\n"
"  Notes        quick notes, saved automatically\n"
"  Text Editor  Ctrl+S save, Ctrl+O open, Ctrl+N new, F5 SVG preview\n"
"  Calculator   mouse or keyboard\n"
"  Files        browse, create, delete, copy/cut/paste, preview SVG and PNG\n"
"  Settings     display, input, time, storage\n\n"
"Screenshots\n"
"  F12 (anywhere)  saves the screen as a PNG in /images\n"
"  Open it in Files: Enter on a .png shows it full size\n\n"
"Clipboard (works in every app)\n"
"  Ctrl+C / Ctrl+X / Ctrl+V   copy, cut, paste     Ctrl+A   select all\n"
"  Shift+arrows select text; drag with the mouse; double-click a word\n"
"  Files: Ctrl+C / Ctrl+X pick an item, Ctrl+V pastes it in the open folder\n\n"
"Downloads and FAT32 (2.1)\n"
"  Network app: wget <url> fetches a file over http or https\n"
"  and saves it to /downloads.\n"
"  Files: press FAT32 to browse a plain FAT32 disk attached\n"
"  as a second drive (QEMU: make runfat); Enter copies a\n"
"  file into the current KTFS folder.\n\n"
"Icons are plain SVG files in /system/icons. Edit one in the\n"
"Text Editor, press Ctrl+S, and the home screen updates at once.\n"
"A PNG with the same name (for example notes.png) replaces the SVG.\n";

static void make_defaults(void) {
    memset(fs_nodes, 0, sizeof fs_nodes); memset(blk_used, 0, sizeof blk_used);
    fs_nodes[0].used = 1; fs_nodes[0].type = FS_DIR; fs_nodes[0].parent = -1; fs_nodes[0].blk = -1;
    fs_create("/system", FS_DIR); fs_create("/system/icons", FS_DIR);
    fs_create("/notes", FS_DIR); fs_create("/docs", FS_DIR); fs_create("/images", FS_DIR); fs_create("/downloads", FS_DIR);
    char path[64];
    for (int i = 0; i < IC_COUNT; i++) {
        kstrcpy(path, "/system/icons/", sizeof path); kstrcat(path, icon_names[i], sizeof path); kstrcat(path, ".svg", sizeof path);
        fs_write_text(path, icon_svgs[i], (int)strlen(icon_svgs[i]));
    }
    fs_write_text("/docs/readme.txt", readme, (int)strlen(readme));
    fs_write_text("/docs/sample.svg", sample_svg, (int)strlen(sample_svg));
    const char *n1 = "Welcome to Notes\nThis note is stored in /notes. Press Ctrl+N for a new one.\n";
    fs_write_text("/notes/welcome.txt", n1, (int)strlen(n1));
}
void fs_init(void) {
    disk_ok = disk_present();
    if (disk_ok && load_disk()) { fs_create("/images", FS_DIR); fs_create("/downloads", FS_DIR); return; }      /* disks made by 1.3 or older get the folder too */
    make_defaults();
    fs_save();
}
void fs_format(void) { make_defaults(); settings_save(); fs_save(); }
