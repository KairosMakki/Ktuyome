#include "ktuyome.h"
/* Ktuyome network stack: PCI + Intel e1000 driver (polled), ARP, IPv4, ICMP, UDP, DHCP client, DNS resolver, TCP client.
 *
 * Works behind any NAT: QEMU user networking (10.0.2.0/24, gateway 10.0.2.2, DNS 10.0.2.3), VirtualBox NAT, etc.
 * The address, gateway and DNS server all come from DHCP. Everything is polled from net_poll() once per main-loop pass.
 * One IRQ-time hook (net_isr_stamp) timestamps frames as they land in the RX ring so ping times are not inflated
 * by however long the UI took to draw a frame.
 */

NetInfo net;

static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 bs16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
static inline u32 bs32(u32 x) { return __builtin_bswap32(x); }

/* ============================ PCI ============================ */
static u32 pci_rd(int b, int d, int f, int off) { outl(0xCF8, 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC)); return inl(0xCFC); }
static void pci_wr(int b, int d, int f, int off, u32 v) { outl(0xCF8, 0x80000000u | (b << 16) | (d << 11) | (f << 8) | (off & 0xFC)); outl(0xCFC, v); }

/* ============================ Intel e1000 ============================ */
#define NRX 32
#define NTX 16
#define BUFSZ 2048
typedef struct { u32 lo, hi; u16 len, csum; u8 status, errors; u16 special; } __attribute__((packed)) RxDesc;
typedef struct { u32 lo, hi; u16 len; u8 cso, cmd, status, css; u16 special; } __attribute__((packed)) TxDesc;
static RxDesc rxd[NRX] __attribute__((aligned(16)));
static TxDesc txd[NTX] __attribute__((aligned(16)));
static u8 rxbuf[NRX][BUFSZ] __attribute__((aligned(16)));
static u8 txbuf[NTX][BUFSZ] __attribute__((aligned(16)));
static volatile u32 *mmio; static int rx_cur, tx_cur;
static volatile u32 rx_stamp[NRX]; static volatile u8 rx_stamp_ok[NRX];
static u32 rx_time;                                   /* arrival time (ms) of the frame being processed */

static inline u32 rd(u32 r) { return mmio[r / 4]; }
static inline void wr(u32 r, u32 v) { mmio[r / 4] = v; }

void net_isr_stamp(void) {                            /* called from the 1 kHz timer interrupt */
    if (!net.present) return;
    u32 t = ticks();
    for (int i = 0; i < NRX; i++) if ((rxd[i].status & 1) && !rx_stamp_ok[i]) { rx_stamp[i] = t; rx_stamp_ok[i] = 1; }
}

static int e1000_supported(u16 id) {
    static const u16 ids[] = { 0x1000, 0x1001, 0x1004, 0x1008, 0x100C, 0x100D, 0x100E, 0x100F, 0x1010, 0x1011, 0x1012, 0x1013,
        0x1015, 0x1016, 0x1017, 0x1018, 0x1019, 0x101A, 0x101D, 0x101E, 0x1026, 0x1027, 0x1028, 0x10D3 };
    for (u32 i = 0; i < sizeof ids / sizeof ids[0]; i++) if (ids[i] == id) return 1;
    return 0;
}
static void hex4(char *d, u16 v) { static const char h[] = "0123456789abcdef"; for (int i = 0; i < 4; i++) d[i] = h[(v >> (12 - 4 * i)) & 15]; }

static int e1000_init(void) {
    int fb = -1, fd = 0, ff = 0; u16 did = 0;
    for (int b = 0; b < 8 && fb < 0; b++) for (int d = 0; d < 32 && fb < 0; d++) for (int f = 0; f < 8; f++) {
        u32 id = pci_rd(b, d, f, 0);
        if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
        if ((id & 0xFFFF) == 0x8086 && e1000_supported((u16)(id >> 16))) { fb = b; fd = d; ff = f; did = (u16)(id >> 16); break; }
    }
    if (fb < 0) return 0;
    pci_wr(fb, fd, ff, 4, pci_rd(fb, fd, ff, 4) | 7);               /* I/O, memory, bus master */
    u32 bar = pci_rd(fb, fd, ff, 0x10);
    if (bar & 1) return 0;
    mmio = (volatile u32 *)(bar & ~15u);
    wr(0x00D8, 0xFFFFFFFF);                                          /* IMC: no interrupts, we poll */
    wr(0x0000, rd(0x0000) | (1u << 26));                             /* CTRL.RST */
    for (volatile int i = 0; i < 2000000 && (rd(0x0000) & (1u << 26)); i++) {}
    wr(0x00D8, 0xFFFFFFFF);
    wr(0x0000, (rd(0x0000) | (1u << 6) | (1u << 5)) & ~(1u << 3));  /* SLU, ASDE, clear LRST */
    u32 ral = rd(0x5400), rah = rd(0x5404);
    net.mac[0] = (u8)ral; net.mac[1] = (u8)(ral >> 8); net.mac[2] = (u8)(ral >> 16); net.mac[3] = (u8)(ral >> 24);
    net.mac[4] = (u8)rah; net.mac[5] = (u8)(rah >> 8);
    for (int i = 0; i < 128; i++) wr(0x5200 + i * 4, 0);             /* clear multicast table */
    for (int i = 0; i < NRX; i++) { memset(&rxd[i], 0, sizeof rxd[i]); rxd[i].lo = (u32)rxbuf[i]; }
    wr(0x2800, (u32)rxd); wr(0x2804, 0); wr(0x2808, sizeof rxd); wr(0x2810, 0); wr(0x2818, NRX - 1);
    wr(0x0100, (1u << 1) | (1u << 15) | (1u << 26));                 /* RCTL: EN, BAM, strip CRC, 2 KB buffers */
    for (int i = 0; i < NTX; i++) { memset(&txd[i], 0, sizeof txd[i]); txd[i].lo = (u32)txbuf[i]; txd[i].status = 1; }
    wr(0x3800, (u32)txd); wr(0x3804, 0); wr(0x3808, sizeof txd); wr(0x3810, 0); wr(0x3818, 0);
    wr(0x0410, 0x0060200A);                                          /* TIPG */
    wr(0x0400, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));/* TCTL: EN, PSP, CT, COLD */
    kstrcpy(net.nic, "Intel PRO/1000 (e1000) 8086:", sizeof net.nic);
    int n = (int)strlen(net.nic); hex4(net.nic + n, did); net.nic[n + 4] = 0;
    return 1;
}
static void nic_send(const u8 *f, int len) {
    if (!net.present) return;
    if (!(txd[tx_cur].status & 1)) { net.tx_drop++; return; }
    if (len > 1514) return;
    memcpy(txbuf[tx_cur], f, (u32)len);
    if (len < 60) { memset(txbuf[tx_cur] + len, 0, (u32)(60 - len)); len = 60; }
    txd[tx_cur].len = (u16)len; txd[tx_cur].cmd = 0x0B; txd[tx_cur].status = 0;
    tx_cur = (tx_cur + 1) % NTX;
    wr(0x3818, (u32)tx_cur);
    net.tx_pkts++; net.tx_bytes += (u32)len;
}

/* ============================ helpers ============================ */
static u32 csum_part(const void *d, int n, u32 s) {
    const u8 *p = d;
    while (n > 1) { s += (u32)((p[0] << 8) | p[1]); p += 2; n -= 2; }
    if (n) s += (u32)(p[0] << 8);
    return s;
}
static u16 csum_fin(u32 s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (u16)~s; }
static u32 pseudo(u32 src, u32 dst, int proto, int len) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + (u32)proto + (u32)len;
}
u32 net_ip(int a, int b, int c, int d) { return ((u32)a << 24) | ((u32)b << 16) | ((u32)c << 8) | (u32)d; }
void net_ip_str(char *out, u32 ip) {
    char t[8]; out[0] = 0;
    for (int i = 3; i >= 0; i--) { kitoa(t, (int)((ip >> (8 * i)) & 255)); kstrcat(out, t, 16); if (i) kstrcat(out, ".", 16); }
}
int net_parse_ip(const char *s, u32 *out) {
    u32 ip = 0; int parts = 0;
    for (;;) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++digits > 3 || v > 255) return 0; }
        ip = (ip << 8) | (u32)v; parts++;
        if (parts == 4) break;
        if (*s++ != '.') return 0;
    }
    if (*s) return 0;
    *out = ip; return 1;
}

/* ============================ Ethernet / ARP / IPv4 ============================ */
typedef struct __attribute__((packed)) { u8 dst[6], src[6]; u16 type; } Eth;
typedef struct __attribute__((packed)) { u8 vihl, tos; u16 len, id, frag; u8 ttl, proto; u16 csum; u32 src, dst; } Ip4;
typedef struct __attribute__((packed)) { u16 htype, ptype; u8 hlen, plen; u16 op; u8 sha[6]; u32 spa; u8 tha[6]; u32 tpa; } Arp;
#define BCAST_MAC ((const u8[6]){ 255, 255, 255, 255, 255, 255 })

NetArp net_arp[8];
static u8 txf[1600];                                  /* frame under construction */
static u8 pend[1600]; static int pend_len; static u32 pend_ip, pend_t; static int pend_tries;

static void eth_send(const u8 *dmac, int type, int len) {   /* payload already at txf+14 */
    Eth *e = (Eth *)txf;
    memcpy(e->dst, dmac, 6); memcpy(e->src, net.mac, 6); e->type = bs16((u16)type);
    nic_send(txf, len + 14);
}
static const u8 *arp_find(u32 ip) {
    for (int i = 0; i < 8; i++) if (net_arp[i].valid && net_arp[i].ip == ip) return net_arp[i].mac;
    return NULL;
}
static void arp_learn(u32 ip, const u8 *mac) {
    int slot = -1;
    for (int i = 0; i < 8; i++) if (net_arp[i].valid && net_arp[i].ip == ip) slot = i;
    if (slot < 0) for (int i = 0; i < 8; i++) if (!net_arp[i].valid) { slot = i; break; }
    if (slot < 0) { slot = 0; for (int i = 1; i < 8; i++) if (net_arp[i].t < net_arp[slot].t) slot = i; }
    net_arp[slot].valid = 1; net_arp[slot].ip = ip; memcpy(net_arp[slot].mac, mac, 6); net_arp[slot].t = ticks();
}
static void arp_request(u32 ip) {
    Arp *a = (Arp *)(txf + 14);
    a->htype = bs16(1); a->ptype = bs16(0x0800); a->hlen = 6; a->plen = 4; a->op = bs16(1);
    memcpy(a->sha, net.mac, 6); a->spa = bs32(net.ip); memset(a->tha, 0, 6); a->tpa = bs32(ip);
    eth_send(BCAST_MAC, 0x0806, sizeof(Arp));
}
static u16 ip_id;
static void ip_out(u32 dst, int proto, int ttl, const u8 *payload, int len) {
    if (!net.present || len > 1480) return;
    static u8 pk[1600];
    Ip4 *ip = (Ip4 *)pk;
    ip->vihl = 0x45; ip->tos = 0; ip->len = bs16((u16)(20 + len)); ip->id = bs16(ip_id++); ip->frag = 0;
    ip->ttl = (u8)ttl; ip->proto = (u8)proto; ip->csum = 0; ip->src = bs32(net.ip); ip->dst = bs32(dst);
    ip->csum = bs16(csum_fin(csum_part(ip, 20, 0)));
    memcpy(pk + 20, payload, (u32)len);
    int total = 20 + len;
    int bcast = dst == 0xFFFFFFFFu || (net.mask && dst == ((net.ip & net.mask) | ~net.mask));
    if (bcast) { memcpy(txf + 14, pk, (u32)total); eth_send(BCAST_MAC, 0x0800, total); return; }
    u32 hop = ((dst & net.mask) == (net.ip & net.mask)) ? dst : net.gw;
    if (!hop) return;                                    /* off-link and no gateway */
    const u8 *mac = arp_find(hop);
    if (mac) { memcpy(txf + 14, pk, (u32)total); eth_send(mac, 0x0800, total); return; }
    memcpy(pend, pk, (u32)total); pend_len = total; pend_ip = hop; pend_t = ticks(); pend_tries = 1;
    arp_request(hop);
}

/* ============================ ICMP ============================ */
static NetIcmpEv ievq[16]; static int iev_w, iev_r;
static void icmp_push(int type, int code, u32 src, int id, int seq, int ttl) {
    NetIcmpEv *e = &ievq[iev_w]; int nw = (iev_w + 1) & 15; if (nw == iev_r) return;
    e->type = (u8)type; e->code = (u8)code; e->src = src; e->id = (u16)id; e->seq = (u16)seq; e->ttl = (u8)ttl; e->t = rx_time;
    iev_w = nw;
}
int net_icmp_pop(NetIcmpEv *e) {
    if (iev_r == iev_w) return 0;
    *e = ievq[iev_r]; iev_r = (iev_r + 1) & 15; return 1;
}
int net_icmp_echo(u32 dst, int ttl, int id, int seq, int plen) {
    if ((dst >> 24) == 127 || (net.ip && dst == net.ip)) { rx_time = ticks(); icmp_push(0, 0, dst, id, seq, 64); net.icmp_tx++; return 1; }
    u8 m[8 + 128]; if (plen > 128) plen = 128;
    m[0] = 8; m[1] = 0; m[2] = m[3] = 0; m[4] = (u8)(id >> 8); m[5] = (u8)id; m[6] = (u8)(seq >> 8); m[7] = (u8)seq;
    for (int i = 0; i < plen; i++) m[8 + i] = (u8)('a' + i % 23);
    u16 c = csum_fin(csum_part(m, 8 + plen, 0)); m[2] = (u8)(c >> 8); m[3] = (u8)c;
    ip_out(dst, 1, ttl, m, 8 + plen);
    net.icmp_tx++;
    return 1;
}
static void icmp_rx(u32 src, int ttl, const u8 *p, int len) {
    if (len < 8) return;
    if (csum_fin(csum_part(p, len, 0)) != 0) return;
    int type = p[0];
    net.icmp_rx++;
    if (type == 8) {                                      /* echo request -> reply */
        static u8 r[1500]; if (len > 1480) return;
        memcpy(r, p, (u32)len); r[0] = 0; r[2] = r[3] = 0;
        u16 c = csum_fin(csum_part(r, len, 0)); r[2] = (u8)(c >> 8); r[3] = (u8)c;
        ip_out(src, 1, 64, r, len);
    } else if (type == 0) {
        icmp_push(0, 0, src, (p[4] << 8) | p[5], (p[6] << 8) | p[7], ttl);
    } else if ((type == 3 || type == 11) && len >= 8 + 20 + 8) {
        const u8 *in = p + 8; int ihl = (in[0] & 15) * 4;
        if (in[9] == 1 && len >= 8 + ihl + 8 && in[ihl] == 8)
            icmp_push(type, p[1], src, (in[ihl + 4] << 8) | in[ihl + 5], (in[ihl + 6] << 8) | in[ihl + 7], 0);
    }
}

/* ============================ UDP ============================ */
#define NUDP 6
typedef struct { int used, has; u16 port, from_port; u32 from_ip; int len; u8 buf[1024]; } Udp;
static Udp udps[NUDP]; static u16 next_port = 49152;
int udp_open(u16 port) {
    for (int i = 0; i < NUDP; i++) if (!udps[i].used) {
        memset(&udps[i], 0, sizeof udps[i]); udps[i].used = 1;
        udps[i].port = port ? port : next_port++; if (next_port < 49152) next_port = 49152;
        return i;
    }
    return -1;
}
void udp_close(int s) { if (s >= 0 && s < NUDP) udps[s].used = 0; }
int udp_send(int s, u32 ip, u16 port, const void *data, int len) {
    if (s < 0 || !udps[s].used || len > 1400) return 0;
    static u8 d[1500]; int n = 8 + len;
    d[0] = (u8)(udps[s].port >> 8); d[1] = (u8)udps[s].port; d[2] = (u8)(port >> 8); d[3] = (u8)port;
    d[4] = (u8)(n >> 8); d[5] = (u8)n; d[6] = d[7] = 0;
    memcpy(d + 8, data, (u32)len);
    u16 c = csum_fin(csum_part(d, n, pseudo(net.ip, ip, 17, n))); if (!c) c = 0xFFFF;
    d[6] = (u8)(c >> 8); d[7] = (u8)c;
    ip_out(ip, 17, 64, d, n); net.udp_tx++;
    return 1;
}
int udp_recv(int s, void *buf, int cap, u32 *from_ip, u16 *from_port) {
    if (s < 0 || !udps[s].used || !udps[s].has) return -1;
    int n = imin(cap, udps[s].len); memcpy(buf, udps[s].buf, (u32)n);
    if (from_ip) *from_ip = udps[s].from_ip;
    if (from_port) *from_port = udps[s].from_port;
    udps[s].has = 0; return n;
}
static void udp_rx(u32 src, u32 dst, const u8 *p, int len) {
    if (len < 8) return;
    int n = (p[4] << 8) | p[5]; if (n < 8 || n > len) return;
    if (p[6] || p[7]) { if (csum_fin(csum_part(p, n, pseudo(src, dst, 17, n))) != 0) return; }
    u16 dport = (u16)((p[2] << 8) | p[3]);
    net.udp_rx++;
    for (int i = 0; i < NUDP; i++) if (udps[i].used && udps[i].port == dport) {
        int l = imin(n - 8, (int)sizeof udps[i].buf);
        memcpy(udps[i].buf, p + 8, (u32)l); udps[i].len = l; udps[i].has = 1;
        udps[i].from_ip = src; udps[i].from_port = (u16)((p[0] << 8) | p[1]);
        return;
    }
}

/* ============================ DHCP client ============================ */
static int dh_sock = -1, dh_tries; static u32 dh_xid, dh_offer, dh_server, dh_t;
static void dhcp_send(int type) {
    u8 m[320]; memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;
    m[4] = (u8)(dh_xid >> 24); m[5] = (u8)(dh_xid >> 16); m[6] = (u8)(dh_xid >> 8); m[7] = (u8)dh_xid;
    m[10] = 0x80;                                          /* broadcast flag */
    memcpy(m + 28, net.mac, 6);
    m[236] = 0x63; m[237] = 0x82; m[238] = 0x53; m[239] = 0x63;
    int o = 240;
    m[o++] = 53; m[o++] = 1; m[o++] = (u8)type;
    if (type == 3) {
        m[o++] = 50; m[o++] = 4; m[o++] = (u8)(dh_offer >> 24); m[o++] = (u8)(dh_offer >> 16); m[o++] = (u8)(dh_offer >> 8); m[o++] = (u8)dh_offer;
        m[o++] = 54; m[o++] = 4; m[o++] = (u8)(dh_server >> 24); m[o++] = (u8)(dh_server >> 16); m[o++] = (u8)(dh_server >> 8); m[o++] = (u8)dh_server;
    }
    m[o++] = 55; m[o++] = 3; m[o++] = 1; m[o++] = 3; m[o++] = 6;
    m[o++] = 255;
    udp_send(dh_sock, 0xFFFFFFFFu, 67, m, 300);                    /* source is 0.0.0.0 until the lease is bound */
}
void net_dhcp_start(void) {
    if (!net.present) return;
    if (dh_sock < 0) dh_sock = udp_open(68);
    net.ip = net.mask = net.gw = net.dns = 0;
    for (int i = 0; i < 8; i++) net_arp[i].valid = 0;
    dh_xid = ticks() * 2654435761u ^ (u32)rdtsc(); dh_tries = 1; dh_t = ticks(); net.dhcp = DHCP_DISCOVER;
    dhcp_send(1);
}
void net_set_static(u32 ip, u32 mask, u32 gw, u32 dns) {
    net.ip = ip; net.mask = mask; net.gw = gw; net.dns = dns; net.dhcp = DHCP_STATIC;
    for (int i = 0; i < 8; i++) net_arp[i].valid = 0;
}
static void dhcp_poll(void) {
    if (dh_sock < 0 || net.dhcp == DHCP_STATIC) return;
    u8 m[600]; u32 from; u16 fp;
    int n = udp_recv(dh_sock, m, sizeof m, &from, &fp);
    if (n >= 241 && m[0] == 2 && m[236] == 0x63 && m[237] == 0x82 && m[238] == 0x53 && m[239] == 0x63 &&
        ((u32)m[4] << 24 | (u32)m[5] << 16 | (u32)m[6] << 8 | m[7]) == dh_xid) {
        u32 yi = (u32)m[16] << 24 | (u32)m[17] << 16 | (u32)m[18] << 8 | m[19];
        int type = 0; u32 mask = 0, gw = 0, dns = 0, sid = 0, lease = 0;
        for (int o = 240; o < n;) {
            int c = m[o++]; if (c == 255) break; if (c == 0) continue;
            if (o >= n) break; int l = m[o++]; if (o + l > n) break;
            u32 v4 = l >= 4 ? ((u32)m[o] << 24 | (u32)m[o + 1] << 16 | (u32)m[o + 2] << 8 | m[o + 3]) : 0;
            if (c == 53) type = m[o]; else if (c == 1) mask = v4; else if (c == 3) gw = v4; else if (c == 6) dns = v4;
            else if (c == 54) sid = v4; else if (c == 51) lease = v4;
            o += l;
        }
        if (type == 2 && net.dhcp == DHCP_DISCOVER) {
            dh_offer = yi; dh_server = sid ? sid : from; net.dhcp = DHCP_REQUEST; dh_tries = 1; dh_t = ticks(); dhcp_send(3);
        } else if (type == 5 && net.dhcp == DHCP_REQUEST) {
            net.ip = yi; net.mask = mask ? mask : 0xFFFFFF00u; net.gw = gw; net.dns = dns; net.lease = lease;
            net.dhcp_server = dh_server; net.dhcp = DHCP_BOUND;
        } else if (type == 6) net_dhcp_start();
    }
    if ((net.dhcp == DHCP_DISCOVER || net.dhcp == DHCP_REQUEST) && ticks() - dh_t > 2500) {
        if (++dh_tries > 5) { net.dhcp = DHCP_FAILED; return; }
        dh_t = ticks(); dhcp_send(net.dhcp == DHCP_DISCOVER ? 1 : 3);
    }
}

/* ============================ DNS resolver ============================ */
#define NDNS 4
typedef struct { int used, sock, tries, result; u16 id; u32 t, ip; char name[100]; } Dns;
static Dns dnsq[NDNS];
static void dns_send(Dns *d) {
    u8 q[200]; memset(q, 0, 12);
    q[0] = (u8)(d->id >> 8); q[1] = (u8)d->id; q[2] = 1; q[5] = 1;                 /* RD, QDCOUNT=1 */
    int o = 12; const char *s = d->name;
    while (*s) {
        const char *e = s; while (*e && *e != '.') e++;
        int l = (int)(e - s); if (l == 0 || l > 63 || o + l + 6 > (int)sizeof q) break;
        q[o++] = (u8)l; memcpy(q + o, s, (u32)l); o += l; s = *e ? e + 1 : e;
    }
    q[o++] = 0; q[o++] = 0; q[o++] = 1; q[o++] = 0; q[o++] = 1;                    /* TYPE A, CLASS IN */
    udp_send(d->sock, net.dns, 53, q, o);
    d->t = ticks();
}
int net_dns_start(const char *name) {
    for (int i = 0; i < NDNS; i++) if (!dnsq[i].used) {
        Dns *d = &dnsq[i]; memset(d, 0, sizeof *d); d->used = 1;
        kstrcpy(d->name, name, sizeof d->name);
        d->id = (u16)(ticks() ^ (u32)rdtsc() ^ (u32)(i * 7919));
        if (!net.dns) { d->result = -3; return i; }
        d->sock = udp_open(0); d->tries = 1; dns_send(d);
        return i;
    }
    return -1;
}
static int dns_skip(const u8 *m, int n, int o) {
    while (o < n) {
        int c = m[o];
        if (c == 0) return o + 1;
        if ((c & 0xC0) == 0xC0) return o + 2;
        o += c + 1;
    }
    return -1;
}
int net_dns_poll(int h, u32 *ip) {                      /* 1 resolved, 0 pending, -1 timeout, -2 no such host, -3 no server */
    if (h < 0 || h >= NDNS || !dnsq[h].used) return -1;
    Dns *d = &dnsq[h];
    if (d->result) { return d->result; }
    u8 m[1024]; int n = udp_recv(d->sock, m, sizeof m, NULL, NULL);
    if (n >= 12 && (((m[0] << 8) | m[1]) == d->id) && (m[2] & 0x80)) {
        int rcode = m[3] & 15, an = (m[6] << 8) | m[7], o = dns_skip(m, n, 12);
        if (rcode) { d->result = -2; goto done; }
        if (o < 0) { d->result = -2; goto done; }
        o += 4;
        for (int i = 0; i < an && o > 0 && o < n; i++) {
            o = dns_skip(m, n, o); if (o < 0 || o + 10 > n) break;
            int type = (m[o] << 8) | m[o + 1], rl = (m[o + 8] << 8) | m[o + 9]; o += 10;
            if (type == 1 && rl == 4 && o + 4 <= n) { d->ip = (u32)m[o] << 24 | (u32)m[o + 1] << 16 | (u32)m[o + 2] << 8 | m[o + 3]; d->result = 1; goto done; }
            o += rl;
        }
        d->result = -2;
    } else if (ticks() - d->t > 1500) {
        if (++d->tries > 3) d->result = -1; else dns_send(d);
    }
done:
    if (d->result == 1 && ip) *ip = d->ip;
    if (d->result) { udp_close(d->sock); return d->result; }
    return 0;
}
void net_dns_free(int h) { if (h >= 0 && h < NDNS && dnsq[h].used) { if (!dnsq[h].result) udp_close(dnsq[h].sock); dnsq[h].used = 0; } }
int net_dns_result_ip(int h, u32 *ip) { if (h >= 0 && h < NDNS && dnsq[h].used && dnsq[h].result == 1) { *ip = dnsq[h].ip; return 1; } return 0; }

/* ============================ TCP client ============================ */
enum { TS_FREE, TS_SYN, TS_EST, TS_FIN1, TS_FIN2, TS_CLOSEWAIT, TS_LASTACK, TS_CLOSING, TS_DONE, TS_REFUSED, TS_RESET, TS_TIMEOUT };
#define NTCP 4
#define RBUF 8192
#define SEG 1200
typedef struct {
    int state; u32 rip; u16 rport, lport;
    u32 snd_una, snd_nxt, rcv_nxt, iss;
    u8 rbuf[RBUF]; int rhead, rcount;
    u8 sbuf[SEG]; int slen;
    u32 t; int tries, fin_sent, fin_rcvd, want_close;
} Tcp;
static Tcp tcps[NTCP];

static void tcp_out(Tcp *t, u32 seq, int flags, const u8 *data, int len) {
    static u8 s[1500]; int hl = (flags & 2) ? 24 : 20;
    int win = RBUF - t->rcount;
    s[0] = (u8)(t->lport >> 8); s[1] = (u8)t->lport; s[2] = (u8)(t->rport >> 8); s[3] = (u8)t->rport;
    s[4] = (u8)(seq >> 24); s[5] = (u8)(seq >> 16); s[6] = (u8)(seq >> 8); s[7] = (u8)seq;
    u32 ack = (flags & 0x10) ? t->rcv_nxt : 0;
    s[8] = (u8)(ack >> 24); s[9] = (u8)(ack >> 16); s[10] = (u8)(ack >> 8); s[11] = (u8)ack;
    s[12] = (u8)((hl / 4) << 4); s[13] = (u8)flags; s[14] = (u8)(win >> 8); s[15] = (u8)win; s[16] = s[17] = s[18] = s[19] = 0;
    if (hl == 24) { s[20] = 2; s[21] = 4; s[22] = 0x05; s[23] = 0xB4; }
    if (len) memcpy(s + hl, data, (u32)len);
    int n = hl + len;
    u16 c = csum_fin(csum_part(s, n, pseudo(net.ip, t->rip, 6, n))); s[16] = (u8)(c >> 8); s[17] = (u8)c;
    ip_out(t->rip, 6, 64, s, n); net.tcp_tx++;
}
static void tcp_rst_reply(u32 src, u16 sport, u16 dport, u32 seq, u32 ack, int had_ack, int seglen) {
    Tcp t; memset(&t, 0, sizeof t); t.rip = src; t.rport = sport; t.lport = dport;
    if (had_ack) tcp_out(&t, ack, 0x04, NULL, 0);
    else { t.rcv_nxt = seq + (u32)seglen; tcp_out(&t, 0, 0x14, NULL, 0); }
}
static u16 tcp_port = 40000;
int tcp_open(u32 ip, u16 port) {
    if (!net.present || !net.ip) return -1;
    for (int i = 0; i < NTCP; i++) if (tcps[i].state == TS_FREE) {
        Tcp *t = &tcps[i]; memset(t, 0, sizeof *t);
        t->rip = ip; t->rport = port; t->lport = tcp_port++; if (tcp_port < 40000) tcp_port = 40000;
        t->iss = ticks() * 2654435761u ^ (u32)rdtsc(); t->snd_una = t->iss; t->snd_nxt = t->iss + 1;
        t->state = TS_SYN; t->t = ticks(); t->tries = 1;
        tcp_out(t, t->iss, 0x02, NULL, 0);
        return i;
    }
    return -1;
}
int tcp_status(int h) {
    if (h < 0 || h >= NTCP) return TCP_NONE;
    switch (tcps[h].state) {
        case TS_FREE: return TCP_NONE;
        case TS_SYN: return TCP_CONNECTING;
        case TS_EST: case TS_FIN1: case TS_FIN2: case TS_CLOSEWAIT: case TS_LASTACK: case TS_CLOSING: return TCP_OPEN;
        case TS_DONE: return TCP_CLOSED;
        case TS_REFUSED: return TCP_REFUSED;
        case TS_RESET: return TCP_RESET;
        default: return TCP_TIMEOUT;
    }
}
int tcp_send(int h, const void *data, int len) {          /* returns bytes accepted (0 = busy, try again) */
    if (h < 0 || h >= NTCP) return 0;
    Tcp *t = &tcps[h];
    if ((t->state != TS_EST && t->state != TS_CLOSEWAIT) || t->slen || t->want_close || len <= 0) return 0;
    int n = imin(len, SEG);
    memcpy(t->sbuf, data, (u32)n); t->slen = n;
    tcp_out(t, t->snd_nxt, 0x18, t->sbuf, n);
    t->snd_nxt += (u32)n; t->t = ticks(); t->tries = 1;
    return n;
}
int tcp_recv(int h, void *buf, int cap) {
    if (h < 0 || h >= NTCP) return 0;
    Tcp *t = &tcps[h]; u8 *o = buf; int n = imin(cap, t->rcount), was_full = t->rcount > RBUF / 2;
    for (int i = 0; i < n; i++) o[i] = t->rbuf[(t->rhead + i) % RBUF];
    t->rhead = (t->rhead + n) % RBUF; t->rcount -= n;
    if (n && was_full && (t->state == TS_EST || t->state == TS_FIN1 || t->state == TS_FIN2)) tcp_out(t, t->snd_nxt, 0x10, NULL, 0);   /* window update */
    return n;
}
int tcp_eof(int h) { return h >= 0 && h < NTCP && tcps[h].rcount == 0 && (tcps[h].fin_rcvd || tcp_status(h) >= TCP_CLOSED); }
void tcp_close(int h) { if (h >= 0 && h < NTCP && tcps[h].state != TS_FREE) tcps[h].want_close = 1; }
void tcp_free(int h) {
    if (h < 0 || h >= NTCP) return;
    Tcp *t = &tcps[h];
    if (t->state == TS_EST || t->state == TS_CLOSEWAIT || t->state == TS_FIN1 || t->state == TS_FIN2) tcp_out(t, t->snd_nxt, 0x14, NULL, 0);   /* abort politely */
    t->state = TS_FREE;
}
static void tcp_rx(u32 src, u32 dst, const u8 *p, int len) {
    if (len < 20) return;
    if (csum_fin(csum_part(p, len, pseudo(src, dst, 6, len))) != 0) return;
    u16 sport = (u16)((p[0] << 8) | p[1]), dport = (u16)((p[2] << 8) | p[3]);
    u32 seq = (u32)p[4] << 24 | (u32)p[5] << 16 | (u32)p[6] << 8 | p[7];
    u32 ack = (u32)p[8] << 24 | (u32)p[9] << 16 | (u32)p[10] << 8 | p[11];
    int hl = (p[12] >> 4) * 4, fl = p[13];
    if (hl < 20 || hl > len) return;
    const u8 *data = p + hl; int dlen = len - hl;
    net.tcp_rx++;
    Tcp *t = NULL;
    for (int i = 0; i < NTCP; i++) if (tcps[i].state != TS_FREE && tcps[i].rip == src && tcps[i].rport == sport && tcps[i].lport == dport) t = &tcps[i];
    if (!t) { if (!(fl & 4)) tcp_rst_reply(src, sport, dport, seq, ack, fl & 0x10, dlen + ((fl & 3) ? 1 : 0)); return; }
    if (t->state >= TS_DONE) return;
    if (t->state == TS_SYN) {
        if (fl & 4) { if ((fl & 0x10) && ack == t->snd_nxt) t->state = TS_REFUSED; return; }
        if ((fl & 0x12) == 0x12 && ack == t->snd_nxt) {
            t->snd_una = ack; t->rcv_nxt = seq + 1; t->state = TS_EST; t->tries = 0;
            tcp_out(t, t->snd_nxt, 0x10, NULL, 0);
        }
        return;
    }
    if (fl & 4) { t->state = TS_RESET; return; }
    if (fl & 0x10) {                                         /* ACK processing */
        if ((s32)(ack - t->snd_una) > 0 && (s32)(ack - t->snd_nxt) <= 0) {
            t->snd_una = ack;
            if (t->slen && ack == t->snd_nxt - (u32)(t->fin_sent ? 1 : 0)) { t->slen = 0; t->tries = 0; }
            if (t->fin_sent && ack == t->snd_nxt) {
                if (t->state == TS_LASTACK || t->state == TS_CLOSING) t->state = TS_DONE;
                else if (t->state == TS_FIN1) t->state = TS_FIN2;
            }
        }
    }
    int fin = fl & 1;
    if (dlen > 0 || fin) {
        if (seq != t->rcv_nxt) { tcp_out(t, t->snd_nxt, 0x10, NULL, 0); return; }          /* out of order: duplicate ACK */
        if (dlen > RBUF - t->rcount) { tcp_out(t, t->snd_nxt, 0x10, NULL, 0); return; }     /* no room: peer retransmits */
        for (int i = 0; i < dlen; i++) t->rbuf[(t->rhead + t->rcount + i) % RBUF] = data[i];
        t->rcount += dlen; t->rcv_nxt += (u32)dlen;
        if (fin) {
            t->rcv_nxt++; t->fin_rcvd = 1;
            if (t->state == TS_EST) t->state = TS_CLOSEWAIT;
            else if (t->state == TS_FIN1) t->state = TS_CLOSING;
            else if (t->state == TS_FIN2) t->state = TS_DONE;
        }
        tcp_out(t, t->snd_nxt, 0x10, NULL, 0);
    }
}
static void tcp_tick(void) {
    u32 now = ticks();
    for (int i = 0; i < NTCP; i++) {
        Tcp *t = &tcps[i];
        if (t->state == TS_FREE || t->state >= TS_DONE) continue;
        if (t->want_close && !t->fin_sent && !t->slen && (t->state == TS_EST || t->state == TS_CLOSEWAIT)) {
            tcp_out(t, t->snd_nxt, 0x11, NULL, 0); t->snd_nxt++; t->fin_sent = 1; t->t = now; t->tries = 1;
            t->state = t->state == TS_EST ? TS_FIN1 : TS_LASTACK;
            continue;
        }
        if (now - t->t < 1000) continue;
        if (t->state == TS_SYN) {
            if (++t->tries > 4) { t->state = TS_TIMEOUT; continue; }
            t->t = now; tcp_out(t, t->iss, 0x02, NULL, 0);
        } else if (t->slen) {
            if (++t->tries > 6) { t->state = TS_TIMEOUT; continue; }
            t->t = now; tcp_out(t, t->snd_una, 0x18, t->sbuf, t->slen);
        } else if (t->fin_sent && t->snd_una != t->snd_nxt) {
            if (++t->tries > 5) { t->state = TS_DONE; continue; }
            t->t = now; tcp_out(t, t->snd_nxt - 1, 0x11, NULL, 0);
        }
    }
}

/* ============================ receive path ============================ */
static void ip_rx(const u8 *p, int len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    const Ip4 *ip = (const Ip4 *)p; int ihl = (p[0] & 15) * 4, tl = bs16(ip->len);
    if (ihl < 20 || tl < ihl || tl > len) return;
    if (csum_fin(csum_part(p, ihl, 0)) != 0) return;
    if (bs16(ip->frag) & 0x3FFF) { net.rx_drop++; return; }                  /* fragments are not supported */
    u32 src = bs32(ip->src), dst = bs32(ip->dst);
    int mine = net.ip == 0 || dst == net.ip || dst == 0xFFFFFFFFu || (net.mask && dst == ((net.ip & net.mask) | ~net.mask));
    if (!mine) return;
    const u8 *pl = p + ihl; int pln = tl - ihl;
    switch (ip->proto) {
        case 1: icmp_rx(src, ip->ttl, pl, pln); break;
        case 17: udp_rx(src, dst, pl, pln); break;
        case 6: tcp_rx(src, dst, pl, pln); break;
    }
}
static void frame_rx(const u8 *f, int len) {
    if (len < 14) return;
    const Eth *e = (const Eth *)f; int type = bs16(e->type);
    if (type == 0x0806 && len >= 14 + (int)sizeof(Arp)) {
        const Arp *a = (const Arp *)(f + 14);
        if (bs16(a->op) == 1 && net.ip && bs32(a->tpa) == net.ip) {
            arp_learn(bs32(a->spa), a->sha);
            Arp *r = (Arp *)(txf + 14);
            r->htype = bs16(1); r->ptype = bs16(0x0800); r->hlen = 6; r->plen = 4; r->op = bs16(2);
            memcpy(r->sha, net.mac, 6); r->spa = bs32(net.ip); memcpy(r->tha, a->sha, 6); r->tpa = a->spa;
            eth_send(a->sha, 0x0806, sizeof(Arp));
        } else if (bs16(a->op) == 2) {
            u32 ip = bs32(a->spa); arp_learn(ip, a->sha);
            if (pend_len && pend_ip == ip) {
                memcpy(txf + 14, pend, (u32)pend_len); eth_send(a->sha, 0x0800, pend_len); pend_len = 0;
            }
        }
    } else if (type == 0x0800) ip_rx(f + 14, len - 14);
}

void net_poll(void) {
    if (!net.present) return;
    for (int guard = 0; guard < 16; guard++) {
        RxDesc *d = &rxd[rx_cur];
        if (!(d->status & 1)) break;
        int len = d->len;
        rx_time = rx_stamp_ok[rx_cur] ? rx_stamp[rx_cur] : ticks();
        if ((d->status & 2) && !(d->errors) && len > 0 && len <= BUFSZ) {
            net.rx_pkts++; net.rx_bytes += (u32)len;
            frame_rx(rxbuf[rx_cur], len);
        } else net.rx_drop++;
        rx_stamp_ok[rx_cur] = 0; d->status = 0;
        wr(0x2818, (u32)rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
    u32 now = ticks();
    static u32 last_link; if (now - last_link > 500) {
        last_link = now; int up = (rd(0x0008) >> 1) & 1;
        if (up && !net.link && net.dhcp == DHCP_NONE) net_dhcp_start();
        net.link = up;
    }
    if (pend_len && now - pend_t > 1000) {                 /* ARP retry */
        if (++pend_tries > 3) { pend_len = 0; net.arp_fail++; } else { pend_t = now; arp_request(pend_ip); }
    }
    dhcp_poll();
    tcp_tick();
}

void net_init(void) {
    memset(&net, 0, sizeof net);
    net.present = e1000_init();
    if (!net.present) return;
    net.link = (rd(0x0008) >> 1) & 1;
    if (net.link) net_dhcp_start();
}
