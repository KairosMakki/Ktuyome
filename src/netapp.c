#include "ktuyome.h"
/* Network app: a console for testing connectivity. Commands run as small state machines that are stepped from
 * na_tick() every pass of the main loop, so a ping keeps going (and keeps its output) when you leave the app. */

#include "vendor/bearssl/bearssl.h"                    /* wget over https:// (TLS 1.2 client) */

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
enum { J_NONE, J_PING, J_TRACE, J_DNS, J_TCP, J_HTTP, J_SCAN, J_UDP, J_WGET };
static int parse_port(const char *s);                        /* (defined with the command line) */
static u8 *w_buf; static u32 w_bufcap;                       /* wget download buffer (see below) */
static u32 w_buflen; static int w_hdr_done;
static int w_https, w_chunked, w_redirs; static u32 w_prog;
static br_ssl_client_context wssl; static int wssl_ready;
#define PING_ID 0x4B54
#define TRACE_ID 0x4B55
#define HBUF 16384
static char hbuf[HBUF]; static int hlen;
static struct {
    int kind, phase, dns_h; char host[72]; u32 ip, t0, t1, next;
    int count, sent, recvd, seq; u32 rmin, rmax, rsum; u32 sendt[64]; u8 st[64];
    int ttl, probe, done, hit; u32 hop_ip; char hopline[LW];
    int sock, port, p_end, open_n, off; char msg[80]; char path[96];
    u32 clen; char oname[28];
} job;

static void job_end(void) {
    if (job.dns_h >= 0) net_dns_free(job.dns_h);
    if (job.sock >= 0) { if (job.kind == J_UDP) udp_close(job.sock); else tcp_free(job.sock); }
    if (job.kind == J_WGET) { if (w_buf) { kfree(w_buf); w_buf = NULL; } w_bufcap = w_buflen = 0; wssl_ready = 0; }
    job.kind = J_NONE; job.dns_h = job.sock = -1; ui_wake();
}static int net_ready(void) {
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

/* ---------------------------------- wget ---------------------------------- */
/* "wget <url>" downloads over http:// or https:// (the vendored BearSSL TLS 1.2 client, the same
 * one the browser uses) into /downloads. Follows up to three redirects and decodes chunked
 * bodies; anything bigger than the KTFS storage pool (7.5 MB) is saved truncated, with a warning.
 * (The wget state variables, W_* macros and the TLS context live above the job struct.) */
static int wtls_recv_cb(void *ctx, u8 *buf, size_t len) { (void)ctx; int n = tcp_recv(job.sock, buf, (int)len); return n > 0 ? n : 0; }
static int wtls_send_cb(void *ctx, const u8 *buf, size_t len) { (void)ctx; int n = tcp_send(job.sock, buf, (int)len); return n > 0 ? n : 0; }

/* TLS-aware read/write on the job socket (plaintext once the handshake is done) */
static int w_recv(void *b, int n) {
    if (w_https && wssl_ready) return br_ssl_engine_recvapp_buf(&wssl.eng, b, (size_t)n);
    return tcp_recv(job.sock, b, n);
}
static int w_send(const void *b, int n) {
    if (w_https && wssl_ready) return br_ssl_engine_sendapp_buf(&wssl.eng, b, (size_t)n);
    return tcp_send(job.sock, b, n);
}

static void w_reset_body(void) {
    if (w_buf) { kfree(w_buf); w_buf = NULL; }
    w_bufcap = w_buflen = 0; w_hdr_done = 0; w_chunked = 0; w_prog = 0; wssl_ready = 0;
}
#define W_CLEN_UNK 0xFFFFFFFFu
#define W_SLICE 32768
#define W_MAX (FS_NBLK * FS_BLK)

/* grow the body buffer and append bytes; returns bytes actually stored (0 = nothing fits) */
static int w_append(const u8 *d, int len) {
    if (len <= 0) return 0;
    int room = (int)(W_MAX - w_buflen);
    if (room <= 0) return 0;
    if (len > room) len = room;
    if (!w_buf || (u32)(w_buflen + len) > w_bufcap) {
        u32 want = w_buflen + (u32)len;
        if (want < W_SLICE) want = W_SLICE;
        u8 *nb = krealloc(w_buf, want);
        if (!nb) return 0;
        w_buf = nb; w_bufcap = want;
    }
    memcpy(w_buf + w_buflen, d, (u32)len);
    w_buflen += (u32)len;
    return len;
}

/* header find+copy: hbuf holds `he` header bytes; returns value length or -1 */
static int w_hdr_val(int he, const char *needle, char *out, int cap) {
    int nl = (int)strlen(needle), p = 0;
    while (p < he) {
        int e = p; while (e < he && hbuf[e] != '\n') e++;
        if (e >= he) break;
        if (e - p > nl && ci_starts(hbuf + p, needle)) {
            int q = p + nl;
            while (q < e && (hbuf[q] == ':' || hbuf[q] == ' ' || hbuf[q] == '\t')) q++;
            int v = q; while (e > v && hbuf[e - 1] == '\r') e--;
            int n = e - v; if (n > cap - 1) n = cap - 1;
            memcpy(out, hbuf + v, (u32)n); out[n] = 0;
            return n;
        }
        p = e + 1;
    }
    return -1;
}
static int w_status_code(void) {                             /* 3-digit code from the first line */
    for (int i = 0; i < 16 && hbuf[i]; i++) if (hbuf[i] == ' ') return katoi(hbuf + i + 1);
    return -1;
}

/* unique save path in /downloads for a base name; m is the download duration in ms */
static void w_save_file(int truncated, u32 m) {
    char name[28]; int w = 0;
    const char *b = job.oname;
    while (*b == '/') b++;
    for (const char *q = b; *q && w < 27; q++) {
        char c = *q;
        if (c == '/' || c == '\\' || c == '?' || c == '%') c = '_';
        if (c < 32 || c > 126) continue;
        name[w++] = c;
    }
    name[w] = 0;
    if (!name[0]) kstrcpy(name, "download.bin", sizeof name);
    char p[80]; kstrcpy(p, "/downloads/", sizeof p); kstrcat(p, name, sizeof p);
    for (int i = 1; fs_lookup(p) >= 0 && i < 100; i++) {
        kstrcpy(p, "/downloads/", sizeof p); kstrcat(p, name, sizeof p);
        char q[8]; kitoa(q, i + 1); kstrcat(p, "-", sizeof p); kstrcat(p, q, sizeof p);
    }
    fs_create("/downloads", FS_DIR);
    int nd = fs_create(p, FS_FILE);
    if (nd < 0) { say(C_ERR, "Could not create the file in /downloads (no room or name clash)."); return; }
    if (fs_write(nd, w_buf, (int)w_buflen) < 0) { fs_delete(nd); fs_save(); files_invalidate(); say(C_ERR, "Storage full: the download was not kept."); return; }
    fs_save(); files_invalidate();
    lb_reset(); lb_s("Saved "); lb_s(p); lb_s(" ("); lb_i((int)w_buflen); lb_s(" bytes, "); lb_i((int)m); lb_s(" ms)"); lb_flush(truncated ? C_WARN : C_OK);
}

/* decode a chunked body in place (trailers after the last-chunk are dropped) */
static void w_unchunk(void) {
    u32 r = 0, wr = 0, guard = 0;
    while (r < w_buflen && guard++ < W_MAX / 256) {
        while (r < w_buflen && (w_buf[r] == '\r' || w_buf[r] == '\n')) r++;
        u32 size = 0; int any = 0;
        while (r < w_buflen && w_buf[r] != '\r' && w_buf[r] != '\n') {
            char c = (char)w_buf[r];
            int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) break;
            size = size * 16 + (u32)v; r++; any = 1;
            if (size > W_MAX) break;
        }
        if (!any) break;
        while (r < w_buflen && (w_buf[r] == '\r' || w_buf[r] == '\n')) r++;
        if (size == 0) break;
        if (r + size > w_buflen) break;                      /* stream ended inside a chunk */
        memmove(w_buf + wr, w_buf + r, size);
        wr += size; r += size;
    }
    w_buflen = wr;
}

/* parse "http://host[:port]/path" (host out 72, path out 96); 1 = ok */
static int w_parse_url(char *u, char *host, int *port, char *path, int *https) {
    *https = 0;
    if (starts_with(u, "https://")) { *https = 1; u += 8; }
    else if (starts_with(u, "http://")) u += 7;
    else return 0;
    char *fr = strchr(u, '#'); if (fr) *fr = 0;              /* fragments are not sent */
    int i = 0;
    while (u[i] && u[i] != '/' && i < 70) { host[i] = u[i]; i++; }
    host[i] = 0;
    if (!host[0]) return 0;
    *port = *https ? 443 : 80;
    for (int k = 0; host[k]; k++) if (host[k] == ':') { host[k] = 0; *port = parse_port(host + k + 1); break; }
    if (!*port) return 0;
    kstrcpy(path, u[i] ? u + i : "/", 96);
    return 1;
}
/* begin (or restart, after a redirect) a download; u is an absolute URL */
static int wget_start(const char *u) {
    char url[200]; kstrcpy(url, u, sizeof url);
    char host[72], path[96]; int port = 0, https;
    if (!w_parse_url(url, host, &port, path, &https)) { say(C_ERR, "usage: wget http://host[:port]/path"); return 0; }
    if (!job_start(J_WGET, host)) return 0;
    w_https = https; job.port = port; kstrcpy(job.path, path, sizeof job.path);
    const char *b = "/index.html";                           /* default save name from the URL tail */
    for (const char *p = path; *p; p++) if (*p == '/' && p[1] && p[1] != '?') b = p + 1;
    kstrcpy(job.oname, b, sizeof job.oname);
    return 1;
}

static void wget_step(void) {
    if (resolve_step() <= 0) return;
    u32 now = ticks();
    if (now - job.t0 > 60000) { say(C_ERR, "Download timed out after 60 s."); job_end(); return; }
    if (job.phase == 1) {
        w_reset_body();
        job.sock = tcp_open(job.ip, (u16)job.port);
        if (job.sock < 0) { say(C_ERR, "No free TCP socket."); job_end(); return; }
        job.phase = 2; job.t1 = now; hlen = 0;
        lb_reset(); lb_s("GET "); lb_s(w_https ? "https://" : "http://"); lb_s(job.host); lb_s(job.path); lb_s(" ..."); lb_flush(C_ACC);
        return;
    }
    if (job.phase == 2) {                                    /* TCP connect, then TLS handshake */
        int s = tcp_status(job.sock);
        if (s == TCP_CONNECTING) return;
        if (s != TCP_OPEN) {
            lb_reset(); lb_s(s == TCP_REFUSED ? "Connection refused" : s == TCP_TIMEOUT ? "Connection timed out" : "Connection failed"); lb_flush(C_ERR);
            job_end(); return;
        }
        if (!w_https) { job.phase = 3; job.off = 0; return; }
        if (!wssl_ready) {
            br_ssl_client_init_full(&wssl, NULL, 0);         /* no certificate verification (as in the browser) */
            br_ssl_client_reset(&wssl, job.host, 0);
            br_ssl_engine_set_io(&wssl.eng, wtls_recv_cb, wtls_send_cb, NULL);
        }
        br_ssl_engine_run(&wssl.eng);
        if (br_ssl_engine_current_state(&wssl.eng) & BR_SSL_CLOSED) {
            say(C_ERR, "TLS handshake failed (no shared cipher or bad key exch)."); job_end(); return;
        }
        if (wssl.eng.hs == TLS_HS_DONE) { wssl_ready = 1; job.phase = 3; job.off = 0; }
        return;
    }
    if (job.phase == 3) {                                    /* send the request */
        static char req[560]; req[0] = 0;
        kstrcat(req, "GET ", sizeof req); kstrcat(req, job.path, sizeof req); kstrcat(req, " HTTP/1.1\r\nHost: ", sizeof req);
        kstrcat(req, job.host, sizeof req);
        if (job.port != (w_https ? 443 : 80)) { kstrcat(req, ":", sizeof req); char t[8]; kitoa(t, job.port); kstrcat(req, t, sizeof req); }
        kstrcat(req, "\r\nUser-Agent: Ktuyome/" KT_VERSION " (wget)\r\nAccept: */*\r\nConnection: close\r\n\r\n", sizeof req);
        int rl = (int)strlen(req);
        if (job.off < rl) { int n = w_send(req + job.off, rl - job.off); if (n > 0) job.off += n; }
        if (job.off >= rl) { job.phase = 4; job.t1 = now; }
        return;
    }

    /* phase 4: receive */
    static u8 tmp[2048];
    int n;
    while ((n = w_recv(tmp, sizeof tmp)) > 0) {
        if (!w_hdr_done) {
            int take = imin(n, HBUF - 1 - hlen);             /* headers gather in hbuf until CRLFCRLF */
            if (take <= 0) { say(C_ERR, "Response header block is too large."); job_end(); return; }
            int leftover = n - take;
            memcpy(hbuf + hlen, tmp, (u32)take);
            hlen += take;
            int he = 0;
            for (int i = 0; i + 3 < hlen; i++)
                if (hbuf[i] == '\r' && hbuf[i+1] == '\n' && hbuf[i+2] == '\r' && hbuf[i+3] == '\n') { he = i + 4; break; }
            if (!he) continue;
            hbuf[hlen] = 0;
            int code = w_status_code();
            if (code >= 300 && code < 400) {                 /* redirect, up to three hops */
                char loc[200]; int ll = w_hdr_val(he, "Location", loc, (int)sizeof loc);
                if (ll > 0 && ++w_redirs <= 3) {
                    int port2 = 0, https2 = 0; char host2[72], path2[96]; char u2[240];
                    int absurl = starts_with(loc, "http://") || starts_with(loc, "https://");
                    if (absurl) {
                        if (!w_parse_url(loc, host2, &port2, path2, &https2)) { say(C_ERR, "Bad redirect URL."); job_end(); return; }
                        kstrcpy(u2, https2 ? "https://" : "http://", sizeof u2);
                        kstrcat(u2, host2, sizeof u2);
                        if (port2 != (https2 ? 443 : 80)) { char t[8]; kitoa(t, port2); kstrcat(u2, ":", sizeof u2); kstrcat(u2, t, sizeof u2); }
                        kstrcat(u2, path2, sizeof u2);
                    } else {
                        kstrcpy(path2, loc, sizeof path2);
                        kstrcpy(u2, w_https ? "https://" : "http://", sizeof u2);
                        kstrcat(u2, job.host, sizeof u2);
                        if (job.port != (w_https ? 443 : 80)) { char t[8]; kitoa(t, job.port); kstrcat(u2, ":", sizeof u2); kstrcat(u2, t, sizeof u2); }
                        kstrcat(u2, path2, sizeof u2);
                    }
                    lb_reset(); lb_s("redirect ("); lb_i(code); lb_s(") -> "); lb_s(u2); lb_flush(C_WARN);
                    job_end();                               /* frees the buffer and the socket */
                    wget_start(u2);
                    return;                                  /* the new job resolves/opens on its own */
                }
                say(C_WARN, "Redirect limit reached (3)."); job_end(); return;
            }
            if (code < 0 || code >= 400) {
                lb_reset(); lb_s("HTTP error "); lb_i(code < 0 ? 0 : code); lb_s(": ");
                for (int i = 0; i < 60 && hbuf[i] && hbuf[i] != '\n'; i++) { char c = hbuf[i]; if (c == '\r') c = ' '; char t[2] = { c, 0 }; kstrcat(lb, t, LW); }
                lb_flush(C_ERR); job_end(); return;
            }
            w_hdr_done = 1;
            char hv[40];
            if (w_hdr_val(he, "Transfer-Encoding", hv, (int)sizeof hv) >= 0 && ci_starts(hv, "chunked")) w_chunked = 1;
            if (w_hdr_val(he, "Content-Length", hv, (int)sizeof hv) >= 0) { int cl = katoi(hv); job.clen = cl <= 0 ? W_CLEN_UNK : (u32)cl; }
            else job.clen = W_CLEN_UNK;
            if (leftover > 0) w_append(tmp + take, leftover);/* body tail of the same read */
            continue;
        }
        /* plain body bytes */
        w_append(tmp, n);
        if (w_buflen >= W_MAX) break;                        /* full: stop reading */
        if (w_buflen - w_prog >= 256 * 1024) {               /* progress messages on big files */
            w_prog = w_buflen;
            lb_reset(); lb_s("Received "); lb_i((int)w_buflen); lb_s(" bytes..."); lb_flush(C_DIM);
        }
    }
    /* completion checks */
    int fin = 0;
    if (w_https && wssl_ready) {
        br_ssl_engine_run(&wssl.eng);
        if (br_ssl_engine_current_state(&wssl.eng) & BR_SSL_CLOSED) fin = 1;
        if (wssl.eng.hs == TLS_HS_DONE && job.clen != W_CLEN_UNK && w_buflen >= job.clen) fin = 1;
    } else {
        if (tcp_eof(job.sock) || (tcp_status(job.sock) != TCP_OPEN && tcp_status(job.sock) != TCP_CONNECTING)) fin = 1;
        if (job.clen != W_CLEN_UNK && w_buflen >= job.clen) fin = 1;
    }
    if (w_buflen >= W_MAX) fin = 1;
    if (!fin) return;
    u32 ms = ticks() - job.t0;
    if (w_chunked) w_unchunk();
    int trunc = w_buflen >= W_MAX;                           /* hit the 7.5 MB storage cap */
    w_save_file(trunc, ms);
    job_end();
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
        case J_WGET: wget_step(); break;
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
        "wget <url>                download a file over http:// or https:// into /downloads",
        "udp <host> <port> <text>  send a UDP datagram and wait for a reply",
        "ip                        adapter, address, gateway, DNS     arp  neighbour cache",
        "stats                     packet counters                    clear  empty the screen",
        "dhcp                      request a new address",
        "setip <ip> <mask> [gw] [dns]    configure by hand            setdns <ip>",
        "Ctrl+C stops the running command.  Up/Down recall history.  PgUp/PgDn scroll.",
    };
    for (u32 i = 0; i < sizeof h / sizeof h[0]; i++) say(i == 12 ? C_DIM : C_TEXT, h[i]);
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
    else if (!strcmp(c, "wget")) {
        if (n < 2) { say(C_ERR, "usage: wget <url>   e.g. wget http://10.0.2.2/file.bin or wget https://example.com/index.html"); return; }
        if (!net.present) { net_ready(); return; }
        w_redirs = 0; w_reset_body();
        wget_start(a[1]);
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
