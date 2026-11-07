#include "ktuyome.h"
/* FAT32 driver (read-only) for the second IDE drive. Attach a FAT32 volume with
 * "make runfat" (QEMU -hdb fat.img) or use a hardware primary slave; Files then shows it
 * under the FAT32 button and copies items into KTFS on demand.
 *
 * Supported: MBR partition types 0x0B/0x0C and superfloppy volumes, 512-byte sectors,
 * long file names (LFN), nested directories, cluster-chain reads. Writes are not
 * performed: the volume is treated as read-only, and files are imported into KTFS.
 */

static int mounted;                                    /* 1 after a successful fat_mount() */
static char mstatus[64] = "no volume mounted";

static u32 bps, spc, reserved, nfats, fatsz;           /* BPB geometry (in sectors) */
static u32 root_clus, part_lba, data_lba;

static u32 rd16(const u8 *p, int o) { return p[o] | ((u32)p[o + 1] << 8); }
static u32 rd32(const u8 *p, int o) {
    return (u32)p[o] | ((u32)p[o + 1] << 8) | ((u32)p[o + 2] << 16) | ((u32)p[o + 3] << 24);
}

/* one-sector cache: path lookups walk the FAT and directories sector by sector */
static u8 cach[512]; static u32 cach_lba; static int cach_ok;
static const u8 *fatsec(u32 lba) {
    if (cach_ok && cach_lba == lba) return cach;
    if (!fat_disk_read(lba, cach, 1)) { cach_ok = 0; return NULL; }
    cach_lba = lba; cach_ok = 1; return cach;
}
static u32 fat_entry(u32 c) {                          /* FAT32 cluster entry (0 = fail -> EOC) */
    u32 off = c * 4;
    const u8 *p = fatsec(part_lba + reserved + off / bps);
    return p ? rd32(p, (int)(off % bps)) & 0x0FFFFFFFu : 0x0FFFFFF8u;
}
static u32 clus_lba(u32 c) { return data_lba + (c - 2) * spc; }
static int eoc(u32 c) { return c >= 0x0FFFFFF8u; }

int fat_present(void) { return mounted; }
const char *fat_status(void) { return mstatus; }

int fat_mount(void) {
    mounted = 0; kstrcpy(mstatus, "no FAT32 volume found", (int)sizeof mstatus);
    if (!fat_disk_present()) { kstrcpy(mstatus, "no second disk attached", (int)sizeof mstatus); return 0; }
    static u8 s0[512];
    if (!fat_disk_read(0, s0, 1)) { kstrcpy(mstatus, "read error on sector 0", (int)sizeof mstatus); return 0; }
    if (s0[510] != 0x55 || s0[511] != 0xAA) { kstrcpy(mstatus, "no MBR/BPB signature (0x55AA)", (int)sizeof mstatus); return 0; }
    part_lba = 0;
    if (rd16(s0, 11) != 512) {                         /* sector 0 is an MBR: find a FAT32 partition */
        int got = 0;
        for (int i = 0; i < 4; i++) {
            const u8 *e = s0 + 446 + i * 16;
            u8 t = e[4];
            if (t == 0x0B || t == 0x0C) { part_lba = rd32(e, 8); got = 1; break; }
        }
        if (!got) return 0;
        if (!fat_disk_read(part_lba, s0, 1)) return 0;
        if (rd16(s0, 11) != 512) { kstrcpy(mstatus, "unsupported sector size", (int)sizeof mstatus); return 0; }
    }
    bps = rd16(s0, 11); spc = s0[13]; reserved = rd16(s0, 14); nfats = s0[16];
    fatsz = rd16(s0, 22) ? rd16(s0, 22) : rd32(s0, 36);
    root_clus = rd32(s0, 44);
    data_lba = part_lba + reserved + nfats * fatsz;
    if (spc == 0 || nfats == 0 || fatsz == 0 || reserved == 0 || root_clus < 2) {
        kstrcpy(mstatus, "invalid FAT32 BPB", (int)sizeof mstatus); return 0;
    }
    if (data_lba + 4 > fat_disk_sectors()) { kstrcpy(mstatus, "volume larger than the disk", (int)sizeof mstatus); return 0; }
    if (!fatsec(clus_lba(root_clus))) { kstrcpy(mstatus, "cannot read the root directory", (int)sizeof mstatus); return 0; }
    mounted = 1;
    kstrcpy(mstatus, "FAT32 volume mounted (read-only)", (int)sizeof mstatus);
    return 1;
}

/* ---------------- directory entry scanning ---------------- */
#define ATTR_LFN 0x0F
typedef struct {                                       /* one directory entry as the eye sees it */
    char name[256]; int is_dir; u32 size; u32 cluster; int lfn;   /* lfn = the name came from LFN records */
} DirEnt;

static u8 lfn_checksum_dir;                            /* checksum of the LFN records pending */
static u8 lfn_have;                                    /* LFN records seen since the last 8.3 entry */
static u16 lfn_parts[260]; static u32 lfn_len;

static void lfn_reset(void) { lfn_have = 0; lfn_len = 0; }
static u32 lfn_ascii(char *out, int cap) {             /* UCS-2 -> ASCII, truncated to cap-1 */
    u32 n = 0;
    for (u32 i = 0; i < lfn_len && n + 1 < (u32)cap; i++) {
        u16 c = lfn_parts[i];
        if (c == 0xFFFF) break;
        out[n++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '_';
    }
    out[n] = 0;
    return n;
}
/* short name (8.3) to a normal string */
static int short_name(const u8 *e, char *out, int cap) {
    if (e[0] == 0x05) { out[0] = (char)0xE5; }
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    out[n] = 0;
    if (e[8] != ' ') {
        kstrcat(out, ".", cap);
        for (int i = 8; i < 11 && e[i] != ' '; i++) { int l = (int)strlen(out); if (l + 1 < cap) { out[l] = (char)e[i]; out[l + 1] = 0; } }
    }
    return 1;
}

/* walk one directory (given its first cluster) and materialise the idx-th entry.
 * dir = first cluster of the directory (root included); idx counts user-visible entries. */
static int dir_get_clus(u32 dir, int idx, DirEnt *out) {
    u32 c = dir; int ne = 0; u32 guard = 0;
    while (!eoc(c)) {
        for (u32 s = 0; s < spc; s++) {                /* every sector of the cluster */
            const u8 *p = fatsec(clus_lba(c) + s);
            if (!p) return 0;
            for (int o = 0; o + 32 <= 512; o += 32) {
                const u8 *e = p + o;
                if (e[0] == 0x00) return -1;           /* end of directory */
                if (e[0] == 0xE5) { lfn_reset(); continue; }                 /* deleted */
                u8 attr = e[11];
                if (attr == ATTR_LFN) {                                  /* part of a long file name */
                    int seq = e[0] & 0x1F; if (!seq || seq > 20) { lfn_reset(); continue; }
                    if (e[0] & 0x40) { lfn_checksum_dir = e[13]; }           /* checksum only on the first record */
                    u32 base = (u32)(seq - 1) * 13;
                    if (base + 13 < 260) {
                        u16 *w = lfn_parts + base;
                        static const int roff[3] = { 1, 14, 28 }; static const int rn[3] = { 5, 6, 2 };
                        for (int k = 0; k < 3; k++) for (int i = 0; i < rn[k]; i++) {
                            u16 ch = (u16)(e[roff[k] + i * 2] | ((u16)e[roff[k] + i * 2 + 1] << 8));
                            w[i] = (ch == 0xFFFF) ? 0 : ch;
                            if (ch == 0) { for (u32 z = i + 1; z < 13; z++) w[z] = 0; break; }   /* NUL terminator */
                        }
                    }
                    if (e[0] & 0x40) lfn_len = base + 13;
                    lfn_have = 1; continue;
                }
                if (attr & 0x08) { lfn_reset(); continue; }                  /* volume label */
                if (e[0] == '.') { lfn_reset(); continue; }                  /* "." and ".." are internal links */
                ne++;                                    /* a real entry (8.3 + optional LFN before it) */
                if (ne - 1 == idx) {
                    char nm[256];
                    short_name(e, nm, (int)sizeof nm);
                    out->name[0] = 0;
                    out->lfn = 0;
                    if (lfn_have) {                      /* use the LFN when its checksum matches */
                        u8 sum = 0; for (int i = 0; i < 11; i++) sum = (u8)(sum + (u8)e[i]);
                        if (sum == lfn_checksum_dir) { out->lfn = 1; lfn_ascii(out->name, (int)sizeof out->name); }
                    }
                    if (!out->name[0]) kstrcpy(out->name, nm, sizeof out->name);
                    out->is_dir = (attr & 0x10) != 0;
                    out->size = e[28] | ((u32)e[29] << 8) | ((u32)e[30] << 16) | ((u32)e[31] << 24);
                    out->cluster = (u32)e[26] | ((u32)e[27] << 16);
                    lfn_reset();
                    return 1;
                }
                lfn_reset();
            }
        }
        c = fat_entry(c); guard++;
        if (guard > 65536) return -1;                  /* corrupt chain */
    }
    return -1;
}

static int ci_eq(const char *a, const char *b) {
    while (*a || *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!*a || !*b) return *a == *b;
        a++; b++;
    }
    return 1;
}
int fat_lookup(const char *path, FatEnt *fe) {
    if (!mounted) return 0;
    u32 cur = root_clus;
    fe->size = 0; fe->cluster = 0; fe->is_dir = 1;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        char comp[64]; int n = 0;
        while (*path && *path != '/' && n < 63) comp[n++] = *path++;
        comp[n] = 0;
        DirEnt d; int i = 0, got = 0;
        for (;;) {
            int r = dir_get_clus(cur, i, &d);
            if (r <= 0) break;
            if (ci_eq(d.name, comp)) { got = 1; break; }
            i++;
        }
        if (!got) return 0;
        fe->size = d.size; fe->cluster = d.cluster; fe->is_dir = d.is_dir;
        if (*path) {
            if (!d.is_dir || d.cluster < 2) return 0;
            cur = d.cluster;
        }
    }
    return 1;
}
int fat_dir_get(const char *dir, int idx, char *name, int cap, FatEnt *fe) {
    if (!mounted) return 0;
    u32 cur = root_clus;
    if (dir && dir[0] && !(dir[0] == '/' && dir[1] == 0)) {
        FatEnt d;
        if (!fat_lookup(dir, &d) || !d.is_dir || d.cluster < 2) return 0;
        cur = d.cluster;
    }
    DirEnt e; int r = dir_get_clus(cur, idx, &e);
    if (r != 1) return r == 0 ? 0 : -1;
    kstrcpy(name, e.name, cap);
    fe->size = e.size; fe->cluster = e.cluster; fe->is_dir = e.is_dir;
    return 1;
}

/* read a file into a buffer (up to cap bytes) following its cluster chain */
int fat_read_file(const char *path, void *buf, int cap) {
    FatEnt fe;
    if (!mounted || cap <= 0) return -1;
    if (!fat_lookup(path, &fe) || fe.is_dir) return -1;
    if (fe.size > (u32)cap) return -1;                 /* too large for the caller's buffer */
    u8 *o = buf; int left = cap; u32 c = fe.cluster, guard = 0; int total = 0;
    while (!eoc(c)) {
        u32 bytes = spc * 512; int at = (u32)fe.size - (u32)total < bytes ? (int)((u32)fe.size - (u32)total) : (int)bytes;
        if (at > left) return -1;                      /* would overflow */
        if (at > 0) {
            int ns = (at + 511) / 512;
            if (ns > (int)(fat_disk_sectors() - clus_lba(c))) return -1;
            if (!fat_disk_read(clus_lba(c), o, ns)) return -1;
        }
        o += at; left -= at; total += at;
        if (total >= (int)fe.size) return total;
        c = fat_entry(c); guard++;
        if (guard > 65536) return -1;
    }
    return total;                                      /* sparse/truncated file: give what we got */
}
