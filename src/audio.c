#include "ktuyome.h"
/* audio.c : sound output.
 *
 *  - Intel AC'97 (ICH) PCM driver: 48 kHz, 16-bit stereo, bus-master DMA through a buffer descriptor list.
 *    This is the "Intel ICH AC97" card in VirtualBox and `-device AC97` in QEMU.
 *  - PC speaker fallback (PIT channel 2), used when no supported sound card is found.
 *  - A tiny additive synth that renders the system sounds (startup, shutdown, navigation ticks, notifications,
 *    success and error beeps, ...) from note tables, so no sample files are needed.
 *
 * Everything is polled: no sound interrupts are used. Playback is non-blocking; snd_play() renders the sound
 * into a static buffer, points the DMA engine at it and returns immediately.
 */

/* ------------------------------ PCI helpers ------------------------------ */
static u32 apci_rd(int b, int d, int f, int off) { outl(0xCF8, 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC)); return inl(0xCFC); }
static void apci_wr(int b, int d, int f, int off, u32 v) { outl(0xCF8, 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC)); outl(0xCFC, v); }
static inline void mem_barrier(void) { __asm__ volatile("" ::: "memory"); }

/* ------------------------------ synth ------------------------------ */
#define RATE   48000
#define MAXMS  1600
#define MAXFR  (RATE / 1000 * MAXMS)

enum { W_SINE, W_TRI, W_SQ, W_NOISE, W_BELL };      /* waveforms */
enum { E_FLAT, E_DECAY };                            /* envelopes: flat = short attack/release, decay = struck bell */
typedef struct { u16 at, dur, f0, f1; u8 vol, wave, env; } Note;   /* at/dur in ms, frequency glides f0 -> f1 */
typedef struct { const Note *n; u8 cnt, prio; } Snd;

#define NOTES(x) x, (u8)(sizeof(x) / sizeof((x)[0]))
/* frequencies: C4 262  G4 392  A4 440  C5 523  D5 587  E5 659  G5 784  A5 880  B5 988  C6 1047  E6 1319 */
static const Note n_startup[] = {
    {   0, 1000, 262, 262,  50, W_SINE, E_DECAY },
    {   0,  520, 523, 523, 110, W_BELL, E_DECAY }, { 120,  520, 659, 659, 110, W_BELL, E_DECAY },
    { 240,  560, 784, 784, 110, W_BELL, E_DECAY }, { 380,  900, 1047, 1047, 120, W_BELL, E_DECAY } };
static const Note n_shutdown[] = {
    {   0,  480, 1047, 1047, 100, W_BELL, E_DECAY }, { 130, 480, 784, 784, 100, W_BELL, E_DECAY },
    { 260,  520, 659, 659, 100, W_BELL, E_DECAY },   { 390, 800, 523, 523, 110, W_BELL, E_DECAY },
    { 390,  800, 262, 262,  60, W_SINE, E_DECAY } };
static const Note n_nav[]    = { { 0, 24, 1500, 1200, 46, W_SINE, E_DECAY } };
static const Note n_tab[]    = { { 0, 30,  880, 1100, 56, W_TRI,  E_DECAY } };
static const Note n_bump[]   = { { 0, 55,  200,  150, 90, W_TRI,  E_FLAT } };
static const Note n_open[]   = { { 0, 60, 659, 659, 70, W_SINE, E_FLAT }, { 55, 100, 880, 1047, 70, W_SINE, E_DECAY } };
static const Note n_back[]   = { { 0, 60, 880, 880, 70, W_SINE, E_FLAT }, { 55, 100, 659, 523, 70, W_SINE, E_DECAY } };
static const Note n_notify[] = { { 0, 280, 880, 880, 84, W_BELL, E_DECAY }, { 95, 320, 1319, 1319, 70, W_BELL, E_DECAY } };
static const Note n_ok[]     = { { 0, 110, 784, 784, 84, W_BELL, E_DECAY }, { 90, 300, 1047, 1047, 96, W_BELL, E_DECAY } };
static const Note n_error[]  = { { 0, 120, 330, 330, 100, W_SQ, E_FLAT }, { 160, 220, 220, 196, 100, W_SQ, E_FLAT } };
static const Note n_warn[]   = { { 0, 100, 587, 587, 100, W_TRI, E_FLAT }, { 120, 170, 440, 440, 100, W_TRI, E_DECAY } };
static const Note n_shot[]   = { { 0, 22, 3000, 3000, 90, W_NOISE, E_DECAY }, { 60, 55, 3000, 3000, 70, W_NOISE, E_DECAY },
                                 { 0, 140, 1800, 900, 36, W_SINE, E_DECAY } };
static const Note n_panic[]  = { { 0, 200, 880, 880, 120, W_SQ, E_FLAT }, { 260, 200, 660, 660, 120, W_SQ, E_FLAT },
                                 { 520, 420, 440, 440, 120, W_SQ, E_FLAT } };

static const Snd snds[SND_COUNT] = {            /* priority: a quieter sound never cuts off a louder one */
    [SND_STARTUP]  = { NOTES(n_startup),  5 }, [SND_SHUTDOWN] = { NOTES(n_shutdown), 5 },
    [SND_NAV]      = { NOTES(n_nav),      1 }, [SND_TAB]      = { NOTES(n_tab),      1 }, [SND_BUMP]  = { NOTES(n_bump), 1 },
    [SND_OPEN]     = { NOTES(n_open),     2 }, [SND_BACK]     = { NOTES(n_back),     2 },
    [SND_NOTIFY]   = { NOTES(n_notify),   2 }, [SND_OK]       = { NOTES(n_ok),       3 },
    [SND_ERROR]    = { NOTES(n_error),    4 }, [SND_WARN]     = { NOTES(n_warn),     3 },
    [SND_SHOT]     = { NOTES(n_shot),     4 }, [SND_PANIC]    = { NOTES(n_panic),    6 },
};

static s16 sintab[1024];
static s16 pcm[MAXFR * 2] __attribute__((aligned(4096)));     /* stereo, interleaved */

static int snd_ms(const Snd *s) {
    int e = 0; for (int i = 0; i < s->cnt; i++) e = imax(e, s->n[i].at + s->n[i].dur);
    return e;
}
static s32 wave(int w, u32 ph, u32 *rng) {
    switch (w) {
        case W_TRI: { int t = (int)(ph >> 16); return t < 32768 ? t * 2 - 32768 : (65535 - t) * 2 - 32768; }
        case W_SQ:  return ((ph >> 31) ? -15000 : 15000) + (sintab[ph >> 22] >> 1);
        case W_NOISE: { u32 x = *rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *rng = x; return (s16)(x >> 16); }
        case W_BELL: return sintab[ph >> 22] * 5 / 8 + sintab[(ph << 1) >> 22] * 2 / 8;
        default:    return sintab[ph >> 22];
    }
}
/* Mix the whole sound into pcm[]; returns the number of frames (a short silent tail keeps DACs from popping). */
static int render(const Snd *s) {
    int frames = imin(snd_ms(s) * (RATE / 1000) + 960, MAXFR);
    memset(pcm, 0, (u32)frames * 4);
    for (int k = 0; k < s->cnt; k++) {
        const Note *nt = &s->n[k];
        int start = nt->at * (RATE / 1000), n = nt->dur * (RATE / 1000);
        if (start + n > frames) n = frames - start;
        u32 ph = 0, rng = 0x9E3779B9u + nt->at * 7919u;
        int att = nt->env == E_DECAY ? 96 : 192, rel = 480;
        for (int i = 0; i < n; i++) {
            int f = nt->f0 + ((int)nt->f1 - (int)nt->f0) * i / n;
            ph += (u32)f * 89478u + (u32)f * 31u / 64u;                 /* 2^32 / 48000 = 89478.485 */
            int g;
            if (nt->env == E_DECAY) { int d = (n - i) * 256 / n; g = d * d >> 8; if (i < att) g = g * i / att; }
            else { int a = i < att ? i * 256 / att : 256, r = n - i < rel ? (n - i) * 256 / rel : 256; g = imin(a, r); }
            s32 v = (wave(nt->wave, ph, &rng) * nt->vol >> 8) * g >> 8;
            s16 *o = &pcm[(start + i) * 2];
            s32 m = o[0] + v; o[0] = (s16)iclamp(m, -32768, 32767);
            o[1] = o[0];
        }
    }
    return frames;
}

/* ------------------------------ AC'97 ------------------------------ */
enum { DEV_NONE, DEV_AC97, DEV_SPEAKER };
static int dev, nam, nabm;
static char dev_name[64] = "None";

/* Bus-master registers (NABM) for the PCM-out engine */
#define PO_BDBAR 0x10
#define PO_LVI   0x15
#define PO_SR    0x16
#define PO_CR    0x1B
#define GLOB_CNT 0x2C
#define GLOB_STA 0x30
typedef struct { u32 addr; u16 len; u16 flags; } __attribute__((packed)) Bd;
static Bd bdl[32] __attribute__((aligned(4096)));

static int wait_ms(int ms, int (*cond)(void)) {
    u32 t0 = ticks();
    while (!cond()) { if ((int)(ticks() - t0) > ms) return 0; }
    return 1;
}
static int c_codec_ready(void) { return (inl(nabm + GLOB_STA) & 0x100) != 0; }
static int c_analog_ready(void) { return (inw(nam + 0x26) & 0x0F) == 0x0F; }

static void ac97_volume(int v) {
    int att = (100 - iclamp(v, 0, 100)) * 31 / 100;
    u16 reg = v <= 0 ? 0x8000 : (u16)(att | (att << 8));
    outw(nam + 0x02, reg);                 /* master */
    outw(nam + 0x04, reg);                 /* headphones (no-op on codecs without it) */
    outw(nam + 0x18, 0x0808);              /* PCM out: 0 dB gain */
}
static int ac97_probe(void) {
    for (int b = 0; b < 8; b++) for (int d = 0; d < 32; d++) for (int f = 0; f < 8; f++) {
        u32 id = apci_rd(b, d, f, 0);
        if ((u16)id == 0xFFFF) { if (f == 0) break; continue; }
        u32 cls = apci_rd(b, d, f, 8) >> 16;                      /* class:subclass */
        if (cls != 0x0401 || (u16)id != 0x8086) continue;        /* Intel multimedia audio (not HDA 0x0403) */
        u32 b0 = apci_rd(b, d, f, 0x10), b1 = apci_rd(b, d, f, 0x14);
        if (!(b0 & 1) || !(b1 & 1)) continue;                     /* both BARs must be I/O space */
        apci_wr(b, d, f, 4, apci_rd(b, d, f, 4) | 5);             /* I/O decode + bus master */
        nam = (int)(b0 & 0xFFFC); nabm = (int)(b1 & 0xFFFC);
        outl(nabm + GLOB_CNT, 0x02);                              /* release cold reset */
        wait_ms(500, c_codec_ready);
        outw(nam + 0x00, 0);                                      /* codec reset */
        wait_ms(300, c_analog_ready);
        outb(nabm + PO_CR, 0x02);                                 /* reset PCM-out engine */
        for (int i = 0; i < 1000 && (inb(nabm + PO_CR) & 2); i++) ;
        ac97_volume(cfg.volume);
        kstrcpy(dev_name, "Intel AC'97 audio (48 kHz, 16-bit stereo)", sizeof dev_name);
        return 1;
    }
    return 0;
}
static void ac97_play(int frames) {
    outb(nabm + PO_CR, 0);                                        /* stop */
    outb(nabm + PO_CR, 0x02);                                     /* reset registers */
    for (int i = 0; i < 1000 && (inb(nabm + PO_CR) & 2); i++) ;
    int samples = frames * 2, d = 0; const s16 *p = pcm;
    while (samples > 0 && d < 32) {
        int n = samples > 0xFFFC ? 0xFFFC : samples;
        bdl[d].addr = (u32)p; bdl[d].len = (u16)n; bdl[d].flags = 0;
        p += n; samples -= n; d++;
    }
    mem_barrier();
    outl(nabm + PO_BDBAR, (u32)bdl);
    outb(nabm + PO_LVI, (u8)(d - 1));
    outw(nabm + PO_SR, 0x1C);                                     /* clear status flags */
    outb(nabm + PO_CR, 0x01);                                     /* run */
}
static void ac97_stop(void) { outb(nabm + PO_CR, 0x00); }

/* ------------------------------ PC speaker ------------------------------ */
static void spk_freq(int hz) {
    if (hz < 20) { outb(0x61, inb(0x61) & ~3); return; }
    u32 div = 1193182u / (u32)hz;
    outb(0x43, 0xB6); outb(0x42, (u8)div); outb(0x42, (u8)(div >> 8));
    outb(0x61, inb(0x61) | 3);
}
static const Snd *spk_cur; static u32 spk_t0;
static void spk_update(void) {
    if (!spk_cur) return;
    int el = (int)(ticks() - spk_t0);
    if (el >= snd_ms(spk_cur)) { spk_freq(0); spk_cur = NULL; return; }
    const Note *best = NULL;                                      /* one voice only: the newest (then loudest) active note wins */
    for (int k = 0; k < spk_cur->cnt; k++) {
        const Note *n = &spk_cur->n[k];
        if (el < n->at || el >= n->at + n->dur) continue;
        if (!best || n->at > best->at || (n->at == best->at && n->vol > best->vol)) best = n;
    }
    if (best) spk_freq(best->f0 + ((int)best->f1 - best->f0) * (el - best->at) / best->dur); else spk_freq(0);
}
static void spin_ms(int ms) { for (int i = 0; i < ms * 1000; i++) outb(0x80, 0); }   /* crude: only for the panic screen */

/* ------------------------------ public API ------------------------------ */
static u32 busy_until, last_start; static int cur_prio;

void snd_init(void) {
    for (int i = 0; i < 1024; i++) sintab[i] = (s16)(k_sin(i * 6.2831853f / 1024.0f) * 32767.0f);
    if (ac97_probe()) dev = DEV_AC97;
    else { dev = DEV_SPEAKER; kstrcpy(dev_name, "PC speaker (no sound card found)", sizeof dev_name); spk_freq(0); }
    dbg_puts("AUDIO: "); dbg_puts(dev_name); dbg_puts("\n");
}
const char *snd_device_name(void) { return dev_name; }
int  snd_device(void) { return dev; }
int  snd_busy(void) { return (int)(busy_until - ticks()) > 0; }
void snd_volume(void) { if (dev == DEV_AC97) ac97_volume(cfg.volume); }
void snd_tick(void) { if (dev == DEV_SPEAKER) spk_update(); }
static void play(int id, int force) {
    if ((!force && !cfg.sound) || dev == DEV_NONE || id < 0 || id >= SND_COUNT) return;
    if (dev == DEV_SPEAKER && cfg.volume <= 0) return;
    const Snd *s = &snds[id]; u32 now = ticks();
    if (snd_busy() && s->prio < cur_prio) return;                 /* never cut a more important sound short */
    if (s->prio <= 1 && now - last_start < 45) return;            /* keep rapid ticks from turning into a buzz */
    cur_prio = s->prio; last_start = now;
    if (dev == DEV_AC97) { int fr = render(s); ac97_play(fr); busy_until = now + (u32)(fr / (RATE / 1000)); }
    else { spk_cur = s; spk_t0 = now; busy_until = now + (u32)snd_ms(s); spk_update(); }
}
void snd_play(int id) { play(id, 0); }
void snd_test(int id) { play(id, 1); }                            /* ignores the "system sounds" switch (Settings test buttons) */
void snd_stop(void) { if (dev == DEV_AC97) ac97_stop(); else if (dev == DEV_SPEAKER) { spk_cur = NULL; spk_freq(0); } busy_until = 0; }
void snd_wait(int max_ms) {                                       /* block until the current sound ends (shutdown / restart) */
    u32 t0 = ticks();
    while (snd_busy() && (int)(ticks() - t0) < max_ms) { snd_tick(); __asm__ volatile("hlt"); }
    snd_tick();
}
/* Panic screen: interrupts and the clock cannot be trusted here, so play without them. */
void snd_panic(void) {
    if (dev == DEV_AC97) { int fr = render(&snds[SND_PANIC]); ac97_play(fr); }
    else if (dev == DEV_SPEAKER && cfg.volume > 0) {
        const Snd *s = &snds[SND_PANIC];
        for (int k = 0; k < s->cnt; k++) { spk_freq(s->n[k].f0); spin_ms(s->n[k].dur); spk_freq(0); spin_ms(60); }
    }
}
/* Pick a sound for a toast message: failures get the error beep, plain confirmations a soft chime. */
static int has(const char *s, const char *pat) {
    int n = (int)strlen(pat);
    for (; *s; s++) {
        int i = 0;
        while (i < n && s[i] && ((s[i] | 32) == (pat[i] | 32))) i++;
        if (i == n) return 1;
    }
    return 0;
}
int snd_for_message(const char *m) {
    static const char *bad[] = { "fail", "could not", "cannot", "can't", "can not", "not enough", "too big", "too large", "full",
        "nothing", "select a", "select files", "unsupported", "damage", "already exists", "taken", "error", "read-only", "invalid",
        "no room", "empty", "not a ", "not found", "denied", "refused", "timed out", "unable" };
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; i++) if (has(m, bad[i])) return SND_ERROR;
    static const char *good[] = { "saved", "created", "deleted", "zipped", "extracted", "moved", "pasted", "copied", "reset", "downloaded" };
    for (u32 i = 0; i < sizeof good / sizeof good[0]; i++) if (has(m, good[i])) return SND_OK;
    return SND_NOTIFY;
}
u32 audio_static_bytes(void) { return (u32)(sizeof pcm + sizeof bdl + sizeof sintab); }
