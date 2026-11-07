#include "ktuyome.h"

/* ============================ interrupts ============================ */
typedef struct { u32 edi, esi, ebp, esp, ebx, edx, ecx, eax, int_no, err, eip, cs, eflags; } Regs;
typedef struct __attribute__((packed)) { u16 lo, sel; u8 zero, flags; u16 hi; } IdtEnt;
static IdtEnt idt[256];
extern u32 isr_table[48];
static volatile u32 tick_count;

static const char *exc_names[20] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "x87 FPU error",
    "Alignment check", "Machine check", "SIMD error" };

/* ============================ PS/2 input ============================ */
#define KQ 128
static volatile u8 kq[KQ]; static volatile int kq_w, kq_r;
typedef struct { s16 dx, dy; s8 wh; u8 btn; } MPkt;
static volatile MPkt mq[64]; static volatile int mq_w, mq_r;
int mouse_x = 640, mouse_y = 360, mouse_btn, mouse_speed = 2, mouse_has_wheel, mouse_ok;
static u8 mpkt[4]; static int mi;

static void mouse_byte(u8 b) {
    if (mi == 0 && !(b & 8)) return;           /* resync */
    mpkt[mi++] = b;
    if (mi < (mouse_has_wheel ? 4 : 3)) return;
    mi = 0;
    if (mpkt[0] & 0xC0) return;                /* overflow */
    MPkt p;
    p.dx = (s16)(mpkt[1] - ((mpkt[0] << 4) & 0x100));
    p.dy = (s16)(mpkt[2] - ((mpkt[0] << 3) & 0x100));
    p.wh = 0;
    if (mouse_has_wheel) { int w = mpkt[3] & 0x0F; p.wh = (s8)((w & 8) ? w - 16 : w); }
    p.btn = mpkt[0] & 7;
    int nw = (mq_w + 1) & 63;
    if (nw != mq_r) { mq[mq_w] = p; mq_w = nw; }
}
static void input_irq(void) {
    for (int guard = 0; guard < 32; guard++) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 b = inb(0x60);
        if (st & 0x20) mouse_byte(b);
        else { int nw = (kq_w + 1) % KQ; if (nw != kq_r) { kq[kq_w] = b; kq_w = nw; } }
    }
}

void isr_dispatch(Regs *r) {
    if (r->int_no < 32) {
        panic_screen(r->int_no < 20 ? exc_names[r->int_no] : "CPU exception", r->int_no, r->eip);
        for (;;) __asm__ volatile("cli; hlt");
    }
    int irq = r->int_no - 32;
    if (irq == 0) { tick_count++; net_isr_stamp(); }
    else if (irq == 1 || irq == 12) input_irq();
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

u32 ticks(void) { return tick_count; }

static void idt_init(void) {
    for (int i = 0; i < 48; i++) {
        u32 h = isr_table[i];
        idt[i].lo = h & 0xFFFF; idt[i].sel = 0x08; idt[i].zero = 0; idt[i].flags = 0x8E; idt[i].hi = h >> 16;
    }
    struct { u16 lim; u32 base; } __attribute__((packed)) d = { sizeof(idt) - 1, (u32)idt };
    __asm__ volatile("lidt %0" :: "m"(d));
}
static void pic_init(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xF8); outb(0xA1, 0xEF);        /* IRQ0, IRQ1, cascade, IRQ12 */
}
static void pit_init(void) {
    outb(0x43, 0x34);
    outb(0x40, 1193 & 0xFF); outb(0x40, 1193 >> 8);   /* ~1000 Hz */
}

/* ---- i8042 controller ---- */
static int kbc_wait_in(void)  { for (int i = 0; i < 200000; i++) if (!(inb(0x64) & 2)) return 1; return 0; }
static int kbc_wait_out(void) { for (int i = 0; i < 200000; i++) if (inb(0x64) & 1) return 1; return 0; }
static void kbc_cmd(u8 c)  { kbc_wait_in(); outb(0x64, c); }
static void kbc_data(u8 d) { kbc_wait_in(); outb(0x60, d); }
static int mouse_write(u8 b) { kbc_cmd(0xD4); kbc_data(b); return kbc_wait_out() ? inb(0x60) : -1; }
static int mouse_read(void)  { return kbc_wait_out() ? inb(0x60) : -1; }

static void input_init(void) {
    while (inb(0x64) & 1) inb(0x60);
    kbc_cmd(0xA8);                                       /* enable auxiliary device */
    kbc_cmd(0x20); kbc_wait_out();
    u8 st = inb(0x60);
    st |= 0x03; st &= ~0x30;                             /* IRQ1 + IRQ12 on, clocks on */
    kbc_cmd(0x60); kbc_data(st);
    if (mouse_write(0xF6) == 0xFA) {                     /* defaults */
        mouse_write(0xF3); mouse_write(200);             /* IntelliMouse wheel handshake */
        mouse_write(0xF3); mouse_write(100);
        mouse_write(0xF3); mouse_write(80);
        mouse_write(0xF2);
        if (mouse_read() == 3) mouse_has_wheel = 1;
        mouse_write(0xF3); mouse_write(100);
        if (mouse_write(0xF4) == 0xFA) mouse_ok = 1;     /* start streaming */
    }
    while (inb(0x64) & 1) inb(0x60);
}

/* ---- event decoding ---- */
#define EVQ 64
static Event evq[EVQ]; static int evq_w, evq_r;
static int sh_l, sh_r, ctl, alt, caps, ext, bstate, ax, ay;

static void evq_push(Event e) { int n = (evq_w + 1) % EVQ; if (n != evq_r) { evq[evq_w] = e; evq_w = n; } }
static int cur_mods(void) { return ((sh_l | sh_r) ? MOD_SHIFT : 0) | (ctl ? MOD_CTRL : 0) | (alt ? MOD_ALT : 0); }

static const char sc_norm[59] = {0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,9,'q','w','e','r','t','y','u','i','o','p','[',']',10,0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0};
static const char sc_shift[59] = {0,27,'!','@','#','$','%','^','&','*','(',')','_','+',8,9,'Q','W','E','R','T','Y','U','I','O','P','{','}',10,0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0};
static const char kp_map[13] = {'7','8','9','-','4','5','6','+','1','2','3','0','.'};

static void key_event(int sc) {
    if (sc == 0xE0) { ext = 1; return; }
    int rel = sc & 0x80, code = sc & 0x7F, e0 = ext; ext = 0;
    if (code == 0x2A) { sh_l = !rel; return; }
    if (code == 0x36) { sh_r = !rel; return; }
    if (code == 0x1D) { ctl = !rel; return; }
    if (code == 0x38) { alt = !rel; return; }
    if (rel) return;
    if (code == 0x3A) { caps = !caps; return; }
    Event e = {0}; e.type = EV_KEY; e.mods = cur_mods();
    int key = 0, ch = 0;
    if (e0) {
        switch (code) {
            case 0x48: key = K_UP; break;    case 0x50: key = K_DOWN; break;
            case 0x4B: key = K_LEFT; break;  case 0x4D: key = K_RIGHT; break;
            case 0x47: key = K_HOME; break;  case 0x4F: key = K_END; break;
            case 0x49: key = K_PGUP; break;  case 0x51: key = K_PGDN; break;
            case 0x53: key = K_DEL; break;   case 0x52: key = K_INS; break;
            case 0x1C: key = ch = K_ENTER; break;
            case 0x35: key = ch = '/'; break;
            default: return;
        }
    } else if (code >= 0x3B && code <= 0x44) key = K_F1 + (code - 0x3B);
    else if (code == 0x57) key = K_F11;
    else if (code == 0x58) key = K_F12;
    else if (code >= 0x47 && code <= 0x53) { key = ch = kp_map[code - 0x47]; }
    else if (code < 59) {
        int sh = (sh_l | sh_r);
        char c = sh ? sc_shift[code] : sc_norm[code];
        if (c >= 'a' && c <= 'z' && caps && !sh) c -= 32;
        else if (c >= 'A' && c <= 'Z' && caps && sh) c += 32;
        if (!c) return;
        key = ch = (u8)c;
        if (ctl || alt) { if (sc_norm[code] >= 'a' && sc_norm[code] <= 'z') key = sc_norm[code]; ch = 0; }
    } else return;
    e.key = key; e.ch = (ch >= 32 && ch < 127) ? ch : 0;      /* ENTER/TAB/BS/ESC are keys, not text */
    evq_push(e);
}

int ev_poll(Event *out) {
    for (;;) {
        if (evq_r != evq_w) { *out = evq[evq_r]; evq_r = (evq_r + 1) % EVQ; return 1; }
        if (mq_r != mq_w) {
            MPkt p = mq[mq_r]; mq_r = (mq_r + 1) & 63;
            ax += p.dx * mouse_speed; ay -= p.dy * mouse_speed;
            int mx = ax / 2, my = ay / 2; ax -= mx * 2; ay -= my * 2;
            int ox = mouse_x, oy = mouse_y;
            mouse_x = iclamp(mouse_x + mx, 0, W - 1); mouse_y = iclamp(mouse_y + my, 0, H - 1);
            Event e = {0}; e.x = mouse_x; e.y = mouse_y; e.mods = cur_mods();
            if (mouse_x != ox || mouse_y != oy) { e.type = EV_MOVE; evq_push(e); }
            int chg = p.btn ^ bstate;
            for (int b = 0; b < 3; b++) if (chg & (1 << b)) {
                e.type = (p.btn & (1 << b)) ? EV_DOWN : EV_UP; e.btn = b + 1; evq_push(e);
            }
            bstate = p.btn; mouse_btn = p.btn;
            if (p.wh) { e.type = EV_WHEEL; e.btn = 0; e.wheel = p.wh; evq_push(e); }
            continue;
        }
        if (kq_r != kq_w) { int sc = kq[kq_r]; kq_r = (kq_r + 1) % KQ; key_event(sc); continue; }
        return 0;
    }
}

/* ============================ RTC / CPU ============================ */
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static int bcd(int v, int isbin) { return isbin ? v : (v & 15) + (v >> 4) * 10; }
void rtc_read(int *h, int *m, int *s, int *day, int *mon, int *year) {
    for (int i = 0; i < 1000 && (cmos(0x0A) & 0x80); i++) ;
    u8 b = cmos(0x0B); int bin = b & 4;
    int hh = cmos(4); int pm = hh & 0x80; hh &= 0x7F;
    *s = bcd(cmos(0), bin); *m = bcd(cmos(2), bin); hh = bcd(hh, bin);
    if (!(b & 2)) { if (hh == 12) hh = 0; if (pm) hh += 12; }
    *h = hh; *day = bcd(cmos(7), bin); *mon = bcd(cmos(8), bin); *year = 2000 + bcd(cmos(9), bin);
}
void rtc_set_time(int h, int m) {
    u8 b = cmos(0x0B); int bin = b & 4;
    int hh = h, mm = m;
    if (!bin) { hh = (h / 10) * 16 + h % 10; mm = (m / 10) * 16 + m % 10; }
    if (!(b & 2)) { int pm = h >= 12; int h12 = h % 12; if (!h12) h12 = 12;
        hh = bin ? h12 : (h12 / 10) * 16 + h12 % 10; if (pm) hh |= 0x80; }
    outb(0x70, 0x0B); outb(0x71, b | 0x80);              /* halt updates */
    outb(0x70, 4); outb(0x71, hh); outb(0x70, 2); outb(0x71, mm);
    outb(0x70, 0x0B); outb(0x71, b & ~0x80);
}
static char brand[64];
const char *cpu_brand(void) {
    if (brand[0]) return brand;
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000));
    char tmp[49]; int n = 0;
    if (a >= 0x80000004) {
        for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf));
            u32 r[4] = {a, b, c, d};
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) tmp[n++] = (char)(r[i] >> (8 * j));
        }
        tmp[48] = 0; char *p = tmp; while (*p == ' ') p++;
        kstrcpy(brand, p, sizeof(brand));
    } else kstrcpy(brand, "x86 processor", sizeof(brand));
    return brand;
}
static u32 g_mem_kb;
u32 mem_total_kb(void) { return g_mem_kb; }

/* ============================ ATA PIO (primary master) ============================ */
static int ata_ok; static u32 ata_secs;
static int ata_wait(int drq) {
    for (int i = 0; i < 400000; i++) {
        u8 s = inb(0x1F7);
        if (s == 0xFF) return 0;
        if (s & 1) return 0;
        if (!(s & 0x80) && (!drq || (s & 8))) return 1;
    }
    return 0;
}
static void ata_init(void) {
    if (inb(0x1F7) == 0xFF) return;                      /* floating bus: no controller */
    outb(0x1F6, 0xA0); outb(0x1F2, 0); outb(0x1F3, 0); outb(0x1F4, 0); outb(0x1F5, 0);
    outb(0x1F7, 0xEC);
    if (inb(0x1F7) == 0) return;
    if (inb(0x1F4) || inb(0x1F5)) return;                /* ATAPI/SATA signature */
    if (!ata_wait(1)) return;
    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(0x1F0);
    ata_secs = id[60] | ((u32)id[61] << 16);
    ata_ok = ata_secs > 64;
}
int disk_present(void) { return ata_ok; }
u32 disk_sectors(void) { return ata_secs; }
static void ata_setup(u32 lba, int n, u8 cmd) {
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, (u8)n); outb(0x1F3, lba); outb(0x1F4, lba >> 8); outb(0x1F5, lba >> 16);
    outb(0x1F7, cmd);
}
int disk_read(u32 lba, void *buf, int nsec) {
    if (!ata_ok) return 0;
    u16 *p = buf;
    while (nsec > 0) {
        int n = nsec > 128 ? 128 : nsec;
        ata_setup(lba, n, 0x20);
        for (int s = 0; s < n; s++) {
            if (!ata_wait(1)) return 0;
            __asm__ volatile("rep insw" : "+D"(p) : "d"((u16)0x1F0), "c"(256) : "memory");
        }
        lba += n; nsec -= n;
    }
    return 1;
}
int disk_write(u32 lba, const void *buf, int nsec) {
    if (!ata_ok) return 0;
    const u16 *p = buf;
    while (nsec > 0) {
        int n = nsec > 128 ? 128 : nsec;
        ata_setup(lba, n, 0x30);
        for (int s = 0; s < n; s++) {
            if (!ata_wait(1)) return 0;
            __asm__ volatile("rep outsw" : "+S"(p) : "d"((u16)0x1F0), "c"(256));
        }
        outb(0x1F7, 0xE7); ata_wait(0);                  /* flush cache */
        lba += n; nsec -= n;
    }
    return 1;
}

/* ============================ power ============================ */
void power_off(void) {
    outw(0x604, 0x2000); outw(0xB004, 0x2000); outw(0x4004, 0x3400);
}
void reboot(void) {
    __asm__ volatile("cli");
    kbc_wait_in(); outb(0x64, 0xFE);
    struct { u16 lim; u32 base; } __attribute__((packed)) z = {0, 0};
    __asm__ volatile("lidt %0; int3" :: "m"(z));
}

void arch_init(void) {
    extern u32 g_mbi_mem;
    g_mem_kb = g_mbi_mem;
    pic_init(); idt_init(); pit_init();
    input_init();
    ata_init();
    net_init();
    __asm__ volatile("sti");
}
