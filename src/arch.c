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

/* ============================ serial console (COM1, test/debug) ============================ */
void dbg_init(void) {
    outb(0x3F8 + 1, 0x00);                 /* disable interrupts */
    outb(0x3F8 + 3, 0x80);                 /* DLAB on */
    outb(0x3F8 + 0, 0x02);                 /* 38400 baud... fine for tests */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);                 /* 8N1 */
    outb(0x3F8 + 2, 0xC7);                 /* FIFO clear+enable */
    outb(0x3F8 + 4, 0x0B);                 /* DTR/RTS/OUT2 */
}
void dbg_puts(const char *s) {
    for (u32 i = 0; s && s[i]; i++) {
        for (int t = 0; t < 20000 && !(inb(0x3F8 + 5) & 0x20); t++) ;
        outb(0x3F8 + 0, s[i]);
    }
}

/* ============================ PCI config space ============================ */
PciDev pci_info[16]; int pci_count;
static u32 pci_cfg_read(u8 bus, u8 dev, u8 fn, u8 off) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void pci_cfg_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}
static void pci_add(u8 bus, u8 dev, u8 fn) {
    if (pci_count >= 16) return;
    u32 id0 = pci_cfg_read(bus, dev, fn, 0x00);
    u32 id2 = pci_cfg_read(bus, dev, fn, 0x08);
    PciDev *d = &pci_info[pci_count];
    d->vendor = id0 & 0xFFFF; d->device = id0 >> 16;
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->cls[0] = (u8)(id2 >> 24); d->cls[1] = (u8)(id2 >> 16); d->cls[2] = (u8)(id2 >> 8);
    pci_count++;
}
void pci_scan(void) {
    pci_count = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++) {
            u32 id = pci_cfg_read(bus, dev, 0, 0x00);
            if ((u16)id == 0xFFFF) continue;
            pci_add(bus, dev, 0);
            u32 hdr = pci_cfg_read(bus, dev, 0, 0x0C);
            int multi = (hdr >> 16) & 0x80;
            for (int fn = 1; multi && fn < 8; fn++) {
                if ((u16)pci_cfg_read(bus, dev, fn, 0x00) != 0xFFFF) pci_add(bus, dev, fn);
            }
        }
}

AhciInfo ahci_info;

/* ============================ AHCI (SATA) driver ============================
 * Polled (no interrupts): the PIC mask stays untouched, so reboot/legacy
 * behaviour is unchanged. In this kernel physical = linear (no paging), so
 * PCI MMIO BARs are directly usable.
 *
 * Command list entry (AHCI spec / QEMU AHCICmdHdr, 32 bytes):
 *   +0x00 u32 opts:   bits 0-4 CFL (FIS length/4), bit 6 W, bits 16-31 PRDTL
 *   +0x04 u32 status: PRDBC bytes transferred (written by controller)
 *   +0x08 u64:        command table base address
 * Command table: CFIS 64B, ACMD 16B, rsvd 48B, PRDT from +0x80. */
/* PRDT entry: u64 base address (DBA+DBAU), u32 reserved, u32 bits 0-21 = byte count - 1, bit31 IOC. */
typedef struct __attribute__((packed)) { u32 dba, dbau, rsv, dw3; } Prd;
typedef struct __attribute__((packed)) { u8 cfis[64]; u8 acmd[16]; u8 rsv[48]; Prd prdt[16]; } CmdTable;
typedef struct __attribute__((packed)) { u32 opts; u32 status; u32 addr0, addr1; u32 rsv[4]; } CmdList;

static volatile u8 *ab;
static int ahci_ok; static u32 ahci_secs;
static volatile u32 *hp;                  /* port registers of the disk port */
static CmdList   ahci_cl  __attribute__((aligned(1024)));
static u8        ahci_fb[256] __attribute__((aligned(256)));
static CmdTable  ahci_ct  __attribute__((aligned(128)));

static const char hxk[] = "0123456789abcdef";
const char *hex8(u32 v) {
    static char buf[11]; buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hxk[(v >> (28 - 4 * i)) & 15];
    buf[10] = 0; return buf;
}
const char *hex4(u32 v) {
    static char buf[7]; buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 4; i++) buf[2 + i] = hxk[(v >> (12 - 4 * i)) & 15];
    buf[6] = 0; return buf;
}

static u32 ahci_cap(void) { return mmio_rd32(ab + 0x00); }
static u32 ahci_pi(void)  { return mmio_rd32(ab + 0x0C); }

static void iosleep(int ms) { u32 end = ticks() + (u32)ms; while (ticks() < end) __asm__ volatile("pause" ::: "memory"); }

/* Compiler fence: the DMA structures (ahci_cl/ahci_ct/buffers) are plain
 * memory - without this GCC may sink their stores past the volatile MMIO
 * PxCI write, so the HBA would submit a stale/empty command. */
static inline void kfence(void) { __asm__ volatile("" ::: "memory"); }

static void p_stop(volatile u32 *p) {          /* px.CMD: ST|FRE off, wait CR/FR clear */
    mmio_wr32(&p[6], mmio_rd32(&p[6]) & ~(0x1u | 0x10u));
    int t;
    for (t = 0; t < 500 && (mmio_rd32(&p[6]) & 0x8000u); t++) iosleep(1);   /* CR */
    for (t = 0; t < 500 && (mmio_rd32(&p[6]) & 0x4000u); t++) iosleep(1);   /* FR */
}
static void p_spinup(volatile u32 *p) {        /* after CLB/FB wired: spin up + start engine */
    mmio_wr32(&p[6], 0x16u);                   /* FRE | POD | SUD */
    for (int t = 0; t < 2000; t++) {
        u32 tfd = mmio_rd32(&p[8]);            /* px.TFD (0x20) */
        if (!(tfd & 0x88u)) break;             /* BSY + DRQ clear */
        iosleep(1);
    }
    mmio_wr32(&p[6], 0x17u);                   /* + ST: command engine running */
    for (int t = 0; t < 500 && !(mmio_rd32(&p[6]) & 0x8000u); t++) iosleep(1);
}

/* ---- wait for a submitted command slot to clear; 0 = ok ---- */
static int ahci_wait_slot(volatile u32 *p, const char *what) {
    int done = 0;
    for (int t = 0; t < 8000; t++) {           /* ~8 s timeout */
        if (!(mmio_rd32(&p[14]) & 1u)) { done = 1; break; }
        iosleep(1);
    }
    u32 tfd = mmio_rd32(&p[8]);                /* px.TFD */
    if (!done || (tfd & 0x1u)) {
        dbg_puts("AHCI: "); dbg_puts(what);
        dbg_puts(" failed tfd="); dbg_puts(hex8(tfd));
        dbg_puts(" ci="); dbg_puts(hex8(mmio_rd32(&p[14]))); dbg_puts("\n");
        return -1;
    }
    return 0;
}

/* ---- IDENTIFY DEVICE over AHCI (PIO-in via the PRD) ---- */
static u32 ahci_identify(volatile u32 *p, u16 *idbuf) {
    u8 *cfis = ahci_ct.cfis;
    for (int i = 0; i < 64; i++) cfis[i] = 0;
    cfis[0] = 0x27; cfis[1] = 0x80;
    cfis[2] = 0xEC;                            /* IDENTIFY DEVICE */
    cfis[7] = 0xE0;                            /* device: DEV 0, LBA mode */
    ahci_cl.addr0 = (u32)&ahci_ct;             /* command table pointer */
    ahci_cl.addr1 = 0;
    ahci_cl.opts = 0x5 | (1u << 16);           /* CFL 5, 1 PRD, read */
    ahci_cl.status = 0;
    ahci_ct.prdt[0].dba = (u32)idbuf; ahci_ct.prdt[0].dbau = 0;
    ahci_ct.prdt[0].rsv = 0;
    ahci_ct.prdt[0].dw3 = 512 - 1;
    mmio_wr32(&p[4], 0);                       /* px.IS clear */
    kfence();
    mmio_wr32(&p[14], 1);                      /* px.CI slot 0 */
    if (ahci_wait_slot(p, "identify")) {
        kfence();
        return 0;
    }
    kfence();
    /* a real IDENTIFY starts with "0x80" + a serial number; sanity checks */
    if (idbuf[0] == 0xFFFF || !idbuf[0]) { dbg_puts("AHCI: identify: bad idblock\n"); return 0; }
    dbg_puts("AHCI: identify ok, sectors=");
    u32 secs = (u32)idbuf[60] | ((u32)idbuf[61] << 16);
    char t[16]; kitoa(t, (int)secs); dbg_puts(t); dbg_puts("\n");
    return secs;
}

static int ahci_port_scan(void) {
    int npc = (ahci_cap() & 0x1F) + 1;
    u32 pi = ahci_pi();
    dbg_puts("AHCI: ports impl="); dbg_puts(hex8(pi)); dbg_puts(" npc="); char n[4]; kitoa(n, npc); dbg_puts(n); dbg_puts("\n");
    for (int i = 0; i < npc && i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        volatile u32 *p = (volatile u32 *)(ab + 0x100 + i * 0x80);
        u32 det = mmio_rd32(&p[10]) & 0xF;     /* px.SSTS (0x28) */
        if (det == 0) continue;                /* no device on this port */
        p_stop(p);
        mmio_wr32(&p[0], (u32)&ahci_cl);       /* px.CLB  */
        mmio_wr32(&p[1], 0);                   /* px.CLBU */
        mmio_wr32(&p[2], (u32)&ahci_fb);       /* px.FB   */
        mmio_wr32(&p[3], 0);                   /* px.FBU  */
        mmio_wr32(&p[12], 0);                  /* px.SERR */
        mmio_wr32(&p[4], 0);                   /* px.IS   */
        /* COMRESET: SCTL.DET = 1 for a few ms, then 0 */
        mmio_wr32(&p[11], 0x1);                /* px.SCTL (0x2C) */
        iosleep(2);
        mmio_wr32(&p[11], 0x0);
        iosleep(20);
        p_spinup(p);
        u16 idbuf[256];
        u32 secs = ahci_identify(p, idbuf);
        if (secs > 64) {
            hp = p;
            ahci_secs = secs;
            dbg_puts("AHCI: SATA disk on port "); kitoa(n, i); dbg_puts(n); dbg_puts("\n");
            return 1;
        }
    }
    return 0;
}

static void ahci_init(void) {
    ahci_ok = 0; hp = NULL; ahci_secs = 0; ab = NULL;
    dbg_puts("AHCI: scan\n");
    if (!ahci_info.found) { dbg_puts("AHCI: no controller\n"); return; }
    if (ahci_info.bar & 1) { dbg_puts("AHCI: I/O BAR unsupported\n"); return; }
    ab = (volatile u8 *)(u32)(ahci_info.bar & 0xFFFFFFF0u);
    /* reset the HBA, then enable AHCI */
    mmio_wr32(ab + 0x04, mmio_rd32(ab + 0x04) | 0x1u);          /* GHC.HR */
    for (int t = 0; t < 100 && (mmio_rd32(ab + 0x04) & 0x1u); t++) iosleep(1);
    mmio_wr32(ab + 0x04, mmio_rd32(ab + 0x04) | 0x80000000u);   /* GHC.AE */
    if (!ahci_port_scan()) { dbg_puts("AHCI: no SATA disk\n"); return; }
    dbg_puts("AHCI: disk ready, sectors="); char t[16]; kitoa(t, (int)ahci_secs); dbg_puts(t); dbg_puts("\n");
    ahci_ok = 1;
}

/* ---- one polled DMA transfer ---- */
static int ahci_issue(u32 lba, void *buf, int nsec, int write) {
    if (!ahci_ok || !hp || !buf) return 0;
    u8 *pb = (u8 *)buf;
    int left = nsec;
    volatile u32 *p = hp;
    while (left > 0) {
        int n = left > 8 ? 8 : left;           /* 8 sectors = one 4 KiB PRD */
        u8 *cfis = ahci_ct.cfis;
        for (int i = 0; i < 64; i++) cfis[i] = 0;
        cfis[0] = 0x27;                        /* host-to-device */
        cfis[1] = 0x80;                        /* C bit (update command) */
        cfis[2] = write ? 0x35 : 0x25;         /* WRITE/READ DMA EXT */
        cfis[4] = (u8)lba;                     /* LBA 7:0   */
        cfis[5] = (u8)(lba >> 8);              /* LBA 15:8  */
        cfis[6] = (u8)(lba >> 16);             /* LBA 23:16 */
        cfis[7] = 0xE0;                        /* device: DEV 0, LBA mode */
        cfis[8] = (u8)(lba >> 24);             /* LBA 31:24 (HOB low)     */
        cfis[9] = 0; cfis[10] = 0;             /* LBA 47:32 (unused here) */
        cfis[12] = (u8)n; cfis[13] = 0;        /* sector count */
        ahci_cl.opts = (write ? 0x40u : 0) | 0x5 | (1u << 16);
        ahci_cl.status = 0;
        ahci_cl.addr0 = (u32)&ahci_ct;
        ahci_cl.addr1 = 0;
        ahci_ct.prdt[0].dba = (u32)pb; ahci_ct.prdt[0].dbau = 0;
        ahci_ct.prdt[0].rsv = 0;
        ahci_ct.prdt[0].dw3 = (u32)(n * 512 - 1);   /* IOC off */
        mmio_wr32(&p[4], 0);                   /* px.IS clear */
        kfence();
        mmio_wr32(&p[14], 1);                  /* px.CI: issue slot 0 */
        if (ahci_wait_slot(p, write ? "write" : "read")) {
            p_stop(p); p_spinup(p);
            return 0;
        }
        kfence();                              /* make the DMA result visible */
        pb += n * 512; lba += n; left -= n;
    }
    return 1;
}
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
int disk_present(void) { return ahci_ok || ata_ok; }
u32 disk_sectors(void) { return ahci_ok ? ahci_secs : ata_secs; }
int disk_is_ahci(void) { return ahci_ok; }

/* ============ second IDE drive: the slave of the primary channel ============ */
/* QEMU's PC machine attaches -hdb (and VirtualBox a "primary slave") here. The KTFS system disk is
 * the master, so nothing writes to this drive; the FAT32 driver only needs reads. */
static int ata2_ok; static u32 ata2_secs;
static int ata2_wait(int drq) {
    for (int i = 0; i < 400000; i++) {
        u8 s = inb(0x1F7);
        if (s == 0xFF) return 0;
        if (s & 1) return 0;
        if (!(s & 0x80) && (!drq || (s & 8))) return 1;
    }
    return 0;
}
static void ata2_init(void) {
    if (inb(0x1F7) == 0xFF) return;
    outb(0x1F6, 0xF0);                                   /* select slave */
    for (volatile int i = 0; i < 800; i++) {}
    if (inb(0x1F4) | inb(0x1F5)) { inb(0x1F7); return; }  /* floating/not ATA */
    outb(0x1F2, 0); outb(0x1F3, 0); outb(0x1F4, 0); outb(0x1F5, 0);
    outb(0x1F7, 0xEC);                                   /* IDENTIFY DEVICE */
    if (inb(0x1F7) == 0) return;
    if (inb(0x1F4) || inb(0x1F5)) return;                /* no slave settled, EXDEV/ATAPI signature */
    if (!ata2_wait(1)) return;
    u16 id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(0x1F0);
    ata2_secs = id[60] | ((u32)id[61] << 16);
    ata2_ok = ata2_secs > 64;
}
int fat_disk_present(void) { return ata2_ok; }
u32 fat_disk_sectors(void) { return ata2_secs; }
static int ata2_read(u32 lba, void *buf, int nsec) {
    if (!ata2_ok) return 0;
    u16 *p = buf;
    while (nsec > 0) {
        int n = nsec > 128 ? 128 : nsec;
        outb(0x1F6, 0xF0 | ((lba >> 24) & 0x0F));
        outb(0x1F2, (u8)n); outb(0x1F3, lba); outb(0x1F4, lba >> 8); outb(0x1F5, lba >> 16);
        outb(0x1F7, 0x20);
        for (int s = 0; s < n; s++) {
            if (!ata2_wait(1)) return 0;
            __asm__ volatile("rep insw" : "+D"(p) : "d"((u16)0x1F0), "c"(256) : "memory");
        }
        lba += n; nsec -= n;
    }
    return 1;
}
int fat_disk_read(u32 lba, void *buf, int nsec) { return ata2_read(lba, buf, nsec); }

/* ---- storage bring-up: PCI scan, AHCI if present, else legacy ATA PIO ---- */
static int find_ahci_controller(void) {
    for (int i = 0; i < pci_count; i++) {
        PciDev *d = &pci_info[i];
        if (d->cls[0] != 0x01 || d->cls[1] != 0x06) continue;      /* mass storage / SATA */
        ahci_info.bus = d->bus; ahci_info.dev = d->dev; ahci_info.fn = d->fn;
        if (d->cls[2] == 0x01 || d->cls[2] == 0x02) {             /* AHCI / AHCI rev1.5 prog-if, or vendor-specific */
            u32 bar = pci_cfg_read(d->bus, d->dev, d->fn, 0x24);  /* BAR5 */
            if (bar == 0 || (bar & 1)) continue;                  /* no usable MMIO BAR */
            ahci_info.found = 1; ahci_info.bar = bar;
            u32 cmd = pci_cfg_read(d->bus, d->dev, d->fn, 0x04);  /* enable memory decode + bus master */
            pci_cfg_write(d->bus, d->dev, d->fn, 0x04, (cmd | 0x2 | 0x4) & ~0x400u);
            dbg_puts("AHCI: controller "); dbg_puts(hex4(d->vendor)); dbg_puts(":");
            dbg_puts(hex4(d->device));
            dbg_puts(" bus="); char t[8]; kitoa(t, d->bus); dbg_puts(t);
            dbg_puts(" bar="); dbg_puts(hex8(bar)); dbg_puts("\n");
            return 1;
        }
        dbg_puts("AHCI: SATA controller found but prog-if not AHCI (prog-if="); dbg_puts(hex8((u32)d->cls[2])); dbg_puts(")\n");
    }
    return 0;
}

static void disk_selftest(void) {                       /* serial-log verification harness */
    static u8 s0[512], s1[512], scratch[512], keep[512];
    if (!disk_present() || !disk_read(0, s0, 1)) { dbg_puts("SELFTEST: read fail\n"); return; }
    if (!disk_read(0, s1, 1) || memcmp(s0, s1, 512)) { dbg_puts("SELFTEST: double-read mismatch\n"); return; }
    u32 h = 0x811C9DC5u;                                 /* FNV-1a of sector 0 */
    for (int i = 0; i < 512; i++) { h ^= s0[i]; h *= 0x01000193u; }
    dbg_puts("S0HASH="); dbg_puts(hex8(h)); dbg_puts("\n");
    dbg_puts("READOK\n");
    if (((u32 *)s0)[0] == 0x3153544Bu) { dbg_puts("WRITE-TEST: skipped (KTFS disk)\n"); return; }
    u32 last = disk_sectors() - 1;                       /* scratch on the last sector */
    if (!disk_read(last, keep, 1)) { dbg_puts("SELFTEST: keep read fail\n"); return; }
    kstrcpy((char *)scratch, "KTAHCI-TEST", (int)sizeof scratch); scratch[15] = 0;
    for (int i = 16; i < 512; i++) scratch[i] = (u8)(i * 37 + 0x5A);
    if (!disk_write(last, scratch, 1)) { dbg_puts("WRITE-TEST: write fail\n"); return; }
    dbg_puts("WRITEOK\n");
    static u8 rb[512];
    if (!disk_read(last, rb, 1)) { dbg_puts("WRITE-TEST: readback fail\n"); return; }
    if (memcmp(rb, scratch, 512)) { dbg_puts("WRITE-TEST: data mismatch\n"); return; }
    dbg_puts("WRITECMP-OK\n");
    if (!disk_write(last, keep, 1) || !disk_read(last, rb, 1) || memcmp(rb, keep, 512)) { dbg_puts("RESTORE-FAIL\n"); return; }
    dbg_puts("RESTORED\n");
}

static void storage_init(void) {
    dbg_puts("KTUYOME: storage init\n");
    pci_scan();
    dbg_puts("PCI: "); char t[8]; kitoa(t, pci_count); dbg_puts(t); dbg_puts(" devices\n");
    if (find_ahci_controller()) ahci_init();
    if (!ahci_ok) ata_init();
    ata2_init();                                             /* second IDE drive (FAT32 disk from -hdb) */
    if (ahci_ok) dbg_puts("DISK: ahci\n"); else if (ata_ok) dbg_puts("DISK: ata-pio\n"); else dbg_puts("DISK: none\n");
    if (ata2_ok) { dbg_puts("DISK2: ata-pio slave\n"); } else dbg_puts("DISK2: none\n");
    disk_selftest();
    fat_mount();                                             /* second IDE drive: probe for a FAT32 volume (read-only) */
    dbg_puts("FAT32: "); dbg_puts(fat_status()); dbg_puts("\n");
}

/* ============================ power ============================ */
static void ata_setup(u32 lba, int n, u8 cmd) {
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, (u8)n); outb(0x1F3, lba); outb(0x1F4, lba >> 8); outb(0x1F5, lba >> 16);
    outb(0x1F7, cmd);
}
static int ata_read(u32 lba, void *buf, int nsec) {
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
static int ata_write(u32 lba, const void *buf, int nsec) {
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
static void ahci_flush(void) {                            /* FLUSH CACHE via AHCI after writes */
    if (!ahci_ok || !hp) return;
    u8 *cfis = ahci_ct.cfis;
    for (int i = 0; i < 16; i++) cfis[i] = 0;
    cfis[0] = 0x27; cfis[1] = 0x80; cfis[2] = 0xE7;       /* FLUSH CACHE */
    ahci_cl.addr0 = (u32)&ahci_ct;
    ahci_cl.addr1 = 0;
    ahci_cl.opts = 0x5 | (1u << 16);                        /* write direction irrelevant; 1 empty PRD */
    ahci_cl.status = 0;
    ahci_ct.prdt[0].dba = 0; ahci_ct.prdt[0].dbau = 0;
    ahci_ct.prdt[0].rsv = 0; ahci_ct.prdt[0].dw3 = 0;
    mmio_wr32(&hp[4], 0);
    mmio_wr32(&hp[14], 1);
    for (int t = 0; t < 4000; t++) {
        if (!(mmio_rd32(&hp[14]) & 1)) return;
        iosleep(1);
    }
}
int disk_read(u32 lba, void *buf, int nsec) {
    if (ahci_ok) return ahci_issue(lba, buf, nsec, 0);
    return ata_read(lba, buf, nsec);
}
int disk_write(u32 lba, const void *buf, int nsec) {
    if (ahci_ok) {
        int ok = ahci_issue(lba, (void *)buf, nsec, 1);
        if (ok) ahci_flush();
        return ok;
    }
    return ata_write(lba, buf, nsec);
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
    dbg_puts("KTUYOME " KT_VERSION ": boot\n");
    dbg_init();
    pic_init(); idt_init(); pit_init();
    input_init();
    __asm__ volatile("sti");
    storage_init();
    net_init();
}
