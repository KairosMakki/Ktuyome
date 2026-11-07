/*
 * browser.c — Text-mode web browser app for ktuyome OS
 *
 * Features:
 *   - URL bar (Prompt widget) supporting http:// and https://
 *   - DNS resolve → TCP connect → optional BearSSL TLS 1.2 handshake
 *   - HTTP/1.1 GET with chunked-transfer decoding and redirect following
 *   - HTML→text renderer: strips tags, renders visible text + links
 *   - Scrollable text viewport with keyboard (PgUp/Dn, arrows) and mouse wheel
 *   - Inline link list: Tab cycles through links, Enter navigates
 *   - Status bar: state, URL, timing, byte count
 *   - JS console tab: runs inline <script> blocks through quickjs_kt
 *   - F5 = reload, Alt+Left = back (1-level history)
 *   - Ctrl+U = view source, Ctrl+S = save page to /downloads/
 *
 * Network model: polled from browser_tick() (called every main-loop pass).
 * All state lives in static globals — no dynamic allocation for the app
 * skeleton itself; only vendor libs (BearSSL ctx, QuickJS runtime) use
 * the kmalloc heap.
 */
#include "ktuyome.h"
#include "vendor/bearssl/bearssl.h"
#include "vendor/quickjs/quickjs_kt.h"

/* ================================================================== */
/*  Sizes & limits                                                     */
/* ================================================================== */
#define BR_URL_MAX       512
#define BR_HOST_MAX      256
#define BR_PATH_MAX      512
#define BR_BUF_SIZE      (256*1024)   /* 256 KB page buffer             */
#define BR_MAX_LINES     4096         /* rendered text lines            */
#define BR_LINE_W        140          /* characters per line            */
#define BR_MAX_LINKS     256          /* in-page anchor links           */
#define BR_MAX_REDIRECTS 5
#define BR_TIMEOUT_MS    20000        /* 20 s overall timeout           */
#define BR_JS_BUF        (32*1024)    /* JS source accumulator          */
#define BR_HIST_DEPTH    16           /* back-history entries           */
#define BR_DL_PATH       "/downloads/"

/* ================================================================== */
/*  Fetch state machine                                                */
/* ================================================================== */
typedef enum {
    F_IDLE = 0,
    F_DNS,
    F_CONNECT,
    F_TLS_HANDSHAKE,
    F_SEND_REQ,
    F_RECV_HDRS,
    F_RECV_BODY,
    F_DONE,
    F_ERROR
} FetchState;

/* ================================================================== */
/*  Page rendering                                                     */
/* ================================================================== */
typedef struct {
    char text[BR_LINE_W];   /* visible text of one rendered line */
    int  is_heading;        /* h1/h2/h3 → bold rendering */
    int  link_idx;          /* -1 = no link; ≥0 = index into g_links */
    u32  color;             /* override color (0 = default)           */
} BrLine;

typedef struct {
    char url[BR_URL_MAX];
    char label[80];
} BrLink;

/* ================================================================== */
/*  Static state                                                       */
/* ================================================================== */

/* --- Network / fetch ----------------------------------------------- */
static FetchState  g_state      = F_IDLE;
static char        g_url[BR_URL_MAX];        /* current URL            */
static char        g_host[BR_HOST_MAX];
static char        g_path[BR_PATH_MAX];
static int         g_port;
static int         g_https;
static int         g_tcp;                    /* TCP socket handle      */
static int         g_dns_h;                  /* DNS query handle       */
static u32         g_peer_ip;
static u32         g_t0;                     /* fetch start ticks      */
static int         g_redirects;
static int         g_req_off;                /* bytes of request sent  */
static int         g_chunked;                /* chunked transfer enc.  */
static int         g_content_len;            /* -1 = unknown           */
static char        g_status_line[128];
static char        g_content_type[64];

/* raw receive accumulator */
static char        g_raw[BR_BUF_SIZE];
static int         g_raw_len;
static int         g_hdr_end;                /* offset of body start   */

/* --- BearSSL ------------------------------------------------------- */
static br_ssl_client_context  g_ssl_ctx;
static int                    g_tls_ready;   /* handshake complete     */

/* TCP I/O callbacks for BearSSL */
static int tls_recv_cb(void *ctx, uint8_t *buf, size_t len) {
    (void)ctx;
    int n = tcp_recv(g_tcp, buf, (int)len);
    return n > 0 ? n : 0;
}
static int tls_send_cb(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    int n = tcp_send(g_tcp, buf, (int)len);
    return n > 0 ? n : 0;
}

/* --- QuickJS ------------------------------------------------------- */
static JSRuntime  *g_js_rt;
static JSContext  *g_js_ctx;
static char        g_js_buf[BR_JS_BUF];     /* accumulated script src */
static int         g_js_len;
static char        g_js_log[BR_MAX_LINES][BR_LINE_W];
static int         g_js_log_n;

/* --- Rendered page -------------------------------------------------- */
static BrLine      g_lines[BR_MAX_LINES];
static int         g_nlines;
static BrLink      g_links[BR_MAX_LINKS];
static int         g_nlinks;
static int         g_scroll;                 /* top visible line       */
static int         g_cur_link;              /* focused link index     */

/* --- UI state ------------------------------------------------------- */
static Prompt      g_prompt;                /* URL bar                */
static int         g_show_source;           /* Ctrl+U: view raw HTML  */
static int         g_tab;                   /* 0=page 1=js_console    */
static char        g_err_msg[128];
static char        g_history[BR_HIST_DEPTH][BR_URL_MAX];
static int         g_hist_n;
/* timing */
static u32         g_fetch_ms;
static int         g_fetch_bytes;

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */
static void br_set_error(const char *msg) {
    kstrcpy(g_err_msg, msg, sizeof g_err_msg);
    g_state = F_ERROR;
    ui_wake();
}

/* URL parser: fills g_host, g_path, g_port, g_https from full URL */
static int parse_url(const char *url) {
    const char *p = url;
    g_https = 0; g_port = 80;
    if (starts_with(p, "https://")) { g_https = 1; g_port = 443; p += 8; }
    else if (starts_with(p, "http://")) { p += 7; }
    else return 0;  /* unknown scheme */

    /* host[:port] */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < (int)sizeof(g_host)-1)
        g_host[hi++] = *p++;
    g_host[hi] = 0;
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') port = port*10 + (*p++ - '0');
        if (port) g_port = port;
    }
    kstrcpy(g_path, *p ? p : "/", sizeof g_path);
    return hi > 0;
}

/* Resolve relative URL against current base */
static void resolve_url(const char *base, const char *rel, char *out, int cap) {
    if (starts_with(rel, "http://") || starts_with(rel, "https://")) {
        kstrcpy(out, rel, cap); return;
    }
    if (rel[0] == '/') {
        /* absolute path */
        char scheme[16], host_part[BR_HOST_MAX];
        const char *p = base;
        int si = 0;
        while (*p && *p != ':' && si < 15) scheme[si++] = *p++;
        scheme[si] = 0;
        if (*p == ':') p += 3;  /* skip :// */
        int hi = 0;
        while (*p && *p != '/' && hi < (int)sizeof(host_part)-1) host_part[hi++] = *p++;
        host_part[hi] = 0;
        kt_snprintf(out, (size_t)cap, "%s://%s%s", scheme, host_part, rel);
        return;
    }
    /* relative path: replace last path component */
    kstrcpy(out, base, cap);
    char *slash = out;
    for (char *q = out; *q; q++) if (*q == '/') slash = q;
    if (slash > out) { slash[1] = 0; kstrcat(out, rel, cap); }
    else             { kstrcpy(out, rel, cap); }
}

/* ================================================================== */
/*  Chunked transfer decoding                                          */
/* ================================================================== */
/* In-place: decode buf[0..len) as chunked body, write to out.
 * Returns number of bytes written, or -1 on fatal error. */
static int decode_chunked(const char *buf, int len, char *out, int cap) {
    int pos = 0, out_pos = 0;
    while (pos < len) {
        /* read hex chunk-size line */
        int sz = 0;
        while (pos < len && buf[pos] != '\r' && buf[pos] != '\n') {
            char c = buf[pos++];
            int d = (c>='0'&&c<='9') ? c-'0' :
                    (c>='a'&&c<='f') ? c-'a'+10 :
                    (c>='A'&&c<='F') ? c-'A'+10 : -1;
            if (d < 0) break;
            sz = sz*16 + d;
        }
        while (pos < len && (buf[pos]=='\r'||buf[pos]=='\n')) pos++;
        if (sz == 0) break;  /* final chunk */
        int take = imin(sz, imin(len - pos, cap - out_pos));
        if (take <= 0) break;
        memcpy(out + out_pos, buf + pos, (u32)take);
        out_pos += take; pos += take;
        while (pos < len && (buf[pos]=='\r'||buf[pos]=='\n')) pos++;
    }
    return out_pos;
}

/* ================================================================== */
/*  HTML → text renderer                                               */
/* ================================================================== */
typedef struct {
    const char *html;
    int         len;
    int         pos;

    /* current line accumulator */
    char   line[BR_LINE_W];
    int    line_len;
    int    heading;        /* 1–3 */
    int    in_link;
    char   link_href[BR_URL_MAX];
    char   link_text[BR_LINE_W];
    int    link_text_len;
    int    in_script;
    int    in_style;
    int    in_pre;
    int    skip_tag;       /* unknown block tags to skip contents */

    /* output */
    BrLine *lines;
    int    *nlines;
    BrLink *links;
    int    *nlinks;

    /* for JS scripts */
    char   *js_buf;
    int    *js_len;
    int     js_cap;
} HtmlParser;

static void html_flush_line(HtmlParser *h) {
    if (!h->line_len) return;
    if (*h->nlines >= BR_MAX_LINES) return;
    h->line[h->line_len] = 0;
    BrLine *bl = &h->lines[(*h->nlines)++];
    kstrcpy(bl->text, h->line, BR_LINE_W);
    bl->is_heading = h->heading;
    bl->link_idx   = -1;
    bl->color      = 0;
    h->line_len = 0; h->line[0] = 0;
}

static void html_emit_char(HtmlParser *h, char c) {
    if (c == '\n') { html_flush_line(h); return; }
    if (c == '\t') c = ' ';
    if ((unsigned char)c < 32) return;
    /* wrap at line width */
    if (h->line_len >= BR_LINE_W - 1) html_flush_line(h);
    /* collapse multiple spaces */
    if (c == ' ' && h->line_len > 0 && h->line[h->line_len-1] == ' ') return;
    h->line[h->line_len++] = c;
    h->line[h->line_len]   = 0;
}

static void html_emit_str(HtmlParser *h, const char *s) {
    for (; *s; s++) html_emit_char(h, *s);
}

/* Read up to `cap-1` chars of tag name from h->pos into `out` */
static void html_read_tagname(HtmlParser *h, char *out, int cap) {
    int n = 0;
    while (h->pos < h->len && n < cap-1) {
        char c = h->html[h->pos];
        if (c == '>' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') break;
        out[n++] = (char)tolower((unsigned char)c);
        h->pos++;
    }
    out[n] = 0;
}

/* Read attribute value (handles quoted and unquoted) into `out` */
static void html_read_attr_val(HtmlParser *h, char *out, int cap) {
    /* skip = and whitespace */
    while (h->pos < h->len && (h->html[h->pos]==' '||h->html[h->pos]=='\t'||h->html[h->pos]=='=')) h->pos++;
    int quoted = 0; char q = 0;
    if (h->pos < h->len && (h->html[h->pos]=='"'||h->html[h->pos]=='\'')) {
        q = h->html[h->pos++]; quoted = 1;
    }
    int n = 0;
    while (h->pos < h->len && n < cap-1) {
        char c = h->html[h->pos];
        if (quoted  && c == q) { h->pos++; break; }
        if (!quoted && (c=='>'||c==' '||c=='\t'||c=='\r'||c=='\n')) break;
        out[n++] = c; h->pos++;
    }
    out[n] = 0;
}

/* Decode common HTML entities in-place */
static void html_decode_entities(char *s) {
    static const struct { const char *e; char c; } ents[] = {
        {"&amp;", '&'},{"&lt;", '<'},{"&gt;", '>'},
        {"&quot;",'"'},{"&apos;",'\''},{"&nbsp;",' '},
        {"&#160;",' '},{NULL,0}
    };
    char *w = s;
    for (const char *r = s; *r; ) {
        if (*r == '&') {
            int matched = 0;
            for (int i = 0; ents[i].e; i++) {
                if (starts_with(r, ents[i].e)) {
                    *w++ = ents[i].c; r += strlen(ents[i].e); matched = 1; break;
                }
            }
            if (!matched) *w++ = *r++;
        } else { *w++ = *r++; }
    }
    *w = 0;
}

static void html_parse(HtmlParser *h) {
    while (h->pos < h->len) {
        char c = h->html[h->pos];

        if (c == '<') {
            h->pos++;
            if (h->pos >= h->len) break;

            int closing = 0;
            if (h->html[h->pos] == '/') { closing = 1; h->pos++; }
            /* skip comments <!-- ... --> */
            if (h->pos+2 < h->len && h->html[h->pos]=='!' &&
                h->html[h->pos+1]=='-' && h->html[h->pos+2]=='-') {
                while (h->pos+2 < h->len &&
                       !(h->html[h->pos]=='-'&&h->html[h->pos+1]=='-'&&h->html[h->pos+2]=='>'))
                    h->pos++;
                h->pos += 3; continue;
            }
            /* skip DOCTYPE */
            if (h->pos < h->len && h->html[h->pos]=='!') {
                while (h->pos < h->len && h->html[h->pos]!='>') h->pos++;
                h->pos++; continue;
            }

            char tag[32]; html_read_tagname(h, tag, sizeof tag);

            /* script */
            if (!strcmp(tag,"script")) {
                if (!closing) { h->in_script = 1; }
                else {
                    h->in_script = 0;
                    /* buffer is already filled; run it */
                }
                while (h->pos < h->len && h->html[h->pos] != '>') h->pos++;
                h->pos++; continue;
            }
            /* style */
            if (!strcmp(tag,"style")) {
                h->in_style = !closing;
                while (h->pos < h->len && h->html[h->pos] != '>') h->pos++;
                h->pos++; continue;
            }

            if (h->in_script || h->in_style) {
                /* accumulate script text (handled below in text section) */
                while (h->pos < h->len && h->html[h->pos] != '>') h->pos++;
                h->pos++; continue;
            }

            /* block elements → flush line */
            if (!strcmp(tag,"p")||!strcmp(tag,"div")||!strcmp(tag,"section")||
                !strcmp(tag,"article")||!strcmp(tag,"main")||!strcmp(tag,"nav")||
                !strcmp(tag,"header")||!strcmp(tag,"footer")||!strcmp(tag,"aside")||
                !strcmp(tag,"ul")||!strcmp(tag,"ol")||!strcmp(tag,"table")||
                !strcmp(tag,"tr")||!strcmp(tag,"td")||!strcmp(tag,"th")||
                !strcmp(tag,"blockquote")||!strcmp(tag,"figcaption")) {
                html_flush_line(h);
            }
            /* headings */
            if (tag[0]=='h' && tag[1]>='1' && tag[1]<='3' && tag[2]==0) {
                html_flush_line(h);
                h->heading = closing ? 0 : tag[1]-'0';
                if (!closing) html_emit_str(h, closing?"":"");
            }
            /* br / hr */
            if (!strcmp(tag,"br")||!strcmp(tag,"hr")) {
                html_flush_line(h);
                if (!strcmp(tag,"hr")) {
                    for (int i=0;i<60;i++) html_emit_char(h,'-');
                    html_flush_line(h);
                }
            }
            /* li */
            if (!strcmp(tag,"li")) {
                html_flush_line(h);
                if (!closing) { html_emit_str(h,"  • "); }
            }
            /* anchor */
            if (!strcmp(tag,"a")) {
                if (!closing) {
                    h->in_link = 1;
                    h->link_href[0] = 0; h->link_text[0] = 0; h->link_text_len = 0;
                    /* parse href */
                    while (h->pos < h->len && h->html[h->pos] != '>') {
                        while (h->pos < h->len && (h->html[h->pos]==' '||h->html[h->pos]=='\t')) h->pos++;
                        if (h->pos >= h->len || h->html[h->pos]=='>') break;
                        char attr[32]; int ai=0;
                        while (h->pos<h->len&&h->html[h->pos]!='='&&h->html[h->pos]!='>'&&
                               h->html[h->pos]!=' ') { if(ai<31) attr[ai++]=tolower((unsigned char)h->html[h->pos]); h->pos++; }
                        attr[ai]=0;
                        if (!strcmp(attr,"href")) {
                            if (h->pos<h->len && h->html[h->pos]=='=')
                                html_read_attr_val(h, h->link_href, sizeof h->link_href);
                        } else {
                            /* skip this attr value */
                            if (h->pos<h->len && h->html[h->pos]=='=') html_read_attr_val(h,attr,sizeof attr);
                        }
                    }
                } else {
                    /* closing </a>: register link and annotate lines */
                    if (h->link_href[0] && *h->nlinks < BR_MAX_LINKS) {
                        char full_href[BR_URL_MAX];
                        resolve_url(g_url, h->link_href, full_href, sizeof full_href);
                        int li = (*h->nlinks)++;
                        kstrcpy(h->links[li].url, full_href, BR_URL_MAX);
                        kstrcpy(h->links[li].label, h->link_text, 80);
                        html_flush_line(h);
                        /* mark the last line with this link index */
                        if (*h->nlines > 0)
                            h->lines[*h->nlines-1].link_idx = li;
                    }
                    h->in_link = 0;
                }
            }
            /* img — emit alt text */
            if (!strcmp(tag,"img")) {
                char alt[80]; alt[0]=0;
                while (h->pos < h->len && h->html[h->pos] != '>') {
                    while (h->pos<h->len&&(h->html[h->pos]==' '||h->html[h->pos]=='\t')) h->pos++;
                    if (h->pos>=h->len||h->html[h->pos]=='>') break;
                    char attr[32]; int ai=0;
                    while (h->pos<h->len&&h->html[h->pos]!='='&&h->html[h->pos]!='>'&&h->html[h->pos]!=' ')
                        { if(ai<31) attr[ai++]=tolower((unsigned char)h->html[h->pos]); h->pos++; }
                    attr[ai]=0;
                    if (!strcmp(attr,"alt")) html_read_attr_val(h,alt,sizeof alt);
                    else if (h->pos<h->len&&h->html[h->pos]=='=') html_read_attr_val(h,attr,sizeof attr);
                }
                if (alt[0]) { html_emit_char(h,'['); html_emit_str(h,alt); html_emit_char(h,']'); }
            }
            /* title — emit as heading */
            if (!strcmp(tag,"title") && !closing) {
                /* text will flow through normal path */
                h->heading = 1;
            }
            if (!strcmp(tag,"title") && closing) {
                html_flush_line(h); h->heading = 0;
            }

            /* skip to end of current tag */
            while (h->pos < h->len && h->html[h->pos] != '>') h->pos++;
            if (h->pos < h->len) h->pos++;
            continue;
        }

        /* text content */
        if (h->in_style) { h->pos++; continue; }

        if (h->in_script) {
            /* accumulate into JS buffer */
            if (h->js_buf && *h->js_len < h->js_cap - 1) {
                h->js_buf[(*h->js_len)++] = c;
                h->js_buf[*h->js_len] = 0;
            }
            h->pos++; continue;
        }

        /* handle &entities; inline */
        if (c == '&') {
            char ent[16]; int ei = 0;
            int saved = h->pos;
            h->pos++;
            while (h->pos < h->len && h->html[h->pos] != ';' && ei < 10)
                ent[ei++] = h->html[h->pos++];
            ent[ei] = 0;
            if (h->pos < h->len && h->html[h->pos] == ';') { h->pos++; }
            else { h->pos = saved; html_emit_char(h,'&'); continue; }
            char decoded = '?';
            if (!strcmp(ent,"amp"))  decoded='&';
            else if (!strcmp(ent,"lt"))   decoded='<';
            else if (!strcmp(ent,"gt"))   decoded='>';
            else if (!strcmp(ent,"quot")) decoded='"';
            else if (!strcmp(ent,"apos")) decoded='\'';
            else if (!strcmp(ent,"nbsp")) decoded=' ';
            else if (ent[0]=='#') decoded=(char)katoi(ent+1);
            html_emit_char(h, decoded);
            continue;
        }

        if (h->in_link && h->link_text_len < (int)sizeof(h->link_text)-1)
            h->link_text[h->link_text_len++] = c;

        html_emit_char(h, c);
        h->pos++;
    }
    html_flush_line(h);
}

/* ================================================================== */
/*  Source-view renderer (no HTML parsing)                            */
/* ================================================================== */
static void render_source(const char *html, int len) {
    g_nlines = 0; g_nlinks = 0; g_scroll = 0;
    int line_pos = 0;
    char line_buf[BR_LINE_W];
    for (int i = 0; i < len && g_nlines < BR_MAX_LINES; i++) {
        char c = html[i];
        if (c == '\n' || line_pos >= BR_LINE_W-1) {
            line_buf[line_pos] = 0;
            kstrcpy(g_lines[g_nlines].text, line_buf, BR_LINE_W);
            g_lines[g_nlines].is_heading = 0;
            g_lines[g_nlines].link_idx   = -1;
            g_lines[g_nlines].color      = 0;
            g_nlines++;
            line_pos = 0;
            if (c == '\n') continue;
        }
        if (c != '\r') line_buf[line_pos++] = c;
    }
    if (line_pos > 0 && g_nlines < BR_MAX_LINES) {
        line_buf[line_pos] = 0;
        kstrcpy(g_lines[g_nlines++].text, line_buf, BR_LINE_W);
    }
}

/* ================================================================== */
/*  Page render entry                                                  */
/* ================================================================== */
static void render_page(void) {
    g_nlines = 0; g_nlinks = 0; g_scroll = 0; g_cur_link = -1;
    g_js_len = 0; g_js_buf[0] = 0;
    g_js_log_n = 0;

    if (g_show_source) {
        const char *body = g_raw;
        int   blen = g_raw_len;
        if (g_hdr_end > 0) { body += g_hdr_end; blen -= g_hdr_end; }
        render_source(body, blen);
        return;
    }

    const char *body = g_raw;
    int   blen = g_raw_len;
    if (g_hdr_end > 0) { body += g_hdr_end; blen -= g_hdr_end; }

    HtmlParser h;
    memset(&h, 0, sizeof h);
    h.html    = body;
    h.len     = blen;
    h.lines   = g_lines;
    h.nlines  = &g_nlines;
    h.links   = g_links;
    h.nlinks  = &g_nlinks;
    h.js_buf  = g_js_buf;
    h.js_len  = &g_js_len;
    h.js_cap  = BR_JS_BUF;
    html_parse(&h);

    /* Run collected JS */
    if (g_js_len > 0 && g_js_ctx) {
        JSValue result = JS_Eval(g_js_ctx, g_js_buf, (size_t)g_js_len,
                                 "<page>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue ex = JS_GetException(g_js_ctx);
            const char *msg = JS_ToCString(g_js_ctx, ex);
            if (g_js_log_n < BR_MAX_LINES)
                kstrcpy(g_js_log[g_js_log_n++], msg, BR_LINE_W);
            JS_FreeCString(g_js_ctx, msg);
            JS_FreeValue(g_js_ctx, ex);
        }
        JS_FreeValue(g_js_ctx, result);
    }

    if (g_nlines == 0) {
        kstrcpy(g_lines[g_nlines].text, "(empty page)", BR_LINE_W);
        g_lines[g_nlines].is_heading=0; g_lines[g_nlines].link_idx=-1; g_nlines++;
    }

    /* Mark first link as focused */
    g_cur_link = -1;
    for (int i = 0; i < g_nlines; i++)
        if (g_lines[i].link_idx >= 0) { g_cur_link = g_lines[i].link_idx; break; }
}

/* ================================================================== */
/*  QuickJS callbacks                                                  */
/* ================================================================== */
static void js_console_cb(const char *line, int is_error) {
    if (g_js_log_n < BR_MAX_LINES)
        kstrcpy(g_js_log[g_js_log_n++], line, BR_LINE_W);
    (void)is_error;
}

static void js_docwrite_cb(const char *html) {
    /* append to raw buffer as if received */
    int add = (int)strlen(html);
    if (g_raw_len + add < BR_BUF_SIZE) {
        memcpy(g_raw + g_raw_len, html, (u32)add);
        g_raw_len += add;
    }
}

/* ================================================================== */
/*  Fetch helpers                                                      */
/* ================================================================== */
static void fetch_reset(void) {
    if (g_dns_h >= 0) { net_dns_free(g_dns_h); g_dns_h = -1; }
    if (g_tcp  >= 0) { tcp_free(g_tcp);         g_tcp  = -1; }
    g_raw_len = 0; g_hdr_end = 0; g_req_off = 0;
    g_chunked = 0; g_content_len = -1;
    g_status_line[0] = 0; g_content_type[0] = 0;
    g_tls_ready = 0;
    g_err_msg[0] = 0;
}

static void fetch_start(const char *url) {
    fetch_reset();
    kstrcpy(g_url, url, sizeof g_url);
    if (!parse_url(url)) { br_set_error("Invalid URL (use http:// or https://)"); return; }
    g_t0       = ticks();
    g_fetch_ms = 0;
    g_fetch_bytes = 0;
    g_state    = F_DNS;

    /* Push to history */
    if (g_hist_n < BR_HIST_DEPTH - 1) {
        kstrcpy(g_history[g_hist_n++], url, BR_URL_MAX);
    }

    /* DNS: if it's already an IP, skip */
    if (net_parse_ip(g_host, &g_peer_ip)) { g_state = F_CONNECT; }
    else {
        g_dns_h = net_dns_start(g_host);
        if (g_dns_h < 0) { br_set_error("DNS resolver busy"); }
    }
    ui_wake();
}

/* Parse HTTP response headers once the header block is complete */
static void parse_response_headers(void) {
    /* Find header end */
    for (int i = 0; i + 3 < g_raw_len; i++) {
        if (g_raw[i]=='\r'&&g_raw[i+1]=='\n'&&g_raw[i+2]=='\r'&&g_raw[i+3]=='\n') {
            g_hdr_end = i + 4; break;
        }
    }
    if (!g_hdr_end) return;
    g_raw[g_hdr_end-1] = 0;  /* NUL-terminate headers for string ops */

    /* Status line */
    int p = 0;
    while (p < g_hdr_end && g_raw[p] != '\n') p++;
    int sl = imin(p, (int)sizeof(g_status_line)-1);
    memcpy(g_status_line, g_raw, (u32)sl);
    g_status_line[sl] = 0;
    /* Remove \r */
    for (int i = 0; g_status_line[i]; i++) if (g_status_line[i]=='\r') g_status_line[i]=0;

    /* Per-header scan */
    p = 0;
    while (p < g_hdr_end) {
        int line_start = p;
        while (p < g_hdr_end && g_raw[p] != '\n') p++;
        g_raw[p-1] = 0;  /* kill \r */
        char *line = g_raw + line_start;
        if      (starts_with(line, "Transfer-Encoding:") || starts_with(line, "transfer-encoding:"))
            g_chunked = (strstr(line,"chunked") != NULL);
        else if (starts_with(line, "Content-Length:")    || starts_with(line, "content-length:")) {
            char *v = line; while (*v && *v != ':') v++; if (*v) v++;
            while (*v == ' ') v++;
            g_content_len = katoi(v);
        }
        else if (starts_with(line, "Content-Type:") || starts_with(line, "content-type:")) {
            char *v = line; while (*v && *v != ':') v++; if (*v) v++;
            while (*v == ' ') v++;
            kstrcpy(g_content_type, v, sizeof g_content_type);
        }
        else if (starts_with(line, "Location:") || starts_with(line, "location:")) {
            if (g_redirects < BR_MAX_REDIRECTS) {
                char *v = line; while (*v && *v != ':') v++; if (*v) v++;
                while (*v == ' ') v++;
                char new_url[BR_URL_MAX];
                resolve_url(g_url, v, new_url, sizeof new_url);
                g_redirects++;
                fetch_reset();
                kstrcpy(g_url, new_url, sizeof g_url);
                if (!parse_url(new_url)) { br_set_error("Bad redirect URL"); return; }
                g_state = F_DNS;
                if (net_parse_ip(g_host, &g_peer_ip)) g_state = F_CONNECT;
                else {
                    g_dns_h = net_dns_start(g_host);
                    if (g_dns_h < 0) br_set_error("DNS resolver busy");
                }
                return;
            }
        }
        p++;  /* skip past NUL we inserted */
    }
    /* Restore header area so render sees full text */
    g_raw[g_hdr_end-1] = '\n';
}

/* ================================================================== */
/*  Fetch state machine — stepped from browser_tick()                 */
/* ================================================================== */
static void fetch_step(void) {
    if (g_state == F_IDLE || g_state == F_DONE || g_state == F_ERROR) return;

    /* Timeout guard */
    if (ticks() - g_t0 > BR_TIMEOUT_MS) { br_set_error("Connection timed out"); return; }

    /* ---- F_DNS ---------------------------------------------------- */
    if (g_state == F_DNS) {
        int r = net_dns_poll(g_dns_h, &g_peer_ip);
        if (r == 0) return;  /* still pending */
        net_dns_free(g_dns_h); g_dns_h = -1;
        if (r != 1) {
            br_set_error(r==-2 ? "DNS: host not found" :
                         r==-1 ? "DNS: timed out" : "DNS: no server configured");
            return;
        }
        g_state = F_CONNECT;
    }

    /* ---- F_CONNECT ------------------------------------------------- */
    if (g_state == F_CONNECT) {
        g_tcp = tcp_open(g_peer_ip, (u16)g_port);
        if (g_tcp < 0) { br_set_error("No free TCP socket"); return; }
        g_state = F_TLS_HANDSHAKE;  /* also used for plain HTTP connect-wait */
    }

    /* ---- F_TLS_HANDSHAKE ------------------------------------------ */
    if (g_state == F_TLS_HANDSHAKE) {
        int s = tcp_status(g_tcp);
        if (s == TCP_CONNECTING) return;
        if (s != TCP_OPEN) {
            br_set_error(s==TCP_REFUSED ? "Connection refused" :
                         s==TCP_TIMEOUT ? "Connection timed out" : "Connection reset");
            return;
        }
        if (!g_https) {
            g_tls_ready = 1;  /* no TLS needed */
            g_state = F_SEND_REQ;
            return;
        }
        /* Initiate TLS if not started */
        if (!g_tls_ready) {
            br_ssl_client_init_full(&g_ssl_ctx, NULL, 0);  /* skip cert verify */
            br_ssl_client_reset(&g_ssl_ctx, g_host, 0);
            br_ssl_engine_set_io(&g_ssl_ctx.eng, tls_recv_cb, tls_send_cb, NULL);
        }
        br_ssl_engine_run(&g_ssl_ctx.eng);
        int st = br_ssl_engine_current_state(&g_ssl_ctx.eng);
        if (st & BR_SSL_CLOSED) {
            br_set_error("TLS handshake failed");
            return;
        }
        if (g_ssl_ctx.eng.hs == TLS_HS_DONE) {
            g_tls_ready = 1;
            g_state = F_SEND_REQ;
        }
        return;
    }

    /* ---- F_SEND_REQ ----------------------------------------------- */
    if (g_state == F_SEND_REQ) {
        static char req[1024];
        if (g_req_off == 0) {
            kt_snprintf(req, sizeof req,
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: Ktuyome/" KT_VERSION " (text browser)\r\n"
                "Accept: text/html,text/plain;q=0.9,*/*;q=0.8\r\n"
                "Accept-Encoding: identity\r\n"
                "Connection: close\r\n"
                "\r\n",
                g_path, g_host);
        }
        int rlen = (int)strlen(req);
        while (g_req_off < rlen) {
            int sent;
            if (g_https && g_tls_ready) {
                sent = br_ssl_engine_sendapp_buf(&g_ssl_ctx.eng,
                    (const uint8_t*)req + g_req_off,
                    (size_t)(rlen - g_req_off));
                br_ssl_engine_run(&g_ssl_ctx.eng);
            } else {
                sent = tcp_send(g_tcp, req + g_req_off, rlen - g_req_off);
            }
            if (sent <= 0) break;
            g_req_off += sent;
        }
        if (g_req_off >= rlen) g_state = F_RECV_HDRS;
        return;
    }

    /* ---- F_RECV_HDRS / F_RECV_BODY -------------------------------- */
    if (g_state == F_RECV_HDRS || g_state == F_RECV_BODY) {
        /* Pull data */
        while (g_raw_len < BR_BUF_SIZE - 1) {
            uint8_t tmp[1200]; int n;
            if (g_https && g_tls_ready) {
                br_ssl_engine_run(&g_ssl_ctx.eng);
                n = br_ssl_engine_recvapp_buf(&g_ssl_ctx.eng, tmp, sizeof tmp);
            } else {
                n = tcp_recv(g_tcp, tmp, sizeof tmp);
            }
            if (n <= 0) break;
            int take = imin(n, BR_BUF_SIZE - 1 - g_raw_len);
            memcpy(g_raw + g_raw_len, tmp, (u32)take);
            g_raw_len += take;
            g_fetch_bytes += take;
        }

        /* Check for header end */
        if (g_state == F_RECV_HDRS && !g_hdr_end) {
            for (int i = 0; i + 3 < g_raw_len; i++) {
                if (g_raw[i]=='\r'&&g_raw[i+1]=='\n'&&g_raw[i+2]=='\r'&&g_raw[i+3]=='\n') {
                    g_state = F_RECV_BODY;
                    parse_response_headers();
                    if (g_state != F_RECV_BODY) return;  /* redirect happened */
                    break;
                }
            }
        }

        /* Done? */
        int done = 0;
        if (g_https) {
            int est = br_ssl_engine_current_state(&g_ssl_ctx.eng);
            done = (est & BR_SSL_CLOSED) != 0;
        } else {
            done = tcp_eof(g_tcp);
        }
        if (!done && g_content_len >= 0 && g_hdr_end > 0) {
            done = (g_raw_len - g_hdr_end >= g_content_len);
        }
        if (done) {
            /* Chunked decode if needed */
            if (g_chunked && g_hdr_end > 0) {
                static char decoded[BR_BUF_SIZE];
                int dl = decode_chunked(g_raw + g_hdr_end,
                                        g_raw_len - g_hdr_end,
                                        decoded, BR_BUF_SIZE);
                memcpy(g_raw + g_hdr_end, decoded, (u32)dl);
                g_raw_len = g_hdr_end + dl;
            }
            g_raw[g_raw_len] = 0;
            g_fetch_ms = ticks() - g_t0;
            g_state = F_DONE;
            g_redirects = 0;
            render_page();
            tcp_close(g_tcp); g_tcp = -1;
            ui_wake();
        }
        return;
    }
}

/* ================================================================== */
/*  App lifecycle                                                      */
/* ================================================================== */
void browser_open(void) {
    static int first = 1;
    if (first) {
        first = 0;
        g_tcp = -1; g_dns_h = -1; g_state = F_IDLE;
        g_show_source = 0; g_tab = 0;
        g_nlines = 0; g_nlinks = 0; g_scroll = 0; g_cur_link = -1;
        g_redirects = 0; g_hist_n = 0;
        g_js_len = 0; g_js_log_n = 0;
        g_err_msg[0] = 0;
        g_status_line[0] = 0;
        g_url[0] = 0;

        /* Show a welcome page */
        kstrcpy(g_raw,
            "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n"
            "<html><head><title>Ktuyome Browser</title></head><body>"
            "<h1>Ktuyome Web Browser</h1>"
            "<p>Type a URL in the bar at the top and press Enter.</p>"
            "<p>Supports <b>http://</b> and <b>https://</b> (TLS 1.2).</p>"
            "<h2>Keyboard shortcuts</h2>"
            "<ul>"
            "<li>F5 — reload page</li>"
            "<li>Tab — cycle through links</li>"
            "<li>Enter on link — navigate</li>"
            "<li>Alt+Left — back</li>"
            "<li>Ctrl+U — view source</li>"
            "<li>Ctrl+S — save page to /downloads/</li>"
            "<li>PgUp / PgDn — scroll</li>"
            "</ul>"
            "<h2>About</h2>"
            "<p>JavaScript engine: quickjs_kt (ES2020 subset)</p>"
            "<p>TLS: BearSSL amalgam, TLS 1.2, ECDHE-RSA-AES128-CBC-SHA256</p>"
            "</body></html>",
            BR_BUF_SIZE);
        g_raw_len = (int)strlen(g_raw);
        g_hdr_end = 0;
        render_page();

        /* Initialise JS engine */
        g_js_rt  = JS_NewRuntime();
        g_js_ctx = JS_NewContext(g_js_rt);
        JS_SetConsoleCallback(g_js_ctx, js_console_cb);
        JS_SetDocWriteCallback(g_js_ctx, js_docwrite_cb);
        JS_SetMemoryLimit(g_js_rt, 8*1024*1024);
    }
    ui_wake();
}

void browser_close(void) {
    /* Keep state so background fetch continues */
}

/* ================================================================== */
/*  Tick (called every main-loop pass from apps_tick)                 */
/* ================================================================== */
void browser_tick(void) {
    net_poll();
    if (g_state > F_IDLE && g_state < F_DONE) {
        fetch_step();
        ui_wake();
    }
}

/* ================================================================== */
/*  Input handler                                                      */
/* ================================================================== */
int browser_key(const Event *e) {
    /* URL bar active — delegate to Prompt */
    if (g_prompt.active) {
        int r = pr_key(&g_prompt, e);
        if (r == 1) {
            /* confirmed — navigate */
            char url[BR_URL_MAX];
            kstrcpy(url, g_prompt.buf, sizeof url);
            /* Auto-prefix http:// if missing */
            if (!starts_with(url,"http://") && !starts_with(url,"https://")) {
                char tmp[BR_URL_MAX]; kt_snprintf(tmp,sizeof tmp,"https://%s",url);
                kstrcpy(url,tmp,sizeof url);
            }
            g_show_source = 0;
            g_state = F_IDLE;
            fetch_reset();
            fetch_start(url);
            ui_wake(); return 1;
        }
        if (r == 2) { /* cancelled */ ui_wake(); return 1; }
        return 1;
    }

    /* Clipboard: Ctrl+C copies the page address (Ctrl+Shift+C the focused link's), Ctrl+A copies the page text, Ctrl+V pastes into the URL bar */
    int ck = clip_key(e);
    if (ck == CK_COPY) {
        const char *u = ((e->mods & MOD_SHIFT) && g_cur_link >= 0 && g_cur_link < g_nlinks) ? g_links[g_cur_link].url : g_url;
        if (!u[0]) { ui_flash("No page loaded yet"); return 1; }
        clip_set(u, (int)strlen(u)); clip_notify("Copied address,", clip_len()); return 1;
    }
    if (ck == CK_ALL) {
        if (g_nlines <= 0) { ui_flash("No page text to copy"); return 1; }
        clip_begin();
        for (int i = 0; i < g_nlines; i++) { clip_add(g_lines[i].text, (int)strlen(g_lines[i].text)); clip_add("\n", 1); }
        clip_notify("Copied page text,", clip_len()); return 1;
    }
    if (ck == CK_PASTE) {
        pr_open(&g_prompt, "URL", "", 0); pr_paste(&g_prompt); ui_wake(); return 1;
    }
    if (ck == CK_CUT) return 1;

    /* Ctrl shortcuts */
    if (e->mods & MOD_CTRL) {
        if (e->key == 'l' || e->key == 'L') {
            pr_open(&g_prompt, "URL", g_url, 0); g_prompt.selall = 1;
            ui_wake(); return 1;
        }
        if (e->key == 'u' || e->key == 'U') {
            g_show_source ^= 1; render_page(); ui_wake(); return 1;
        }
        if (e->key == 's' || e->key == 'S') {
            /* save page */
            if (g_raw_len > 0) {
                char path[64]; kstrcpy(path, BR_DL_PATH, sizeof path);
                kstrcat(path, "page.html", sizeof path);
                fs_write_text(path, g_raw, g_raw_len);
                fs_save();
                ui_flash("Page saved to /downloads/page.html");
            }
            return 1;
        }
        if (e->key == 'r' || e->key == 'R') {
            /* reload */
            if (g_url[0]) { fetch_reset(); fetch_start(g_url); } return 1;
        }
        return 0;
    }

    /* Alt+Left — back */
    if ((e->mods & MOD_ALT) && e->key == K_LEFT) {
        if (g_hist_n >= 2) {
            g_hist_n--;  /* pop current */
            char prev[BR_URL_MAX]; kstrcpy(prev, g_history[g_hist_n-1], sizeof prev);
            fetch_reset(); fetch_start(prev);
        }
        return 1;
    }

    /* F5 — reload */
    if (e->key == K_F5) {
        if (g_url[0]) { fetch_reset(); fetch_start(g_url); } return 1;
    }

    /* Tab — cycle through links */
    if (e->key == K_TAB) {
        int dir = (e->mods & MOD_SHIFT) ? -1 : 1;
        if (g_nlinks > 0) {
            g_cur_link = (g_cur_link + dir + g_nlinks) % g_nlinks;
            /* scroll to show the focused link's first line */
            for (int i = 0; i < g_nlines; i++) {
                if (g_lines[i].link_idx == g_cur_link) {
                    g_scroll = imax(0, i - 2); break;
                }
            }
            ui_wake();
        }
        return 1;
    }

    /* Enter — navigate to focused link */
    if (e->key == K_ENTER) {
        if (g_cur_link >= 0 && g_cur_link < g_nlinks) {
            g_show_source = 0;
            fetch_reset();
            fetch_start(g_links[g_cur_link].url);
        } else {
            /* open URL bar if no link focused */
            pr_open(&g_prompt, "URL", g_url, 0);
        }
        return 1;
    }

    /* Scroll */
    if (e->key == K_UP)    { g_scroll = imax(0, g_scroll-1); ui_wake(); return 1; }
    if (e->key == K_DOWN)  { g_scroll = imax(0, imin(g_scroll+1, g_nlines-1)); ui_wake(); return 1; }
    if (e->key == K_PGUP)  { g_scroll = imax(0, g_scroll-20); ui_wake(); return 1; }
    if (e->key == K_PGDN)  { g_scroll = imax(0, imin(g_scroll+20, g_nlines-1)); ui_wake(); return 1; }
    if (e->key == K_HOME)  { g_scroll = 0; ui_wake(); return 1; }
    if (e->key == K_END)   { g_scroll = imax(0, g_nlines-1); ui_wake(); return 1; }

    /* Switch tabs: F1=page, F2=js_console */
    if (e->key == K_F1) { g_tab = 0; ui_wake(); return 1; }
    if (e->key == K_F2) { g_tab = 1; g_scroll = 0; ui_wake(); return 1; }

    /* Click on URL bar or any typed character opens URL prompt */
    if (e->type == EV_KEY && e->ch >= 32 && e->ch < 127) {
        pr_open(&g_prompt, "URL", g_url, 0);
        /* feed the character into the prompt */
        if (g_prompt.len < (int)sizeof(g_prompt.buf)-1) {
            g_prompt.buf[g_prompt.len++] = (char)e->ch;
            g_prompt.buf[g_prompt.len]   = 0;
        }
        ui_wake(); return 1;
    }

    return 0;
}

/* ================================================================== */
/*  Draw                                                               */
/* ================================================================== */
void browser_draw(int x, int y, int w, int h) {
    /* ---- toolbar -------------------------------------------------- */
    int tb_h = 48;
    gfx_rrect(x, y, w, tb_h, 12, WHITE_A(20), WHITE_A(12), 0);
    gfx_rrect(x, y, w, tb_h, 12, WHITE_A(28), WHITE_A(28), 1);

    /* Back button */
    int bw = 34;
    int back_on = g_hist_n >= 2;
    if (ui_btn(x+8, y+8, bw, 32, "<", 0) && back_on) {
        g_hist_n--;
        char prev[BR_URL_MAX]; kstrcpy(prev, g_history[g_hist_n-1], sizeof prev);
        fetch_reset(); fetch_start(prev);
    }

    /* Reload button */
    if (ui_btn(x+8+bw+4, y+8, bw, 32, "⟳", g_state > F_IDLE && g_state < F_DONE)) {
        if (g_url[0]) { fetch_reset(); fetch_start(g_url); }
    }

    /* URL bar */
    int bar_x = x + 8 + bw*2 + 12, bar_w = w - bar_x - 8 - x;
    if (!g_prompt.active) {
        gfx_rrect(bar_x, y+9, bar_w, 30, 15, BLACK_A(140), BLACK_A(140), 0);
        gfx_rrect(bar_x, y+9, bar_w, 30, 15, WHITE_A(50), WHITE_A(50), 1);
        /* Scheme icon color */
        u32 sc = g_https ? 0xFF4DD0A1 : WHITE_A(140);
        gfx_text(F_SM, bar_x+10, y+19, g_https ? "🔒" : "🌐", sc);
        /* URL text — clip to bar width */
        gfx_clip(bar_x+30, y+9, bar_w-36, 30);
        gfx_text(F_MDB, bar_x+30, y+15, g_url[0] ? g_url : "Enter URL...",
                 g_url[0] ? 0xFFFFFFFF : WHITE_A(100));
        gfx_noclip();
        if (ui_click_in(bar_x, y+9, bar_w, 30)) {
            pr_open(&g_prompt, "URL", g_url, 0);
            ui_wake();
        }
    } else {
        pr_draw(&g_prompt, bar_x, y+9, bar_w, 30);
    }

    /* ---- tabs ----------------------------------------------------- */
    int ty = y + tb_h + 4;
    int tab_h = 30;
    int tw = 90;
    const char *tab_names[2] = { "Page", "JS Console" };
    for (int i = 0; i < 2; i++) {
        u32 tc = (g_tab==i) ? ACC : WHITE_A(40);
        gfx_rrect(x + 8 + i*(tw+4), ty, tw, tab_h, 10, tc, tc, 0);
        gfx_text_c(F_MDB, x+8+i*(tw+4)+tw/2, ty+7, tab_names[i], 0xFFFFFFFF);
        if (ui_click_in(x+8+i*(tw+4), ty, tw, tab_h)) { g_tab=i; g_scroll=0; ui_wake(); }
    }

    /* ---- status bar ----------------------------------------------- */
    int sb_h = 24;
    int sb_y = y + h - sb_h;
    gfx_rrect(x, sb_y, w, sb_h, 8, BLACK_A(100), BLACK_A(100), 0);

    /* State indicator */
    const char *state_str[] = {"","Resolving DNS…","Connecting…","TLS handshake…",
                                "Sending request…","Receiving headers…","Receiving…","","Error"};
    u32 sc = (g_state==F_ERROR) ? 0xFFFF8A80 :
             (g_state==F_DONE)  ? 0xFF4DD0A1 :
             (g_state>F_IDLE)   ? 0xFFFFD27A : WHITE_A(120);
    gfx_text(F_SM, x+10, sb_y+7, state_str[g_state], sc);

    if (g_state == F_ERROR) {
        gfx_text(F_SM, x+130, sb_y+7, g_err_msg, 0xFFFF8A80);
    } else if (g_state == F_DONE || g_state == F_IDLE) {
        if (g_status_line[0]) {
            gfx_text(F_SM, x+10, sb_y+7, g_status_line, sc);
        }
        if (g_fetch_ms) {
            char t[64]; kt_snprintf(t, sizeof t, "  %d bytes  %ums", g_fetch_bytes, g_fetch_ms);
            gfx_text(F_SM, x+10+gfx_text_w(F_SM, g_status_line, -1), sb_y+7, t, WHITE_A(120));
        }
    } else {
        /* progress: bytes received so far */
        char t[48]; kt_snprintf(t, sizeof t, "%d bytes", g_raw_len);
        gfx_text_r(F_SM, x+w-10, sb_y+7, t, WHITE_A(140));
    }

    /* ---- viewport ------------------------------------------------- */
    int vp_y  = ty + tab_h + 6;
    int vp_h  = sb_y - vp_y - 4;
    int vp_x  = x + 6;
    int vp_w  = w - 12;

    gfx_rrect(vp_x, vp_y, vp_w, vp_h, 10, BLACK_A(90), BLACK_A(90), 0);
    gfx_rrect(vp_x, vp_y, vp_w, vp_h, 10, WHITE_A(18), WHITE_A(18), 1);
    gfx_clip(vp_x+4, vp_y+4, vp_w-8, vp_h-8);

    /* Mouse wheel scroll */
    if (g_wheel) {
        g_scroll = iclamp(g_scroll - g_wheel * 3, 0, imax(0, g_nlines-1));
        ui_wake();
    }

    int lh     = fonts[F_MONO].h + 2;
    int rows   = (vp_h - 8) / lh;
    int cx     = vp_x + 10;
    int cy_top = vp_y + 6;

    if (g_tab == 0) {
        /* page content */
        int shown = 0;
        for (int li = g_scroll; li < g_nlines && shown < rows; li++, shown++) {
            BrLine *bl = &g_lines[li];
            int ly = cy_top + shown * lh;

            /* Highlight focused link */
            int is_focused_link = (bl->link_idx >= 0 && bl->link_idx == g_cur_link);
            if (is_focused_link) {
                gfx_fill(cx-2, ly-1, vp_w-16, lh, WHITE_A(30));
            }

            /* click to navigate */
            if (bl->link_idx >= 0 && ui_click_in(cx-2, ly-1, vp_w-16, lh)) {
                g_cur_link = bl->link_idx;
                g_show_source = 0;
                fetch_reset();
                fetch_start(g_links[bl->link_idx].url);
            }

            /* choose color */
            u32 col;
            if      (bl->color)        col = bl->color;
            else if (bl->link_idx>=0)  col = is_focused_link ? ACC : col_lerp(ACC, 0xFFFFFFFF, 60);
            else if (bl->is_heading==1) col = 0xFFFFFFFF;
            else if (bl->is_heading==2) col = WHITE_A(230);
            else if (bl->is_heading==3) col = WHITE_A(200);
            else                        col = WHITE_A(180);

            int font = bl->is_heading ? F_MDB : F_MONO;
            gfx_text(font, cx, ly, bl->text, col);
        }

        /* scrollbar */
        if (g_nlines > rows) {
            int sb_x2 = vp_x + vp_w - 8;
            int track = vp_h - 8;
            int thumb = imax(20, track * rows / g_nlines);
            int thumb_y = vp_y + 4 + (track - thumb) * g_scroll / imax(1, g_nlines - rows);
            gfx_rrect(sb_x2, vp_y+4, 4, track, 2, WHITE_A(20), WHITE_A(20), 0);
            gfx_rrect(sb_x2, thumb_y, 4, thumb, 2, WHITE_A(80), WHITE_A(80), 0);
        }

        /* loading spinner (animated dots) */
        if (g_state > F_IDLE && g_state < F_DONE) {
            int frame = (int)(ticks() / 200) % 4;
            char spin[5] = "...."; spin[frame] = 'O'; spin[4] = 0;
            gfx_text_c(F_MD, vp_x + vp_w/2, vp_y + vp_h/2 - 10, spin, WHITE_A(160));
        }

    } else {
        /* JS console */
        for (int li = g_scroll; li < g_js_log_n && (li - g_scroll) < rows; li++) {
            gfx_text(F_MONO, cx, cy_top + (li - g_scroll) * lh,
                     g_js_log[li], WHITE_A(200));
        }
        if (g_js_log_n == 0) {
            gfx_text(F_MONO, cx, cy_top, "(no JS output yet)", WHITE_A(80));
        }
    }

    gfx_noclip();

    /* ---- link list overlay (bottom of viewport) ------------------- */
    if (g_tab == 0 && g_cur_link >= 0 && g_cur_link < g_nlinks) {
        const char *link_url = g_links[g_cur_link].url;
        int uw = gfx_text_w(F_SM, link_url, -1) + 16;
        int ux = iclamp(vp_x + 4, vp_x, vp_x + vp_w - uw);
        gfx_rrect(ux, sb_y - 22, uw, 18, 9, BLACK_A(200), BLACK_A(200), 0);
        gfx_text(F_SM, ux+8, sb_y-20, link_url, WHITE_A(180));
    }
}

/* ================================================================== */
/*  Static-bytes introspection (task manager)                         */
/* ================================================================== */
u32 browser_static_bytes(void) {
    return (u32)(sizeof(g_raw) + sizeof(g_lines) +
                 sizeof(g_links) + sizeof(g_js_buf) +
                 sizeof(g_js_log));
}
