#include "ktuyome.h"
/* Network app: a console for testing connectivity. Commands run as small state machines that are stepped from
 * na_tick() every pass of the main loop, so a ping keeps going (and keeps its output) when you leave the app. */

#define NL 400                                           /* scrollback lines */
#define LW 128
static char lines[NL][LW]; static u8 lcol[NL]; static int lcount;     /* lcount counts every line ever written */
static char lb[LW];
static int scroll_off;                                    /* lines scrolled up from the bottom */
enum { C_TEXT, C_OK, C_ERR, C_DIM, C_ACC, C_WARN, C_CMD };

static void lb_reset(void) { lb[0] = 0; }
static void lb_s(const char *s) { kstrcat(lb, s, LW); }
static void lb_i(int v) { char t[16]; kitoa(t, v); kstrcat(lb, t, LW); }
static void lb_ip(u32 ip) { char t[20]; net_ip_str(t, ip); kstrcat(lb, t, LW); }
static void lb_mac(const u8 *m) {
    static const char h[] = "0123456789abcdef"; char t[4]; t[2] = 0;
    for (int i = 0; i < 6; i++) { t[0] = h[m[i] >> 4]; t[1] = h[m[i] & 15]; kstrcat(lb, t, LW); if (i < 5) kstrcat(lb, ":", LW); }
}
static void lb_flush(int col) {
    int i = lcount % NL; kstrcpy(lines[i], lb, LW); lcol[i] = (u8)col; lcount++; lb[0] = 0; ui_wake();
}
static void say(int col, const char *s) { lb_reset(); lb_s(s); lb_flush(col); }
static void lb_ms(u32 ms) { if (ms == 0) lb_s("<1 ms"); else { lb_i((int)ms); lb_s(" ms"); } }

/* ---------------------------------- job state ---------------------------------- */
enum { J_NONE, J_PING, J_TRACE, J_DNS, J_TCP, J_HTTP, J_SCAN, J_UDP };
#define PING_ID 0x4B54
#define TRACE_ID 0x4B55
#define HBUF 16384
static char hbuf[HBUF]; static int hlen;
static struct {
    int kind, phase, dns_h; char host[72]; u32 ip, t0, t1, next;
    int count, sent, recvd, seq; u32 rmin, rmax, rsum; u32 sendt[64]; u8 st[64];
    int ttl, probe, done, hit; u32 hop_ip; char hopline[LW];
    int sock, port, p_end, open_n, off; char msg[80]; char path[96];
} job;

static void job_end(void) {
    if (job.dns_h >= 0) net_dns_free(job.dns_h);
    if (job.sock >= 0) { if (job.kind == J_UDP) udp_close(job.sock); else tcp_free(job.sock); }
    job.kind = J_NONE; job.dns_h = job.sock = -1; ui_wake();
}
static int net_ready(void) {
    if (!net.present) { say(C_ERR, "No supported network card found."); say(C_DIM, "Ktuyome drives the Intel e1000, the default NIC in QEMU and VirtualBox (Intel PRO/1000)."); return 0; }
    if (!net.link) { say(C_ERR, "Network cable is unplugged (link down)."); return 0; }
    if (!net.ip) { say(C_ERR, net.dhcp == DHCP_FAILED ? "No DHCP server answered. Type 'dhcp' to retry or 'setip' to configure by hand." : "Still waiting for an address from DHCP..."); return 0; }
    return 1;
}
static const char *dns_err(int r) { return r == -1 ? "DNS request timed out" : r == -2 ? "Host not found" : "No DNS server configured"; }

/* Resolve job.host. Returns 1 when job.ip is ready, 0 while pending, -1 on failure (message printed, job ended). */
static int resolve_step(void) {
    if (job.phase > 0) return 1;
    int r = net_dns_poll(job.dns_h, &job.ip);
    if (r == 0) return 0;
    if (r == 1) { job.phase = 1; job.t1 = ticks(); return 1; }
    lb_reset(); lb_s(job.host); lb_s(": "); lb_s(dns_err(r)); lb_flush(C_ERR);
    job_end(); return -1;
}
static int job_start(int kind, const char *host) {
    if (job.kind != J_NONE) { say(C_WARN, "A command is still running. Press Ctrl+C to stop it."); return 0; }
    if (!net_ready()) return 0;
    memset(&job, 0, sizeof job); job.kind = kind; job.dns_h = job.sock = -1;
    kstrcpy(job.host, host, sizeof job.host); job.t0 = ticks();
    if (net_parse_ip(host, &job.ip)) job.phase = 1;
    else if (starts_with(host, "localhost")) { job.ip = net_ip(127, 0, 0, 1); job.phase = 1; }
    else {
        job.dns_h = net_dns_start(host);
        if (job.dns_h < 0) { say(C_ERR, "Resolver busy, try again."); job.kind = J_NONE; return 0; }
    }
    return 1;
}

/* ---------------------------------- ping ---------------------------------- */
static void ping_summary(void) {
    lb_reset(); lb_s("--- "); lb_s(job.host); lb_s(" ping statistics ---"); lb_flush(C_DIM);
    lb_reset(); lb_i(job.sent); lb_s(" sent, "); lb_i(job.recvd); lb_s(" received, "); lb_i(job.sent ? (job.sent - job.recvd) * 100 / job.sent : 0); lb_s("% loss");
    lb_flush(job.recvd ? C_OK : C_ERR);
    if (job.recvd) {
        lb_reset(); lb_s("rtt min/avg/max = "); lb_i((int)job.rmin); lb_s("/"); lb_i((int)(job.rsum / (u32)job.recvd)); lb_s("/"); lb_i((int)job.rmax); lb_s(" ms"); lb_flush(C_TEXT);
    }
}
static void ping_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (job.phase == 1) {
        job.phase = 2; job.next = now;
        lb_reset(); lb_s("PING "); lb_s(job.host); lb_s(" ("); lb_ip(job.ip); lb_s(") 32 bytes of data"); lb_flush(C_ACC);
    }
    if (job.sent < job.count && (s32)(now - job.next) >= 0) {
        net_icmp_echo(job.ip, 64, PING_ID, job.sent, 32);
        job.st[job.sent] = 1; job.sendt[job.sent] = now; job.sent++; job.next = now + 1000;
    }
    NetIcmpEv ev;
    while (net_icmp_pop(&ev)) {
        if (ev.id != PING_ID || ev.seq >= job.sent) continue;
        if (job.st[ev.seq] != 1) continue;
        if (ev.type == 0) {
            u32 rtt = ev.t - job.sendt[ev.seq]; if ((s32)rtt < 0) rtt = 0;
            job.st[ev.seq] = 2; job.recvd++;
            if (job.recvd == 1 || rtt < job.rmin) job.rmin = rtt;
            if (rtt > job.rmax) job.rmax = rtt;
            job.rsum += rtt;
            lb_reset(); lb_s("Reply from "); lb_ip(ev.src); lb_s(": seq="); lb_i(ev.seq + 1); lb_s(" ttl="); lb_i(ev.ttl); lb_s(" time="); lb_ms(rtt);
            lb_flush(C_OK);
        } else {
            job.st[ev.seq] = 3;
            lb_reset(); lb_s("From "); lb_ip(ev.src); lb_s(": seq="); lb_i(ev.seq + 1);
            lb_s(ev.type == 11 ? " TTL expired in transit" : " destination unreachable"); lb_flush(C_ERR);
        }
    }
    int open = 0;
    for (int i = 0; i < job.sent; i++) {
        if (job.st[i] == 1 && now - job.sendt[i] > 2000) { job.st[i] = 3; lb_reset(); lb_s("seq="); lb_i(i + 1); lb_s(": request timed out"); lb_flush(C_ERR); }
        if (job.st[i] == 1) open++;
    }
    if (job.sent >= job.count && !open) { ping_summary(); job_end(); }
}

/* ---------------------------------- traceroute ---------------------------------- */
static void trace_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (job.phase == 1) {
        job.phase = 2; job.ttl = 1; job.probe = 0; job.next = now; job.st[0] = 0; job.hop_ip = 0; lb_reset();
        lb_s("traceroute to "); lb_s(job.host); lb_s(" ("); lb_ip(job.ip); lb_s("), 16 hops max"); lb_flush(C_ACC);
        job.hopline[0] = 0;
    }
    if (job.st[0] == 0) {                                          /* send a probe */
        job.seq = job.ttl * 4 + job.probe; job.sendt[0] = now; job.st[0] = 1;
        if (job.probe == 0) { char t[16]; kitoa(t, job.ttl); kstrcpy(job.hopline, job.ttl < 10 ? " " : "", LW); kstrcat(job.hopline, t, LW); kstrcat(job.hopline, "  ", LW); }
        net_icmp_echo(job.ip, job.ttl, TRACE_ID, job.seq, 32);
        return;
    }
    NetIcmpEv ev; int got = 0;
    while (net_icmp_pop(&ev)) {
        if (ev.id != TRACE_ID || ev.seq != job.seq) continue;
        u32 rtt = ev.t - job.sendt[0]; if ((s32)rtt < 0) rtt = 0;
        char t[24];
        if (ev.src != job.hop_ip) { net_ip_str(t, ev.src); kstrcat(job.hopline, " ", LW); kstrcat(job.hopline, t, LW); job.hop_ip = ev.src; }
        kstrcat(job.hopline, " ", LW); kitoa(t, (int)rtt); kstrcat(job.hopline, t, LW); kstrcat(job.hopline, "ms", LW);
        if (ev.type == 0 || ev.type == 3) job.done = 1;
        got = 1;
    }
    if (!got && now - job.sendt[0] > 1500) { kstrcat(job.hopline, "  *", LW); got = 1; }
    if (!got) return;
    job.st[0] = 0;
    if (++job.probe < 3) return;
    lb_reset(); lb_s(job.hopline); lb_flush(job.done ? C_OK : C_TEXT);
    job.probe = 0; job.hop_ip = 0;
    if (job.done) { say(C_DIM, "Trace complete."); job_end(); }
    else if (++job.ttl > 16) { say(C_WARN, "Gave up after 16 hops (NAT routers often hide hops)."); job_end(); }
}

/* ---------------------------------- nslookup ---------------------------------- */
static void dns_step(void) {
    if (resolve_step() <= 0) return;
    lb_reset(); lb_s("Server:  "); lb_ip(net.dns); lb_flush(C_DIM);
    lb_reset(); lb_s("Name:    "); lb_s(job.host); lb_flush(C_TEXT);
    lb_reset(); lb_s("Address: "); lb_ip(job.ip); lb_s("   ("); lb_ms(ticks() - job.t0); lb_s(")"); lb_flush(C_OK);
    job_end();
}

/* ---------------------------------- tcp connect test ---------------------------------- */
static void tcp_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (job.phase == 1) {
        job.sock = tcp_open(job.ip, (u16)job.port);
        if (job.sock < 0) { say(C_ERR, "No free TCP socket."); job_end(); return; }
        job.phase = 2; job.t1 = now;
        lb_reset(); lb_s("Connecting to "); lb_ip(job.ip); lb_s(":"); lb_i(job.port); lb_s(" ..."); lb_flush(C_ACC);
        return;
    }
    int s = tcp_status(job.sock);
    if (s == TCP_CONNECTING) return;
    lb_reset();
    if (s == TCP_OPEN) { lb_s("Port "); lb_i(job.port); lb_s(" is open ("); lb_ms(now - job.t1); lb_s(" to connect)"); lb_flush(C_OK); }
    else if (s == TCP_REFUSED) { lb_s("Connection refused: nothing is listening on port "); lb_i(job.port); lb_flush(C_ERR); }
    else if (s == TCP_TIMEOUT) { lb_s("Timed out: no answer from "); lb_ip(job.ip); lb_s(" (filtered or host down)"); lb_flush(C_ERR); }
    else { lb_s("Connection failed (reset)"); lb_flush(C_ERR); }
    if (s == TCP_OPEN) tcp_close(job.sock);
    if (s == TCP_OPEN) { job.phase = 3; job.t1 = now; return; }
    job_end();
}
static void tcp_closing_step(void) { if (tcp_status(job.sock) != TCP_OPEN || ticks() - job.t1 > 1500) job_end(); }

/* ---------------------------------- http ---------------------------------- */
static int ci_starts(const char *s, const char *p) {
    for (; *p; s++, p++) { char a = *s, b = *p; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) return 0; }
    return 1;
}
static void http_show(void) {
    hbuf[hlen < HBUF ? hlen : HBUF - 1] = 0;
    int body = -1;
    for (int i = 0; i + 3 < hlen; i++) if (hbuf[i] == '\r' && hbuf[i + 1] == '\n' && hbuf[i + 2] == '\r' && hbuf[i + 3] == '\n') { body = i + 4; break; }
    if (hlen == 0) { say(C_ERR, "The server closed the connection without sending anything."); return; }
    int hend = body < 0 ? hlen : body - 4, o = 0, first = 1;
    while (o < hend) {
        int e = o; while (e < hend && hbuf[e] != '\r' && hbuf[e] != '\n') e++;
        int l = imin(e - o, 110);
        if (first) { lb_reset(); for (int i = 0; i < l; i++) { char c[2] = { hbuf[o + i], 0 }; kstrcat(lb, c, LW); } lb_flush(l > 9 && hbuf[o + 9] == '2' ? C_OK : C_WARN); first = 0; }
        else if (ci_starts(hbuf + o, "content-type:") || ci_starts(hbuf + o, "content-length:") || ci_starts(hbuf + o, "server:") || ci_starts(hbuf + o, "location:")) {
            lb_reset(); for (int i = 0; i < l; i++) { char c[2] = { hbuf[o + i], 0 }; kstrcat(lb, c, LW); } lb_flush(C_DIM);
        }
        while (e < hend && (hbuf[e] == '\r' || hbuf[e] == '\n')) e++;
        o = e;
    }
    if (body >= 0 && body < hlen) {
        say(C_DIM, "-----");
        int shown = 0; o = body;
        while (o < hlen && shown < 80) {
            int e = o; while (e < hlen && hbuf[e] != '\n') e++;
            lb_reset(); int n = 0;
            for (int i = o; i < e && n < 96; i++, n++) { char c = hbuf[i]; if (c == '\r') { n--; continue; } if (c == '\t') c = ' '; if (c < 32 || c > 126) c = '.'; char t[2] = { c, 0 }; kstrcat(lb, t, LW); }
            lb_flush(C_TEXT); shown++; o = e + 1;
        }
        if (o < hlen) say(C_DIM, "... (output truncated)");
    }
}
static void http_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (job.phase == 1) {
        job.sock = tcp_open(job.ip, (u16)job.port); hlen = 0; job.off = 0;
        if (job.sock < 0) { say(C_ERR, "No free TCP socket."); job_end(); return; }
        job.phase = 2; job.t1 = now;
        lb_reset(); lb_s("GET http://"); lb_s(job.host); lb_s(job.path); lb_s("  ("); lb_ip(job.ip); lb_s(":"); lb_i(job.port); lb_s(")"); lb_flush(C_ACC);
        return;
    }
    int s = tcp_status(job.sock);
    if (job.phase == 2) {
        if (s == TCP_CONNECTING) return;
        if (s != TCP_OPEN) {
            lb_reset(); lb_s(s == TCP_REFUSED ? "Connection refused" : s == TCP_TIMEOUT ? "Connection timed out" : "Connection failed"); lb_flush(C_ERR); job_end(); return;
        }
        static char req[320]; req[0] = 0;
        kstrcat(req, "GET ", sizeof req); kstrcat(req, job.path, sizeof req); kstrcat(req, " HTTP/1.0\r\nHost: ", sizeof req);
        kstrcat(req, job.host, sizeof req); kstrcat(req, "\r\nUser-Agent: Ktuyome/" KT_VERSION "\r\nAccept: */*\r\nConnection: close\r\n\r\n", sizeof req);
        int rl = (int)strlen(req);
        if (job.off < rl) { int n = tcp_send(job.sock, req + job.off, rl - job.off); job.off += n; }
        if (job.off >= rl) { job.phase = 3; job.next = now; }
        return;
    }
    int n;
    while ((n = tcp_recv(job.sock, hbuf + hlen, HBUF - hlen)) > 0) hlen += n;
    int fin = tcp_eof(job.sock) || hlen >= HBUF || (s != TCP_OPEN && s != TCP_CONNECTING) || now - job.t1 > 15000;
    if (!fin) return;
    if (now - job.t1 > 15000 && !hlen) say(C_ERR, "No response within 15 s.");
    else { http_show(); lb_reset(); lb_s("Received "); lb_i(hlen); lb_s(" bytes in "); lb_ms(now - job.t1); lb_flush(C_DIM); }
    tcp_close(job.sock); job_end();
}

/* ---------------------------------- port scan ---------------------------------- */
static const u16 common_ports[] = { 21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 993, 995, 3306, 3389, 5432, 8000, 8080, 8443 };
static void scan_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (job.phase == 1) {
        job.phase = 2; job.sock = -1; job.probe = 0; job.open_n = 0;
        lb_reset(); lb_s("Scanning "); lb_ip(job.ip); lb_s(job.p_end ? "" : " (common ports)"); lb_flush(C_ACC);
    }
    if (job.sock < 0) {
        int total = job.p_end ? job.p_end - job.port + 1 : (int)(sizeof common_ports / sizeof common_ports[0]);
        if (job.probe >= total) {
            lb_reset(); lb_i(job.open_n); lb_s(" open port"); if (job.open_n != 1) lb_s("s"); lb_s(" of "); lb_i(total); lb_s(" scanned"); lb_flush(job.open_n ? C_OK : C_WARN);
            job_end(); return;
        }
        job.hit = job.p_end ? job.port + job.probe : common_ports[job.probe];
        job.sock = tcp_open(job.ip, (u16)job.hit); job.t1 = now;
        if (job.sock < 0) job.sock = -1;
        return;
    }
    int s = tcp_status(job.sock);
    if (s == TCP_CONNECTING && now - job.t1 < 1200) return;
    if (s == TCP_OPEN) { lb_reset(); lb_s("  "); lb_i(job.hit); lb_s("/tcp  open"); lb_flush(C_OK); job.open_n++; }
    tcp_free(job.sock); job.sock = -1; job.probe++;
}

/* ---------------------------------- udp ---------------------------------- */
static void udp_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (job.phase == 1) {
        job.sock = udp_open(0); job.phase = 2; job.t1 = now;
        udp_send(job.sock, job.ip, (u16)job.port, job.msg, (int)strlen(job.msg));
        lb_reset(); lb_s("Sent "); lb_i((int)strlen(job.msg)); lb_s(" bytes to "); lb_ip(job.ip); lb_s(":"); lb_i(job.port); lb_flush(C_ACC);
        return;
    }
    char buf[200]; u32 from; u16 fp;
    int n = udp_recv(job.sock, buf, sizeof buf - 1, &from, &fp);
    if (n >= 0) {
        buf[n] = 0; for (int i = 0; i < n; i++) if (buf[i] < 32 || buf[i] > 126) buf[i] = '.';
        lb_reset(); lb_s("Reply from "); lb_ip(from); lb_s(":"); lb_i(fp); lb_s(" ("); lb_i(n); lb_s(" bytes, "); lb_ms(now - job.t1); lb_s("): "); lb_s(buf); lb_flush(C_OK);
        job_end();
    } else if (now - job.t1 > 2500) { say(C_WARN, "No reply (UDP is connectionless: the port may be closed or the service silent)."); job_end(); }
}

void na_tick(void) {
    net_poll();
    switch (job.kind) {
        case J_PING: ping_step(); break;
        case J_TRACE: trace_step(); break;
        case J_DNS: dns_step(); break;
        case J_TCP: if (job.phase == 3) tcp_closing_step(); else tcp_step(); break;
        case J_HTTP: http_step(); break;
        case J_SCAN: scan_step(); break;
        case J_UDP: udp_step(); break;
    }
}

/* ---------------------------------- command line ---------------------------------- */
static char inbuf[120]; static int inlen;
static char hist[16][120]; static int hist_n, hist_pos;
static const char *dhcp_name(void) {
    switch (net.dhcp) { case DHCP_DISCOVER: return "discovering"; case DHCP_REQUEST: return "requesting"; case DHCP_BOUND: return "leased"; case DHCP_FAILED: return "failed"; case DHCP_STATIC: return "static"; default: return "off"; }
}
static void show_ip(void) {
    if (!net.present) { net_ready(); return; }
    lb_reset(); lb_s("Adapter   "); lb_s(net.nic); lb_flush(C_TEXT);
    lb_reset(); lb_s("Link      "); lb_s(net.link ? "up" : "down (cable unplugged)"); lb_flush(net.link ? C_OK : C_ERR);
    lb_reset(); lb_s("MAC       "); lb_mac(net.mac); lb_flush(C_TEXT);
    lb_reset(); lb_s("Address   "); if (net.ip) lb_ip(net.ip); else lb_s("(none)"); lb_s("   config: "); lb_s(dhcp_name()); lb_flush(C_TEXT);
    lb_reset(); lb_s("Netmask   "); lb_ip(net.mask); lb_flush(C_TEXT);
    lb_reset(); lb_s("Gateway   "); lb_ip(net.gw); lb_flush(C_TEXT);
    lb_reset(); lb_s("DNS       "); lb_ip(net.dns); lb_flush(C_TEXT);
    if (net.lease) { lb_reset(); lb_s("Lease     "); lb_i((int)net.lease); lb_s(" s from "); lb_ip(net.dhcp_server); lb_flush(C_TEXT); }
}
static void show_arp(void) {
    int n = 0;
    for (int i = 0; i < 8; i++) if (net_arp[i].valid) { lb_reset(); lb_ip(net_arp[i].ip); lb_s("   "); lb_mac(net_arp[i].mac); lb_flush(C_TEXT); n++; }
    if (!n) say(C_DIM, "ARP cache is empty.");
}
static void show_stats(void) {
    lb_reset(); lb_s("Frames   rx "); lb_i((int)net.rx_pkts); lb_s("   tx "); lb_i((int)net.tx_pkts); lb_s("   dropped rx "); lb_i((int)net.rx_drop); lb_s(" tx "); lb_i((int)net.tx_drop); lb_flush(C_TEXT);
    lb_reset(); lb_s("Bytes    rx "); lb_i((int)net.rx_bytes); lb_s("   tx "); lb_i((int)net.tx_bytes); lb_flush(C_TEXT);
    lb_reset(); lb_s("ICMP     rx "); lb_i((int)net.icmp_rx); lb_s("   tx "); lb_i((int)net.icmp_tx); lb_flush(C_TEXT);
    lb_reset(); lb_s("UDP      rx "); lb_i((int)net.udp_rx); lb_s("   tx "); lb_i((int)net.udp_tx); lb_flush(C_TEXT);
    lb_reset(); lb_s("TCP      rx "); lb_i((int)net.tcp_rx); lb_s("   tx "); lb_i((int)net.tcp_tx); lb_flush(C_TEXT);
}
static void help(void) {
    static const char *h[] = {
        "ping [-c N] <host>        ICMP echo with round-trip times (default 4, max 64)",
        "traceroute <host>         show each router on the way (needs ICMP time-exceeded)",
        "nslookup <host>           resolve a name with the DHCP-supplied DNS server",
        "tcp <host> <port>         test whether a TCP port accepts connections",
        "scan <host> [from-to]     scan common ports, or a range such as 20-100 (max 200)",
        "http <host>[/path]        fetch a page over HTTP and show headers and the start",
        "udp <host> <port> <text>  send a UDP datagram and wait for a reply",
        "ip                        adapter, address, gateway, DNS     arp  neighbour cache",
        "stats                     packet counters                    clear  empty the screen",
        "dhcp                      request a new address",
        "setip <ip> <mask> [gw] [dns]    configure by hand            setdns <ip>",
        "Ctrl+C stops the running command.  Up/Down recall history.  PgUp/PgDn scroll.",
    };
    for (u32 i = 0; i < sizeof h / sizeof h[0]; i++) say(i == 11 ? C_DIM : C_TEXT, h[i]);
}
static int tokenize(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        argv[n++] = s; while (*s && *s != ' ') s++;
    }
    return n;
}
static int parse_port(const char *s) { int v = katoi(s); return (v >= 1 && v <= 65535) ? v : 0; }

static void run_command(char *line) {
    char *a[8]; int n = tokenize(line, a, 8);
    if (!n) return;
    const char *c = a[0];
    if (!strcmp(c, "help") || !strcmp(c, "?")) help();
    else if (!strcmp(c, "clear") || !strcmp(c, "cls")) { lcount = 0; scroll_off = 0; ui_wake(); }
    else if (!strcmp(c, "ip") || !strcmp(c, "ifconfig") || !strcmp(c, "ipconfig")) show_ip();
    else if (!strcmp(c, "arp")) show_arp();
    else if (!strcmp(c, "stats") || !strcmp(c, "netstat")) show_stats();
    else if (!strcmp(c, "dhcp")) {
        if (!net.present) { net_ready(); return; }
        net_dhcp_start(); say(C_ACC, "DHCP discover sent...");
    }
    else if (!strcmp(c, "setip")) {
        u32 ip, mask, gw = 0, dns = 0;
        if (n < 3 || !net_parse_ip(a[1], &ip) || !net_parse_ip(a[2], &mask) || (n > 3 && !net_parse_ip(a[3], &gw)) || (n > 4 && !net_parse_ip(a[4], &dns))) {
            say(C_ERR, "usage: setip <ip> <netmask> [gateway] [dns]   e.g. setip 10.0.2.15 255.255.255.0 10.0.2.2 10.0.2.3"); return; }
        if (!net.present) { net_ready(); return; }
        net_set_static(ip, mask, gw, dns); show_ip();
    }
    else if (!strcmp(c, "setdns")) {
        u32 ip; if (n < 2 || !net_parse_ip(a[1], &ip)) { say(C_ERR, "usage: setdns <ip>"); return; }
        net.dns = ip; lb_reset(); lb_s("DNS server is now "); lb_ip(ip); lb_flush(C_OK);
    }
    else if (!strcmp(c, "ping")) {
        int count = 4, hi = 1;
        if (n >= 3 && !strcmp(a[1], "-c")) { count = iclamp(katoi(a[2]), 1, 64); hi = 3; }
        if (n <= hi) { say(C_ERR, "usage: ping [-c N] <host>"); return; }
        if (job_start(J_PING, a[hi])) job.count = count;
    }
    else if (!strcmp(c, "traceroute") || !strcmp(c, "tracert")) {
        if (n < 2) { say(C_ERR, "usage: traceroute <host>"); return; }
        job_start(J_TRACE, a[1]);
    }
    else if (!strcmp(c, "nslookup") || !strcmp(c, "dns") || !strcmp(c, "host")) {
        if (n < 2) { say(C_ERR, "usage: nslookup <host>"); return; }
        u32 t; if (net_parse_ip(a[1], &t)) { say(C_WARN, "That is already an IP address."); return; }
        job_start(J_DNS, a[1]);
    }
    else if (!strcmp(c, "tcp") || !strcmp(c, "connect")) {
        int p = n >= 3 ? parse_port(a[2]) : 0;
        if (!p) { say(C_ERR, "usage: tcp <host> <port>"); return; }
        if (job_start(J_TCP, a[1])) job.port = p;
    }
    else if (!strcmp(c, "scan")) {
        if (n < 2) { say(C_ERR, "usage: scan <host> [from-to]"); return; }
        int p1 = 0, p2 = 0;
        if (n >= 3) {
            const char *r = a[2]; p1 = parse_port(r); while (*r && *r != '-') r++;
            p2 = *r == '-' ? parse_port(r + 1) : p1;
            if (!p1 || !p2 || p2 < p1 || p2 - p1 >= 200) { say(C_ERR, "Range must be like 20-100 and span at most 200 ports."); return; }
        }
        if (job_start(J_SCAN, a[1])) { job.port = p1; job.p_end = p2; }
    }
    else if (!strcmp(c, "udp")) {
        int p = n >= 3 ? parse_port(a[2]) : 0;
        if (!p) { say(C_ERR, "usage: udp <host> <port> <text>"); return; }
        if (job_start(J_UDP, a[1])) {
            job.port = p; job.msg[0] = 0;
            for (int i = 3; i < n; i++) { if (i > 3) kstrcat(job.msg, " ", sizeof job.msg); kstrcat(job.msg, a[i], sizeof job.msg); }
            if (!job.msg[0]) kstrcpy(job.msg, "hello from Ktuyome", sizeof job.msg);
        }
    }
    else if (!strcmp(c, "http") || !strcmp(c, "get") || !strcmp(c, "curl")) {
        if (n < 2) { say(C_ERR, "usage: http <host>[:port][/path]"); return; }
        char *u = a[1]; if (starts_with(u, "http://")) u += 7;
        if (starts_with(u, "https://")) { say(C_ERR, "HTTPS is not supported (no TLS). Use http:// or port 80."); return; }
        char host[72]; int i = 0; while (u[i] && u[i] != '/' && i < 70) { host[i] = u[i]; i++; } host[i] = 0;
        const char *path = u[i] ? u + i : "/";
        int port = 80;
        for (int k = 0; host[k]; k++) if (host[k] == ':') { host[k] = 0; port = parse_port(host + k + 1); break; }
        if (!port || !host[0]) { say(C_ERR, "usage: http <host>[:port][/path]"); return; }
        if (job_start(J_HTTP, host)) { job.port = port; kstrcpy(job.path, path, sizeof job.path); }
    }
    else { lb_reset(); lb_s("Unknown command: "); lb_s(c); lb_s("   (type help)"); lb_flush(C_ERR); }
}
static void submit(void) {
    lb_reset(); lb_s("> "); lb_s(inbuf); lb_flush(C_CMD);
    if (inlen && (hist_n == 0 || strcmp(hist[(hist_n - 1) % 16], inbuf))) kstrcpy(hist[hist_n++ % 16], inbuf, sizeof hist[0]);
    hist_pos = hist_n; scroll_off = 0;
    char tmp[120]; kstrcpy(tmp, inbuf, sizeof tmp); inbuf[0] = 0; inlen = 0;
    run_command(tmp);
}

void na_open(void) {
    static int first = 1;
    if (first) {
        first = 0; job.dns_h = job.sock = -1;
        lb_reset(); lb_s("Ktuyome network tools   "); lb_s(KT_VERSION); lb_flush(C_ACC);
        say(C_DIM, "Type help for commands. Try: ping 10.0.2.2   nslookup example.com   http example.com");
    }
    ui_wake();
}
void na_close(void) {}

static void na_copy_log(void) {                                            /* the whole scrollback, oldest line first */
    clip_begin();
    for (int i = imax(0, lcount - NL); i < lcount; i++) { clip_add(lines[i % NL], (int)strlen(lines[i % NL])); clip_add("\n", 1); }
    clip_notify("Copied output:", clip_len());
}
int na_key(const Event *e) {
    int ck = clip_key(e);
    if (ck == CK_PASTE) {                                                  /* first line only, so a paste never runs several commands */
        char t[128]; int more = 0, n = clip_line(t, sizeof t, &more);
        if (!n) { ui_flash("Clipboard is empty"); return 1; }
        int take = imin(n, (int)sizeof inbuf - 1 - inlen);
        memcpy(inbuf + inlen, t, take); inlen += take; inbuf[inlen] = 0;
        ui_flash(more ? "Pasted the first line only" : "Pasted"); ui_wake(); return 1;
    }
    if (ck == CK_CUT && inlen) { clip_set(inbuf, inlen); clip_notify("Cut", clip_len()); inbuf[0] = 0; inlen = 0; ui_wake(); return 1; }
    if (ck == CK_COPY && (e->key == K_INS || (e->mods & MOD_SHIFT))) {     /* Ctrl+C alone still interrupts; Ctrl+Shift+C / Ctrl+Insert copy */
        if (inlen) { clip_set(inbuf, inlen); clip_notify("Copied", clip_len()); } else na_copy_log();
        ui_wake(); return 1;
    }
    if (e->mods & MOD_CTRL) {
        if (e->key == 'c') { if (job.kind != J_NONE) { say(C_WARN, "^C"); job_end(); } else { inbuf[0] = 0; inlen = 0; ui_wake(); } return 1; }
        if (e->key == 'l') { lcount = 0; scroll_off = 0; ui_wake(); return 1; }
        return 0;
    }
    switch (e->key) {
        case K_ENTER: submit(); return 1;
        case K_BS: if (inlen) inbuf[--inlen] = 0; ui_wake(); return 1;
        case K_UP: if (hist_pos > 0 && hist_pos > hist_n - 16) { hist_pos--; kstrcpy(inbuf, hist[hist_pos % 16], sizeof inbuf); inlen = (int)strlen(inbuf); } ui_wake(); return 1;
        case K_DOWN: if (hist_pos < hist_n - 1) { hist_pos++; kstrcpy(inbuf, hist[hist_pos % 16], sizeof inbuf); inlen = (int)strlen(inbuf); } else { hist_pos = hist_n; inbuf[0] = 0; inlen = 0; } ui_wake(); return 1;
        case K_PGUP: scroll_off += 8; ui_wake(); return 1;
        case K_PGDN: scroll_off = imax(0, scroll_off - 8); ui_wake(); return 1;
        case K_END: scroll_off = 0; ui_wake(); return 1;
        case K_ESC: if (inlen) { inbuf[0] = 0; inlen = 0; ui_wake(); return 1; } return 0;
    }
    if (e->ch >= 32 && e->ch < 127 && inlen < (int)sizeof inbuf - 1) { inbuf[inlen++] = (char)e->ch; inbuf[inlen] = 0; ui_wake(); return 1; }
    return 0;
}

/* ---------------------------------- drawing ---------------------------------- */
static void pill_info(int x, int y, const char *label, const char *val, u32 vc) {
    gfx_text(F_SM, x, y, label, WHITE_A(120));
    gfx_text(F_MD, x, y + 18, val, vc);
}
void na_draw(int x, int y, int w, int h) {
    /* status strip */
    int sh = 76;
    gfx_rrect(x, y, w, sh, 16, WHITE_A(18), WHITE_A(10), 0);
    gfx_rrect(x, y, w, sh, 16, WHITE_A(32), WHITE_A(32), 1);
    char t[48];
    u32 ok = net.present && net.link && net.ip;
    gfx_circle(x + 26, y + 26, 7, ok ? 0xFF4DD0A1 : (net.present && net.link ? 0xFFFFC857 : 0xFFFF6E6E));
    gfx_text(F_MDB, x + 42, y + 14, !net.present ? "No adapter" : !net.link ? "Disconnected" : net.ip ? "Connected" : "Waiting for address", 0xFFFFFFFF);
    gfx_text(F_SM, x + 20, y + 44, net.present ? net.nic : "Ktuyome needs an Intel e1000 NIC", WHITE_A(130));
    int cx = x + 300, cw = 170;
    net_ip_str(t, net.ip); pill_info(cx, y + 12, "ADDRESS", net.ip ? t : "-", 0xFFFFFFFF);
    net_ip_str(t, net.gw); pill_info(cx + cw, y + 12, "GATEWAY", net.gw ? t : "-", 0xFFFFFFFF);
    net_ip_str(t, net.dns); pill_info(cx + cw * 2, y + 12, "DNS", net.dns ? t : "-", 0xFFFFFFFF);
    t[0] = 0; kstrcat(t, "rx ", sizeof t); { char n[16]; kitoa(n, (int)net.rx_pkts); kstrcat(t, n, sizeof t); kstrcat(t, "  tx ", sizeof t); kitoa(n, (int)net.tx_pkts); kstrcat(t, n, sizeof t); }
    pill_info(cx + cw * 3, y + 12, "PACKETS", t, WHITE_A(220));
    /* console */
    int cy = y + sh + 10, ch = h - sh - 10;
    gfx_rrect(x, cy, w, ch, 16, BLACK_A(110), BLACK_A(110), 0);
    gfx_rrect(x, cy, w, ch, 16, WHITE_A(30), WHITE_A(30), 1);
    int lh = fonts[F_MONO].h + 3, inh = 44, rows = (ch - inh - 12) / lh;
    int total = imin(lcount, NL);
    if (g_wheel) scroll_off = imax(0, scroll_off + g_wheel * 3);
    scroll_off = imin(scroll_off, imax(0, total - rows));
    gfx_clip(x + 4, cy + 4, w - 8, ch - inh - 6);
    int last = lcount - 1 - scroll_off;
    for (int r = 0; r < rows; r++) {
        int li = last - (rows - 1 - r); if (li < 0 || li < lcount - NL) continue;
        int idx = li % NL; u32 col;
        switch (lcol[idx]) { case C_OK: col = 0xFF8BE9B0; break; case C_ERR: col = 0xFFFF8A80; break; case C_DIM: col = WHITE_A(130); break;
            case C_ACC: col = col_lerp(ACC, 0xFFFFFFFF, 90); break; case C_WARN: col = 0xFFFFD27A; break; case C_CMD: col = 0xFFFFFFFF; break; default: col = WHITE_A(215); }
        gfx_text(F_MONO, x + 16, cy + 10 + r * lh, lines[idx], col);
    }
    gfx_noclip();
    if (scroll_off > 0) gfx_text_r(F_SM, x + w - 18, cy + 8, "scrolled - End to follow", WHITE_A(120));
    /* input line */
    int iy = cy + ch - inh;
    gfx_fill(x + 14, iy - 2, w - 28, 1, WHITE_A(26));
    gfx_text(F_MONO, x + 16, iy + 11, job.kind != J_NONE ? "*" : ">", job.kind != J_NONE ? 0xFFFFD27A : ACC);
    gfx_text(F_MONO, x + 38, iy + 11, inbuf, 0xFFFFFFFF);
    if (!g_blink && job.kind == J_NONE) gfx_fill(x + 38 + gfx_text_w(F_MONO, inbuf, -1), iy + 10, 2, fonts[F_MONO].h, ACC);
    if (job.kind != J_NONE) gfx_text_r(F_SM, x + w - 18, iy + 13, "running - Ctrl+C to stop", 0xFFFFD27A);
    else {
        int bx = x + w - 18;
        static const char *bl[3] = { "Ping gateway", "Test DNS", "Renew DHCP" };
        for (int i = 2; i >= 0; i--) {
            int bw = gfx_text_w(F_MDB, bl[i], -1) + 36; bx -= bw;
            if (ui_btn(bx, iy + 5, bw, 32, bl[i], 0)) {
                char c[40]; c[0] = 0;
                if (i == 0 && !net.gw) { say(C_ERR, "No gateway configured."); continue; }
                if (i == 0) { kstrcat(c, "ping ", sizeof c); char ip[20]; net_ip_str(ip, net.gw); kstrcat(c, ip, sizeof c); }
                else if (i == 1) kstrcat(c, "nslookup example.com", sizeof c);
                else kstrcat(c, "dhcp", sizeof c);
                kstrcpy(inbuf, c, sizeof inbuf); inlen = (int)strlen(inbuf); submit();
            }
            bx -= 8;
        }
    }
}

u32 netapp_static_bytes(void) { return (u32)(sizeof lines + sizeof lcol + sizeof hbuf + sizeof hist + sizeof inbuf + sizeof job); }
