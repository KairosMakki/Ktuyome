#pragma once
/* Ktuyome OS - shared declarations */
typedef unsigned char u8;  typedef signed char s8;
typedef unsigned short u16; typedef short s16;
typedef unsigned int u32;  typedef int s32;
typedef unsigned long long u64;
#define NULL ((void*)0)
#define KT_VERSION "1.9"

/* ---------- lib.c ---------- */
void *memcpy(void *d, const void *s, u32 n);
void *memmove(void *d, const void *s, u32 n);
void *memset(void *d, int c, u32 n);
int   memcmp(const void *a, const void *b, u32 n);
u32   strlen(const char *s);
int   strcmp(const char *a, const char *b);
void  kstrcpy(char *dst, const char *src, int cap);   /* bounded copy, always terminates */
int   kstrcat(char *dst, const char *src, int cap);
int   kitoa(char *out, int v);                         /* returns length */
int   katoi(const char *s);
int   ends_with(const char *s, const char *suffix);
int   starts_with(const char *s, const char *prefix);
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iabs(int a) { return a < 0 ? -a : a; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float k_sqrt(float x);  float k_sin(float x);  float k_cos(float x);
float k_atan2(float y, float x);  float k_floor(float x);  float k_ceil(float x);
static inline float k_fabs(float x) { return x < 0 ? -x : x; }
double d_floor(double x);  double d_sqrt(double x);

/* ---------- arch.c : CPU, PIC, PIT, RTC, ATA, input drivers ---------- */
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }

void arch_init(void);
u32  ticks(void);                         /* milliseconds since boot */
void rtc_read(int *h, int *m, int *s, int *day, int *mon, int *year);
void rtc_set_time(int h, int m);
void power_off(void);
void reboot(void);
const char *cpu_brand(void);
u32  mem_total_kb(void);

enum { EV_KEY, EV_MOVE, EV_DOWN, EV_UP, EV_WHEEL };
enum { K_BS = 8, K_TAB = 9, K_ENTER = 10, K_ESC = 27,
       K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_INS,
       K_F1 = 0x120, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12 };
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
typedef struct { int type, key, ch, mods, x, y, btn, wheel; } Event;
int  ev_poll(Event *e);                   /* 1 when an event was returned */
extern int mouse_x, mouse_y, mouse_btn, mouse_speed, mouse_has_wheel, mouse_ok;

int  disk_present(void);
u32  disk_sectors(void);
int  disk_read(u32 lba, void *buf, int nsec);
int  disk_write(u32 lba, const void *buf, int nsec);

/* ---------- gfx.c ---------- */
#define ARGB(a,r,g,b) (((u32)(a)<<24)|((u32)(r)<<16)|((u32)(g)<<8)|(u32)(b))
#define RGB(r,g,b) ARGB(255,r,g,b)
#define WHITE_A(a) ARGB(a,255,255,255)
#define BLACK_A(a) ARGB(a,0,0,0)
#define MAXW 1920
#define MAXH 1080
typedef struct { u8 w, h; s8 bx, by; u8 adv; u32 off; } Glyph;
typedef struct { const Glyph *g; const u8 *d; int h, asc, mono; } Font;
extern const Font fonts[6];
enum { F_SM, F_MD, F_LG, F_MDB, F_XL, F_MONO };
extern int W, H;
extern u32 g_bb[MAXW * MAXH];
extern u32 g_wall[MAXW * MAXH];
extern int g_brightness;

int  gfx_init(u32 mbi);
void gfx_target(u32 *p, int w, int h);
void gfx_clip(int x, int y, int w, int h);
void gfx_noclip(void);
void gfx_fill(int x, int y, int w, int h, u32 argb);
void gfx_rrect(int x, int y, int w, int h, int r, u32 c1, u32 c2, int thick);
void gfx_circle(int cx, int cy, int r, u32 argb);
void gfx_shadow(int x, int y, int w, int h, int r, int blur, int alpha);
void gfx_glow(int cx, int cy, int r, u32 rgb, int amax);
void gfx_vgrad(int x, int y, int w, int h, u32 c1, u32 c2);
int  gfx_text_w(int font, const char *s, int n);
void gfx_text(int font, int x, int y, const char *s, u32 argb);
void gfx_text_n(int font, int x, int y, const char *s, int n, u32 argb);
void gfx_text_c(int font, int cx, int y, const char *s, u32 argb);
void gfx_text_r(int font, int rx, int y, const char *s, u32 argb);
void gfx_blit_pm(const u32 *src, int sw, int sh, int dx, int dy, int alpha);
void gfx_blit_scaled(const u32 *src, int sw, int sh, int dx, int dy, int dw, int dh, int alpha);
void gfx_present(void);
void gfx_bake_wallpaper(int theme);
u32  col_lerp(u32 a, u32 b, int t);       /* t 0..256 */
u32  col_alpha(u32 c, int a);             /* scale alpha by a/255 */

/* ---------- svg.c ---------- */
int  svg_render(const char *svg, int len, u32 *out, int size, u32 tint);   /* premultiplied ARGB */

/* ---------- png.c : PNG decoder / encoder ---------- */
typedef struct { int w, h; u32 *px; } PngImage;               /* px: premultiplied ARGB, kmalloc'd */
int  png_is(const char *name);                                /* file name ends in .png */
int  png_decode(const void *data, int len, PngImage *img);    /* 1 = ok (free with png_free), 0 = not a supported PNG */
void png_free(PngImage *img);
void png_fit(const PngImage *img, u32 *out, int size);        /* letterbox into size x size (icons, thumbnails) */
int  png_max_size(int w, int h);                              /* output buffer size png_encode may need */
int  png_encode(const u32 *px, int w, int h, u8 *out, int cap);/* 8-bit RGB PNG; returns bytes or -1 */

/* ---------- fs.c ---------- */
#define FS_MAX 128
#define FS_BLK 16384
#define FS_NBLK 480
#define FS_DIR 1
#define FS_FILE 2
typedef struct { char name[28]; s16 parent; u8 type; u8 used; u32 size; s32 blk; } Node;
extern Node fs_nodes[FS_MAX];
void fs_init(void);                        /* loads disk or creates defaults */
int  fs_lookup(const char *path);          /* -1 if missing */
int  fs_create(const char *path, int type);/* returns node or -1; existing file returned as-is */
int  fs_write(int n, const void *data, int len);   /* files may span several contiguous blocks (up to FS_NBLK * FS_BLK) */
const char *fs_data(int n);                /* file content pointer (not terminated) */
int  fs_delete(int n);                     /* recursive for directories */
int  fs_child(int dir, int prev);          /* iterate children: prev=-1 starts; -1 end */
void fs_path(int n, char *out, int cap);
int  fs_count_files(void);
int  fs_blocks_used(void);
void fs_save(void);
void fs_format(void);
int  fs_has_disk(void);
int  fs_read_text(const char *path, char *out, int cap);  /* returns length or -1 */
int  fs_write_text(const char *path, const char *s, int len);

/* ---------- settings ---------- */
typedef struct { u32 magic; s32 accent, wall, bright, mspeed, h24, secs; } Settings;
extern Settings cfg;
extern const u32 accents[5];
extern const char *accent_names[5];
extern const char *wall_names[3];
void settings_load(void);
void settings_save(void);
void settings_apply(void);
#define ACC (accents[cfg.accent])

/* ---------- ui.c : shell + immediate-mode widgets ---------- */
typedef struct {
    const char *title; int icon;
    void (*open)(void); void (*close)(void);
    int  (*key)(const Event *e);                 /* return 1 if consumed */
    void (*draw)(int x, int y, int w, int h);
} App;
enum { APP_NOTES, APP_EDITOR, APP_CALC, APP_FILES, APP_SETTINGS, APP_TASKMGR, APP_NETWORK, APP_BROWSER, APP_DISCORD, APP_ZIP, APP_COUNT };
enum { IC_NOTES, IC_EDITOR, IC_CALC, IC_FILES, IC_SETTINGS, IC_ABOUT, IC_RESTART, IC_POWER, IC_TASKMGR, IC_NETWORK, IC_BROWSER, IC_DISCORD, IC_ZIP, IC_COUNT };
extern int g_click, g_click_x, g_click_y, g_click_mods, g_wheel;
extern int g_blink;
void ui_open_app(int app);
void ui_close_app(void);
void ui_dialog(const char *title, const char *msg, const char *b0, const char *b1, int action);
int  ui_click_in(int x, int y, int w, int h);       /* consumes click */
int  ui_hover(int x, int y, int w, int h);
int  ui_btn(int x, int y, int w, int h, const char *label, int primary);
int  ui_toggle(int x, int y, int on);               /* returns 1 if clicked */
int  ui_slider(int id, int x, int y, int w, int val, int lo, int hi); /* returns new value */
void ui_icon(int icon, int x, int y, int size, int alpha);
void ui_panel(int x, int y, int w, int h, u32 fill);
void ui_wake(void);                                 /* request redraw */
void ui_flash(const char *msg);
void ui_reload_icons(void);                         /* icons come from /system/icons/<name>.png, else <name>.svg */
void ui_switch_app(int app);
void ui_logo(int x, int y, int size);
int  ui_foreground_app(void);                       /* -1 when the home screen is showing */
int  ui_app_opens(int app);                         /* times opened this session */
u32  ui_frames(void);                               /* frames rendered since boot */
extern int g_dlg_arg;
enum { DLG_RESTART = 1, DLG_SHUTDOWN, DLG_FORMAT, DLG_APP = 10 };
void apps_dialog_done(int action, int arg);
void apps_tick(void);

/* ---------- text widget ---------- */
typedef struct { char *buf; int cap, len, cur, scrolly, scrollx, font, dirty, autoindent, last_edit, follow;
                 int sel, drag, clk_t, clk_n; } TextBuf;       /* sel = selection anchor (-1 none); the other end is cur */
void tb_init(TextBuf *t, char *buf, int cap, int font);
void tb_set(TextBuf *t, const char *s, int len);
int  tb_key(TextBuf *t, const Event *e);            /* returns 1 if consumed */
void tb_draw(TextBuf *t, int x, int y, int w, int h, int focus, int gutter);
int  tb_line(TextBuf *t);  int tb_col(TextBuf *t);  int tb_lines(TextBuf *t);

/* ---------- apps.c ---------- */
extern App apps[APP_COUNT];
void apps_init(void);
void editor_open_path(const char *path);
void files_invalidate(void);                         /* re-read the folder the Files app is showing */
void settings_goto(int page);
extern const char *icon_names[IC_COUNT];
extern const char *icon_svgs[IC_COUNT];
extern const char *sample_svg;
void panic_screen(const char *msg, u32 a, u32 b);

/* single-line input prompt (save as, new file, ...) */
typedef struct { int active; char title[40]; char buf[512]; int len; int id; int selall; } Prompt;
void pr_open(Prompt *p, const char *title, const char *init, int id);
int  pr_key(Prompt *p, const Event *e);              /* 1 = confirmed, 2 = cancelled, 0 = still open */
void pr_draw(Prompt *p, int x, int y, int w, int h);
void pr_paste(Prompt *p);                            /* append the clipboard's first line (replaces the text when it is all selected) */
void notes_save_now(void);

/* ---------- clip.c : clipboard ---------- */
enum { CK_NONE, CK_COPY, CK_CUT, CK_PASTE, CK_ALL };
int  clip_key(const Event *e);                      /* Ctrl+C/X/V/A, Ctrl+Insert, Shift+Insert, Shift+Delete -> CK_* */
int  clip_set(const char *s, int n);                /* replace the clipboard; returns characters stored */
void clip_begin(void);  void clip_add(const char *s, int n);   /* build it in pieces */
int  clip_len(void);  const char *clip_text(void);  /* NUL-terminated */
int  clip_truncated(void);                          /* last copy did not fit */
int  clip_history_count(void);
const char *clip_history(int i);                    /* 0 = most recent */
void clip_clear(void);
int  clip_line(char *out, int cap, int *more);      /* first non-blank line, trimmed */
void clip_notify(const char *verb, int n);          /* toast: "Copied 12 characters" */
u32  clip_static_bytes(void);

/* ---------- taskmgr.c : Task Manager / resource monitor ---------- */
static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
void tm_account(int app, u64 busy, u64 idle);       /* called by the main loop every iteration (app -1 = shell) */
void tm_tick(void);                                 /* once per main-loop pass; samples once per second */
void tm_open(void); void tm_close(void);
int  tm_key(const Event *e);
void tm_draw(int x, int y, int w, int h);
u32  app_static_bytes(int app);                     /* statically allocated working memory of an app */
u32  files_static_bytes(void);

/* ---------- net.c : e1000 driver + TCP/IP stack ---------- */
enum { DHCP_NONE, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND, DHCP_FAILED, DHCP_STATIC };
typedef struct {
    int present, link, dhcp; u8 mac[6]; char nic[40];
    u32 ip, mask, gw, dns, lease, dhcp_server;
    u32 rx_pkts, tx_pkts, rx_bytes, tx_bytes, rx_drop, tx_drop, arp_fail, icmp_rx, icmp_tx, udp_rx, udp_tx, tcp_rx, tcp_tx;
} NetInfo;
typedef struct { int valid; u32 ip; u8 mac[6]; u32 t; } NetArp;
typedef struct { u8 type, code, ttl; u16 id, seq; u32 src, t; } NetIcmpEv;   /* type 0 echo reply, 3 unreachable, 11 time exceeded */
enum { TCP_NONE, TCP_CONNECTING, TCP_OPEN, TCP_CLOSED, TCP_REFUSED, TCP_RESET, TCP_TIMEOUT };
extern NetInfo net; extern NetArp net_arp[8];
void net_init(void);  void net_poll(void);  void net_isr_stamp(void);
void net_dhcp_start(void);
void net_set_static(u32 ip, u32 mask, u32 gw, u32 dns);
u32  net_ip(int a, int b, int c, int d);
void net_ip_str(char *out, u32 ip);                  /* out needs 16 bytes */
int  net_parse_ip(const char *s, u32 *out);          /* dotted quad only */
int  net_icmp_echo(u32 dst, int ttl, int id, int seq, int plen);
int  net_icmp_pop(NetIcmpEv *e);
int  udp_open(u16 port);  void udp_close(int s);
int  udp_send(int s, u32 ip, u16 port, const void *data, int len);
int  udp_recv(int s, void *buf, int cap, u32 *from_ip, u16 *from_port);
int  net_dns_start(const char *name);                /* handle or -1 */
int  net_dns_poll(int h, u32 *ip);                   /* 1 ok, 0 pending, -1 timeout, -2 no such host, -3 no DNS server */
void net_dns_free(int h);
int  tcp_open(u32 ip, u16 port);  int tcp_status(int h);
int  tcp_send(int h, const void *d, int n);  int tcp_recv(int h, void *buf, int cap);
int  tcp_eof(int h);  void tcp_close(int h);  void tcp_free(int h);

/* ---------- netapp.c : Network app ---------- */
void na_open(void); void na_close(void);
int  na_key(const Event *e);
void na_draw(int x, int y, int w, int h);
void na_tick(void);
u32  netapp_static_bytes(void);

/* ---------- browser.c : Text-mode web browser ---------- */
void browser_open(void); void browser_close(void);
int  browser_key(const Event *e);
void browser_draw(int x, int y, int w, int h);
void browser_tick(void);
u32  browser_static_bytes(void);

/* ---------- discord.c : Discord client ---------- */
void discord_open(void); void discord_close(void);
int  discord_key(const Event *e);
void discord_draw(int x, int y, int w, int h);
void discord_tick(void);
u32  discord_static_bytes(void);

/* ---------- zip.c : Zip archive app ---------- */
void zip_open(void); void zip_close(void);
int  zip_key(const Event *e);
void zip_draw(int x, int y, int w, int h);
u32  zip_static_bytes(void);

/* ---------- kmalloc.c : kernel heap ---------- */
void *kmalloc(u32 n);
void *krealloc(void *p, u32 n);
void  kfree(void *p);
void *kcalloc(u32 nmemb, u32 sz);
u32   kmalloc_used(void);
u32   kmalloc_total(void);
