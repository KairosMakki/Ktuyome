/*
 * discord.c — Discord client app for ktuyome OS
 *
 * A text-mode Discord client implementing:
 *   - Token login / logout (bot or user token, persisted to disk)
 *   - WebSocket Gateway connection (RFC 6455 over TLS 1.2 via BearSSL)
 *   - Real-time MESSAGE_CREATE events with heartbeat
 *   - REST API calls (GET guilds, channels, messages; POST send message)
 *   - Channel list, message view, message input bar
 *
 * All network I/O is polled from discord_tick() (called every main-loop pass).
 * Uses the kmalloc heap for JSON parsing.  Two TCP sockets: one persistent
 * Gateway WebSocket, one for one-shot REST API calls.
 *
 * Discord API version: v10
 * Intents: GUILDS (1) | GUILD_MESSAGES (512) = 513
 */
#include "ktuyome.h"
#include "vendor/bearssl/bearssl.h"

extern int br_last_alert;

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */
#define D_API_HOST      "discord.com"
#define D_GATE_HOST     "gateway.discord.gg"
#define D_GATE_DEFAULT  "gateway.discord.gg"  /* fallback if REST lookup fails */
#define D_API_PORT      443
#define D_GATE_PORT     443
#define D_API_BASE      "/api/v10"
#define D_GATE_PATH     "/?v=10&encoding=json"
#define D_API_VER       10

#define D_INTENTS  (1 | (1 << 9))  /* GUILDS | GUILD_MESSAGES */

#define D_TOKEN_MAX     256
#define D_BUF_SIZE      (128 * 1024)   /* WebSocket + REST receive buffer */
#define D_SEND_BUF      (16 * 1024)   /* WebSocket send buffer            */
#define D_REST_BUF      (64 * 1024)    /* REST response buffer             */
#define D_MAX_GUILDS    64
#define D_MAX_CHANNELS  256
#define D_MAX_MSGS      100
#define D_MSG_LEN       1024
#define D_NAME_LEN      100
#define D_ID_LEN        24
#define D_INPUT_MAX     512
#define D_WS_KEY_LEN    24           /* 16 bytes → 24 base64 chars + NUL */

/* Gateway opcodes */
#define OP_DISPATCH       0
#define OP_HEARTBEAT      1
#define OP_IDENTIFY       2
#define OP_RESUME         6
#define OP_RECONNECT      7
#define OP_INVALID_SESS  9
#define OP_HELLO          10
#define OP_HEARTBEAT_ACK  11

/* WebSocket opcodes */
#define WS_CONT   0x0
#define WS_TEXT   0x1
#define WS_BIN    0x2
#define WS_CLOSE  0x8
#define WS_PING   0x9
#define WS_PONG   0xA

/* ================================================================== */
/*  Data structures                                                    */
/* ================================================================== */
typedef struct {
    char id[D_ID_LEN];
    char name[D_NAME_LEN];
    u64  id_num;
} DGuild;

typedef struct {
    char id[D_ID_LEN];
    char guild_id[D_ID_LEN];
    char name[D_NAME_LEN];
    int  type;   /* 0=text, 2=voice, 4=category, 5=annoucement, 15=forum */
} DChannel;

typedef struct {
    char id[D_ID_LEN];
    char channel_id[D_ID_LEN];
    char author[D_NAME_LEN];
    char content[D_MSG_LEN];
    u32  ts;     /* ticks() when received */
} DMsg;

/* JSON value types */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal JVal;
struct JVal {
    JType type;
    union {
        int    b;
        double num;
        char  *str;     /* owned */
        struct { JVal **items; int count; } arr;
        struct { char **keys; JVal **vals; int count; } obj;
    };
};

/* Connection state */
typedef enum {
    DC_OFFLINE = 0,
    DC_RESOLVING_API,        /* resolve discord.com for REST gateway lookup */
    DC_FETCH_GATEWAY_URL,    /* GET /api/v10/gateway via REST */
    DC_RESOLVING_GATE,
    DC_CONNECTING_GATE,
    DC_TLS_HANDSHAKE,
    DC_WS_HANDSHAKE,
    DC_GATE_HELLO,
    DC_IDENTIFYING,
    DC_READY,
    DC_DISCONNECTED,
    DC_ERROR
} DConnState;

/* REST API request types */
typedef enum {
    REST_NONE = 0,
    REST_GET_GATEWAY,
    REST_GET_GUILDS,
    REST_GET_CHANNELS,
    REST_GET_MESSAGES,
    REST_SEND_MESSAGE,
    REST_GET_USER
} RestReq;

/* ================================================================== */
/*  Static state                                                       */
/* ================================================================== */

/* --- Connection --- */
static DConnState  g_conn = DC_OFFLINE;
static int         g_gate_tcp = -1;
static int         g_dns_h    = -1;
static u32         g_gate_ip;
static u32         g_api_ip;
static char        g_gate_host[128];   /* resolved gateway hostname from REST API */
static u32         g_conn_t0;
static int         g_last_seq;          /* last gateway sequence number */
static u32         g_next_heartbeat;    /* ticks() when next heartbeat due */
static int         g_heartbeat_interval;
static int         g_got_ack;           /* got heartbeat ACK since last beat */

/* --- TLS (Gateway) --- */
static br_ssl_client_context g_gate_ssl;
static int                    g_gate_tls_ready;

/* --- TLS (REST) --- */
static br_ssl_client_context g_rest_ssl;
static int                    g_rest_tcp;
static int                    g_rest_tls_ready;
static int                    g_rest_dns_h;

/* --- Buffers --- */
static char  g_recv_buf[D_BUF_SIZE];     /* WebSocket receive accumulator */
static int   g_recv_len;
static int   g_recv_pos;                 /* unconsumed WS frame data start */
static char  g_send_buf[D_SEND_BUF];     /* WebSocket send accumulator */
static int   g_send_len;
static char  g_rest_buf[D_REST_BUF];     /* REST response */
static int   g_rest_len;
static int   g_rest_req_off;             /* bytes of HTTP request sent */

/* --- Token --- */
static char  g_token[D_TOKEN_MAX];
static int   g_token_len;
static int   g_is_bot;                   /* 1 = bot token, 0 = user token */

/* --- User info (from READY) --- */
static char  g_username[D_NAME_LEN];
static char  g_userid[D_ID_LEN];

/* --- Data --- */
static DGuild    g_guilds[D_MAX_GUILDS];
static int        g_nguilds;
static DChannel   g_channels[D_MAX_CHANNELS];
static int        g_nchannels;
static DMsg       g_msgs[D_MAX_MSGS];    /* ring buffer */
static int        g_msg_head;           /* next write slot */
static int        g_msg_count;          /* messages in buffer */

/* --- UI state --- */
static int        g_sel_guild;          /* selected guild index */
static int        g_sel_channel;       /* selected channel index */
static int        g_channel_scroll;    /* message scroll offset */
static int        g_channel_list_scroll;
static char       g_input[D_INPUT_MAX];
static int        g_input_len;
static int        g_focused_panel;      /* 0=channels 1=messages 2=input */
static char       g_status[128];
static int        g_view_mode;          /* 0=guild/channel list 1=channel view */
static Prompt     g_login_prompt;       /* token entry */

/* --- REST API state machine --- */
static RestReq    g_rest_state;
static int        g_rest_phase;         /* 0=dns 1=connect 2=tls 3=http 4=recv 5=done */
static char       g_rest_path[256];     /* REST endpoint path */
static char       g_rest_body[D_MSG_LEN]; /* POST body */
static int        g_rest_body_len;
static int        g_rest_is_post;
static u32        g_rest_t0;

/* ================================================================== */
/*  Base64 encoder (for WebSocket Sec-WebSocket-Key)                   */
/* ================================================================== */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const u8 *in, int len, char *out) {
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        u32 v = (u32)in[i] << 16;
        if (i + 1 < len) v |= (u32)in[i + 1] << 8;
        if (i + 2 < len) v |= (u32)in[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? b64tab[v & 63]       : '=';
    }
    out[o] = 0;
    return o;
}

/* ================================================================== */
/*  JSON parser (recursive descent)                                    */
/*  Allocates from kmalloc; free with json_free().                    */
/* ================================================================== */
typedef struct { const char *s; int len; int pos; } JParser;

static void j_skip_ws(JParser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static JVal *j_parse_val(JParser *p);

static JVal *j_new(JType t) {
    JVal *v = (JVal *)kmalloc(sizeof(JVal));
    if (v) memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

static void json_free(JVal *v) {
    if (!v) return;
    switch (v->type) {
    case J_STR: if (v->str) kfree(v->str); break;
    case J_ARR:
        for (int i = 0; i < v->arr.count; i++) json_free(v->arr.items[i]);
        if (v->arr.items) kfree(v->arr.items);
        break;
    case J_OBJ:
        for (int i = 0; i < v->obj.count; i++) {
            if (v->obj.keys[i]) kfree(v->obj.keys[i]);
            json_free(v->obj.vals[i]);
        }
        if (v->obj.keys) kfree(v->obj.keys);
        if (v->obj.vals) kfree(v->obj.vals);
        break;
    default: break;
    }
    kfree(v);
}

static JVal *j_parse_str(JParser *p) {
    p->pos++; /* skip opening quote */
    int start = p->pos;
    while (p->pos < p->len && p->s[p->pos] != '"') {
        if (p->s[p->pos] == '\\' && p->pos + 1 < p->len) p->pos += 2;
        else p->pos++;
    }
    int raw_len = p->pos - start;
    p->pos++; /* skip closing quote */

    /* Unescape */
    char *out = (char *)kmalloc((u32)(raw_len + 1));
    if (!out) return j_new(J_NULL);
    int o = 0;
    for (int i = 0; i < raw_len; i++) {
        if (p->s[start + i] == '\\' && i + 1 < raw_len) {
            char e = p->s[start + ++i];
            switch (e) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case '"': out[o++] = '"';  break;
            case '\\':out[o++] = '\\'; break;
            case '/': out[o++] = '/';  break;
            case 'b': out[o++] = '\b'; break;
            case 'f': out[o++] = '\f'; break;
            case 'u': {
                /* decode \uXXXX */
                if (i + 4 < raw_len) {
                    u32 cp = 0;
                    for (int j = 0; j < 4; j++) {
                        char c = p->s[start + i + 1 + j];
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= c - '0';
                        else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                    }
                    i += 4;
                    /* UTF-8 encode */
                    if (cp < 0x80) out[o++] = (char)cp;
                    else if (cp < 0x800) {
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 63));
                    } else {
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 63));
                        out[o++] = (char)(0x80 | (cp & 63));
                    }
                }
                break;
            }
            default: out[o++] = e; break;
            }
        } else {
            out[o++] = p->s[start + i];
        }
    }
    out[o] = 0;
    JVal *v = j_new(J_STR);
    v->str = out;
    return v;
}

static JVal *j_parse_num(JParser *p) {
    char *end;
    double d = kt_strtod(p->s + p->pos, &end);
    p->pos = (int)(end - p->s);
    JVal *v = j_new(J_NUM);
    v->num = d;
    return v;
}

static JVal *j_parse_arr(JParser *p) {
    p->pos++; /* skip [ */
    JVal *v = j_new(J_ARR);
    int cap = 8;
    v->arr.items = (JVal **)kmalloc(sizeof(JVal *) * (u32)cap);
    j_skip_ws(p);
    while (p->pos < p->len && p->s[p->pos] != ']') {
        if (v->arr.count >= cap) {
            cap *= 2;
            JVal **nb = (JVal **)kmalloc(sizeof(JVal *) * (u32)cap);
            memcpy(nb, v->arr.items, sizeof(JVal *) * (u32)v->arr.count);
            kfree(v->arr.items);
            v->arr.items = nb;
        }
        v->arr.items[v->arr.count++] = j_parse_val(p);
        j_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') p->pos++;
        j_skip_ws(p);
    }
    p->pos++; /* skip ] */
    return v;
}

static JVal *j_parse_obj(JParser *p) {
    p->pos++; /* skip { */
    JVal *v = j_new(J_OBJ);
    int cap = 8;
    v->obj.keys = (char **)kmalloc(sizeof(char *) * (u32)cap);
    v->obj.vals = (JVal **)kmalloc(sizeof(JVal *) * (u32)cap);
    j_skip_ws(p);
    while (p->pos < p->len && p->s[p->pos] != '}') {
        if (v->obj.count >= cap) {
            cap *= 2;
            char **nk = (char **)kmalloc(sizeof(char *) * (u32)cap);
            JVal **nv = (JVal **)kmalloc(sizeof(JVal *) * (u32)cap);
            memcpy(nk, v->obj.keys, sizeof(char *) * (u32)v->obj.count);
            memcpy(nv, v->obj.vals, sizeof(JVal *) * (u32)v->obj.count);
            kfree(v->obj.keys); kfree(v->obj.vals);
            v->obj.keys = nk; v->obj.vals = nv;
        }
        j_skip_ws(p);
        if (p->s[p->pos] != '"') break;
        JVal *key = j_parse_str(p);
        v->obj.keys[v->obj.count] = key->str;
        kfree(key); /* just take the string */
        j_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ':') p->pos++;
        j_skip_ws(p);
        v->obj.vals[v->obj.count] = j_parse_val(p);
        v->obj.count++;
        j_skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') p->pos++;
        j_skip_ws(p);
    }
    p->pos++; /* skip } */
    return v;
}

static JVal *j_parse_val(JParser *p) {
    j_skip_ws(p);
    if (p->pos >= p->len) return j_new(J_NULL);
    char c = p->s[p->pos];
    if (c == '{') return j_parse_obj(p);
    if (c == '[') return j_parse_arr(p);
    if (c == '"') return j_parse_str(p);
    if (c == '-' || (c >= '0' && c <= '9')) return j_parse_num(p);
    if (c == 't' && p->pos + 4 <= p->len && !memcmp(p->s + p->pos, "true", 4)) {
        p->pos += 4; JVal *v = j_new(J_BOOL); v->b = 1; return v;
    }
    if (c == 'f' && p->pos + 5 <= p->len && !memcmp(p->s + p->pos, "false", 5)) {
        p->pos += 5; JVal *v = j_new(J_BOOL); v->b = 0; return v;
    }
    if (c == 'n' && p->pos + 4 <= p->len && !memcmp(p->s + p->pos, "null", 4)) {
        p->pos += 4; return j_new(J_NULL);
    }
    p->pos++;
    return j_new(J_NULL);
}

static JVal *json_parse(const char *s, int len) {
    JParser p = { s, len, 0 };
    return j_parse_val(&p);
}

/* JSON accessors */
static const char *j_str(const JVal *v) {
    return (v && v->type == J_STR) ? v->str : "";
}
static double j_num(const JVal *v) {
    return (v && v->type == J_NUM) ? v->num : 0;
}
static int j_bool(const JVal *v) {
    return (v && v->type == J_BOOL) ? v->b : 0;
}
static const JVal *j_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->obj.count; i++)
        if (!strcmp(obj->obj.keys[i], key)) return obj->obj.vals[i];
    return NULL;
}
static const JVal *j_arr(const JVal *arr, int idx) {
    if (!arr || arr->type != J_ARR || idx < 0 || idx >= arr->arr.count) return NULL;
    return arr->arr.items[idx];
}
static int j_arr_len(const JVal *arr) {
    return (arr && arr->type == J_ARR) ? arr->arr.count : 0;
}

/* Parse a Discord snowflake string to u64 (for dedup) */
static u64 snowflake(const char *s) {
    u64 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0');
    return v;
}

/* ================================================================== */
/*  TLS I/O callbacks for Gateway and REST connections                 */
/* ================================================================== */
static int gate_tls_recv(void *ctx, u8 *buf, size_t len) {
    (void)ctx;
    int n = tcp_recv(g_gate_tcp, buf, (int)len);
    return n > 0 ? n : 0;
}
static int gate_tls_send(void *ctx, const u8 *buf, size_t len) {
    (void)ctx;
    int n = tcp_send(g_gate_tcp, buf, (int)len);
    return n > 0 ? n : 0;
}
static int rest_tls_recv(void *ctx, u8 *buf, size_t len) {
    (void)ctx;
    int n = tcp_recv(g_rest_tcp, buf, (int)len);
    return n > 0 ? n : 0;
}
static int rest_tls_send(void *ctx, const u8 *buf, size_t len) {
    (void)ctx;
    int n = tcp_send(g_rest_tcp, buf, (int)len);
    return n > 0 ? n : 0;
}

/* ================================================================== */
/*  WebSocket framing (RFC 6455) — client side                         */
/* ================================================================== */

/* Write a masked text frame into g_send_buf.
 * Returns total bytes written (frame header + masked payload). */
static int ws_write_text_frame(const char *payload, int plen) {
    if (g_send_len + 14 + plen > D_SEND_BUF) return -1;
    u8 mask[4];
    br_prng_bytes(mask, 4);  /* reuse BearSSL PRNG */

    u8 *hdr = (u8 *)(g_send_buf + g_send_len);
    int hlen = 0;
    hdr[hlen++] = 0x80 | WS_TEXT;  /* FIN + text */
    if (plen < 126) {
        hdr[hlen++] = 0x80 | (u8)plen;  /* MASK set */
    } else if (plen < 65536) {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (u8)(plen >> 8);
        hdr[hlen++] = (u8)plen;
    } else {
        hdr[hlen++] = 0x80 | 127;
        for (int i = 5; i >= 0; i--) hdr[hlen++] = 0; /* 64-bit length (high) */
        hdr[hlen++] = (u8)(plen >> 24);
        hdr[hlen++] = (u8)(plen >> 16);
        hdr[hlen++] = (u8)(plen >> 8);
        hdr[hlen++] = (u8)plen;
    }
    /* masking key */
    hdr[hlen++] = mask[0];
    hdr[hlen++] = mask[1];
    hdr[hlen++] = mask[2];
    hdr[hlen++] = mask[3];
    /* masked payload */
    u8 *p = (u8 *)(g_send_buf + g_send_len + hlen);
    for (int i = 0; i < plen; i++)
        p[i] = (u8)payload[i] ^ mask[i % 4];
    g_send_len += hlen + plen;
    return hlen + plen;
}

/* Try to read one complete WebSocket frame from g_recv_buf.
 * On success: sets *out_type, copies payload to out (up to cap), returns payload length.
 * If the frame is incomplete, returns -1.
 * Advances g_recv_pos past the consumed frame. */
static int ws_read_frame(u8 *out_type, char *out, int cap) {
    if (g_recv_pos >= g_recv_len) return -1;
    u8 *buf = (u8 *)g_recv_buf + g_recv_pos;
    int avail = g_recv_len - g_recv_pos;
    if (avail < 2) return -1;

    u8 b0 = buf[0], b1 = buf[1];
    int fin = (b0 >> 7) & 1;
    *out_type = b0 & 0x0F;
    int masked = (b1 >> 7) & 1;
    int plen = b1 & 0x7F;
    int hdr_len = 2;

    if (plen == 126) {
        if (avail < 4) return -1;
        plen = (buf[2] << 8) | buf[3];
        hdr_len = 4;
    } else if (plen == 127) {
        if (avail < 10) return -1;
        plen = (buf[6] << 24) | (buf[7] << 16) | (buf[8] << 8) | buf[9];
        hdr_len = 10;
    }
    if (masked) hdr_len += 4;  /* server shouldn't mask, but handle it */
    if (avail < hdr_len + plen) return -1;

    /* copy payload (unmask if needed) */
    u8 *mask = masked ? buf + hdr_len - 4 : NULL;
    u8 *payload = buf + hdr_len;
    int copy = plen < cap ? plen : cap;
    for (int i = 0; i < copy; i++) {
        out[i] = mask ? (char)(payload[i] ^ mask[i % 4]) : (char)payload[i];
    }
    if (cap > 0) out[copy < cap ? copy : cap - 1] = 0;  /* NUL-terminate if room */

    g_recv_pos += hdr_len + plen;
    (void)fin;
    return plen;
}

/* Compact the receive buffer: move unconsumed data to front */
static void ws_compact_recv(void) {
    if (g_recv_pos > 0 && g_recv_pos < g_recv_len) {
        memmove(g_recv_buf, g_recv_buf + g_recv_pos, (u32)(g_recv_len - g_recv_pos));
        g_recv_len -= g_recv_pos;
    } else if (g_recv_pos >= g_recv_len) {
        g_recv_len = 0;
    }
    g_recv_pos = 0;
}

/* Flush pending send data through TLS */
static void gate_flush_send(void) {
    if (!g_gate_tls_ready || g_send_len == 0) return;
    while (g_send_len > 0) {
        int n = br_ssl_engine_sendapp_buf(&g_gate_ssl.eng,
                                          (const u8 *)g_send_buf,
                                          (size_t)g_send_len);
        if (n <= 0) break;
        /* shift remaining */
        int rem = g_send_len - n;
        if (rem > 0) memmove(g_send_buf, g_send_buf + n, (u32)rem);
        g_send_len = rem;
    }
    br_ssl_engine_run(&g_gate_ssl.eng);
}

/* Read decrypted data from TLS into the receive buffer */
static int gate_pump_recv(void) {
    if (!g_gate_tls_ready) return 0;
    br_ssl_engine_run(&g_gate_ssl.eng);
    int total = 0;
    while (g_recv_len < D_BUF_SIZE - 1) {
        u8 tmp[1200];
        int n = br_ssl_engine_recvapp_buf(&g_gate_ssl.eng, tmp, sizeof tmp);
        if (n <= 0) break;
        int take = imin(n, D_BUF_SIZE - 1 - g_recv_len);
        memcpy(g_recv_buf + g_recv_len, tmp, (u32)take);
        g_recv_len += take;
        total += take;
    }
    return total;
}

/* ================================================================== */
/*  REST API helper                                                     */
/*  Makes a single HTTPS request to discord.com, stores response       */
/*  in g_rest_buf.  Uses a separate TCP socket + TLS context.           */
/* ================================================================== */

static void rest_start_get(const char *path) {
    g_rest_state = REST_NONE;  /* will be set by caller after */
    g_rest_phase = 0;
    g_rest_is_post = 0;
    g_rest_path[0] = 0; kstrcat(g_rest_path, path, sizeof g_rest_path);
    g_rest_body_len = 0;
    g_rest_len = 0; g_rest_req_off = 0;
    g_rest_t0 = ticks();

    /* DNS resolve discord.com if not cached */
    if (g_api_ip) {
        g_rest_phase = 1;  /* skip to connect */
    } else {
        g_rest_dns_h = net_dns_start(D_API_HOST);
        if (g_rest_dns_h < 0) { g_rest_phase = 5; kt_snprintf(g_status, sizeof g_status, "DNS failed"); }
    }
}

static void rest_start_post(const char *path, const char *body) {
    g_rest_phase = 0;
    g_rest_is_post = 1;
    g_rest_path[0] = 0; kstrcat(g_rest_path, path, sizeof g_rest_path);
    kstrcpy(g_rest_body, body, sizeof g_rest_body);
    g_rest_body_len = (int)strlen(g_rest_body);
    g_rest_len = 0; g_rest_req_off = 0;
    g_rest_t0 = ticks();

    if (g_api_ip) g_rest_phase = 1;
    else {
        g_rest_dns_h = net_dns_start(D_API_HOST);
        if (g_rest_dns_h < 0) { g_rest_phase = 5; kt_snprintf(g_status, sizeof g_status, "DNS failed"); }
    }
}

/* Build and send the HTTP request for the current REST call */
static void rest_send_request(void) {
    static char req[2048];
    int pos = 0;
    if (g_rest_is_post) {
        pos += kt_snprintf(req + pos, sizeof req - pos,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s%s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "User-Agent: Ktuyome/" KT_VERSION "\r\n"
            "Connection: close\r\n"
            "\r\n",
            g_rest_path, D_API_HOST,
            g_is_bot ? "Bot " : "", g_token,
            g_rest_body_len);
    } else {
        pos += kt_snprintf(req + pos, sizeof req - pos,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s%s\r\n"
            "User-Agent: Ktuyome/" KT_VERSION "\r\n"
            "Connection: close\r\n"
            "\r\n",
            g_rest_path, D_API_HOST,
            g_is_bot ? "Bot " : "", g_token);
    }
    int rlen = pos;
    while (g_rest_req_off < rlen) {
        int n = br_ssl_engine_sendapp_buf(&g_rest_ssl.eng,
            (const u8 *)req + g_rest_req_off,
            (size_t)(rlen - g_rest_req_off));
        if (n <= 0) break;
        g_rest_req_off += n;
    }
    br_ssl_engine_run(&g_rest_ssl.eng);
}

/* ---- minimal HTTP/1.1 response handling for the REST client ---- */
static int rest_hdr_end(void) {            /* index of first body byte, or -1 */
    for (int i = 0; i + 3 < g_rest_len; i++)
        if (g_rest_buf[i] == '\r' && g_rest_buf[i+1] == '\n' &&
            g_rest_buf[i+2] == '\r' && g_rest_buf[i+3] == '\n') return i + 4;
    return -1;
}
static char rest_lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
/* Find a header (name given lowercase, including the colon); returns its value or NULL */
static const char *rest_hdr_find(int hend, const char *name) {
    int nl = (int)strlen(name);
    for (int i = 0; i + nl < hend; i++) {
        if (i != 0 && g_rest_buf[i-1] != '\n') continue;
        int k = 0;
        while (k < nl && rest_lc(g_rest_buf[i+k]) == name[k]) k++;
        if (k == nl) {
            const char *v = g_rest_buf + i + nl;
            while (*v == ' ' || *v == '\t') v++;
            return v;
        }
    }
    return NULL;
}
static int rest_is_chunked(int hend) {
    const char *v = rest_hdr_find(hend, "transfer-encoding:");
    if (!v) return 0;
    const char *w = "chunked";
    for (int i = 0; w[i]; i++) if (rest_lc(v[i]) != w[i]) return 0;
    return 1;
}
static int rest_content_length(int hend) {  /* -1 if absent */
    const char *v = rest_hdr_find(hend, "content-length:");
    if (!v || *v < '0' || *v > '9') return -1;
    int n = 0;
    while (*v >= '0' && *v <= '9' && n < 100000000) n = n * 10 + (*v++ - '0');
    return n;
}
static int rest_http_status(void) {         /* 0 if no status line yet */
    if (g_rest_len < 12 || g_rest_buf[0] != 'H' || g_rest_buf[1] != 'T') return 0;
    int i = 0;
    while (i < g_rest_len && g_rest_buf[i] != ' ') i++;
    i++;
    int n = 0, d = 0;
    while (i < g_rest_len && g_rest_buf[i] >= '0' && g_rest_buf[i] <= '9' && d < 3) { n = n * 10 + (g_rest_buf[i] - '0'); i++; d++; }
    return n;
}
/* Walk a chunked body starting at `start`.  With `compact`, the decoded data is moved
 * down to `start`.  Returns the decoded length once the terminating 0-chunk is present,
 * or -1 if the body is still incomplete. */
static int rest_walk_chunks(int start, int compact) {
    int pos = start, out = start;
    for (;;) {
        int size = 0, nd = 0;
        while (pos < g_rest_len) {
            char c = g_rest_buf[pos]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (size > D_REST_BUF) return -1;
            size = size * 16 + d; nd++; pos++;
        }
        if (nd == 0) return -1;
        while (pos < g_rest_len && g_rest_buf[pos] != '\n') pos++;   /* skip chunk extensions */
        if (pos >= g_rest_len) return -1;
        pos++;                                                        /* past the LF */
        if (size == 0) {                      /* last chunk: optional trailers, then a blank line */
            for (;;) {
                if (pos + 1 >= g_rest_len) return -1;
                if (g_rest_buf[pos] == '\r' && g_rest_buf[pos+1] == '\n') return out - start;
                while (pos < g_rest_len && g_rest_buf[pos] != '\n') pos++;
                if (pos >= g_rest_len) return -1;
                pos++;
            }
        }
        if (pos + size + 2 > g_rest_len) return -1;
        if (compact && out != pos) memmove(g_rest_buf + out, g_rest_buf + pos, (u32)size);
        out += size; pos += size + 2;
    }
}
/* Has the whole response arrived (without waiting for the server to close)? */
static int rest_response_complete(void) {
    int he = rest_hdr_end();
    if (he < 0) return 0;
    if (rest_is_chunked(he)) return rest_walk_chunks(he, 0) >= 0;
    int cl = rest_content_length(he);
    return cl >= 0 && g_rest_len - he >= cl;
}
/* Strip chunked framing in place so the body is plain JSON.  Call exactly once. */
static void rest_finalize(void) {
    int he = rest_hdr_end();
    if (he < 0) return;
    if (rest_is_chunked(he)) {
        int n = rest_walk_chunks(he, 1);
        if (n >= 0) { g_rest_len = he + n; g_rest_buf[g_rest_len] = 0; }
    }
}

/* Step the REST state machine.  Returns 1 when response is complete. */
static int rest_step(void) {
    if (g_rest_phase >= 5) return 1;
    if (ticks() - g_rest_t0 > 15000) { g_rest_phase = 5; kt_snprintf(g_status, sizeof g_status, "REST timeout"); return 1; }

    /* Phase 0: DNS */
    if (g_rest_phase == 0) {
        int r = net_dns_poll(g_rest_dns_h, &g_api_ip);
        if (r == 0) return 0;
        net_dns_free(g_rest_dns_h); g_rest_dns_h = -1;
        if (r != 1) { g_rest_phase = 5; kt_snprintf(g_status, sizeof g_status, "DNS: host not found"); return 1; }
        g_rest_phase = 1;
    }

    /* Phase 1: TCP connect */
    if (g_rest_phase == 1) {
        g_rest_tcp = tcp_open(g_api_ip, D_API_PORT);
        if (g_rest_tcp < 0) { g_rest_phase = 5; kt_snprintf(g_status, sizeof g_status, "No TCP socket"); return 1; }
        g_rest_phase = 2;
    }

    /* Phase 2: wait for TCP open, then start TLS */
    if (g_rest_phase == 2) {
        int s = tcp_status(g_rest_tcp);
        if (s == TCP_CONNECTING) return 0;
        if (s != TCP_OPEN) { g_rest_phase = 5; tcp_free(g_rest_tcp); g_rest_tcp = -1; kt_snprintf(g_status, sizeof g_status, "REST connect failed"); return 1; }
        br_ssl_client_init_full(&g_rest_ssl, NULL, 0);
        br_ssl_client_reset(&g_rest_ssl, D_API_HOST, 0);
        br_ssl_engine_set_io(&g_rest_ssl.eng, rest_tls_recv, rest_tls_send, NULL);
        g_rest_tls_ready = 0;
        g_rest_phase = 3;
    }

    /* Phase 3: TLS handshake */
    if (g_rest_phase == 3) {
        br_ssl_engine_run(&g_rest_ssl.eng);
        int st = br_ssl_engine_current_state(&g_rest_ssl.eng);
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&g_rest_ssl.eng);
            char msg[128];
            kt_snprintf(msg, sizeof msg, "TLS failed (err=%d, hs=%d, alert=%d)", err, g_rest_ssl.eng.hs, br_last_alert);
            g_rest_phase = 5; tcp_free(g_rest_tcp); g_rest_tcp = -1;
            kstrcpy(g_status, msg, sizeof g_status);
            return 1;
        }
        if (g_rest_ssl.eng.hs == TLS_HS_DONE) {
            g_rest_tls_ready = 1;
            g_rest_phase = 4;
            rest_send_request();
        }
        return 0;
    }

    /* Phase 4: receive response */
    if (g_rest_phase == 4) {
        br_ssl_engine_run(&g_rest_ssl.eng);
        while (g_rest_len < D_REST_BUF - 1) {
            u8 tmp[1200];
            int n = br_ssl_engine_recvapp_buf(&g_rest_ssl.eng, tmp, sizeof tmp);
            if (n <= 0) break;
            int take = imin(n, D_REST_BUF - 1 - g_rest_len);
            memcpy(g_rest_buf + g_rest_len, tmp, (u32)take);
            g_rest_len += take;
        }
        /* check if done: TLS closed or Connection: close */
        int st = br_ssl_engine_current_state(&g_rest_ssl.eng);
        /* done when: TLS closed, the TCP peer hung up with nothing left to read,
         * the HTTP body is complete (Content-Length / final chunk), or buffer full */
        int peer_gone = tcp_eof(g_rest_tcp) && !(st & BR_SSL_RECVAPP);
        if ((st & BR_SSL_CLOSED) || peer_gone || rest_response_complete() ||
            g_rest_len >= D_REST_BUF - 1) {
            g_rest_buf[g_rest_len] = 0;
            g_rest_phase = 5;
            tcp_free(g_rest_tcp); g_rest_tcp = -1;
            return 1;
        }
        return 0;
    }

    return 1;
}

/* Find the body in the REST response (skip HTTP headers) */
static const char *rest_body_ptr(int *out_len) {
    for (int i = 0; i + 3 < g_rest_len; i++) {
        if (g_rest_buf[i] == '\r' && g_rest_buf[i+1] == '\n' &&
            g_rest_buf[i+2] == '\r' && g_rest_buf[i+3] == '\n') {
            *out_len = g_rest_len - (i + 4);
            return g_rest_buf + i + 4;
        }
    }
    *out_len = 0;
    return "";
}

/* ================================================================== */
/*  Discord Gateway connection                                          */
/* ================================================================== */

static void d_set_status(const char *s) {
    kstrcpy(g_status, s, sizeof g_status);
    ui_wake();
}

static void d_disconnect(void) {
    if (g_gate_tcp >= 0) { tcp_free(g_gate_tcp); g_gate_tcp = -1; }
    g_gate_tls_ready = 0;
    g_conn = DC_OFFLINE;
    g_recv_len = g_recv_pos = 0;
    g_send_len = 0;
    g_last_seq = 0;
    d_set_status("Disconnected");
}

/* Start connection sequence: resolve discord.com, fetch gateway URL, then connect */
static void d_connect(void) {
    if (g_conn != DC_OFFLINE) return;
    g_conn_t0 = ticks();
    g_recv_len = g_recv_pos = 0;
    g_send_len = 0;
    g_last_seq = 0;
    g_got_ack = 1;
    g_gate_host[0] = 0;

    /* Phase 1: resolve discord.com for REST API gateway lookup */
    g_dns_h = net_dns_start(D_API_HOST);
    if (g_dns_h < 0) { d_set_status("DNS: resolver busy"); g_conn = DC_ERROR; return; }
    g_conn = DC_RESOLVING_API;
    d_set_status("Resolving discord.com...");
    ui_wake();
}

/* Send IDENTIFY payload after HELLO is received */
static void d_send_identify(void) {
    char payload[1024];
    int pos = 0;
    pos += kt_snprintf(payload + pos, sizeof payload - pos,
        "{\"op\":2,\"d\":{\"token\":\"%s\",\"intents\":%d,\"properties\":{\"os\":\"ktuyome\",\"browser\":\"ktuyome\",\"device\":\"ktuyome\"}}}",
        g_token, D_INTENTS);
    ws_write_text_frame(payload, pos);
    gate_flush_send();
    g_conn = DC_IDENTIFYING;
    d_set_status("Identifying...");
}

/* Send heartbeat */
static void d_send_heartbeat(void) {
    char payload[64];
    int pos = 0;
    if (g_last_seq >= 0)
        pos += kt_snprintf(payload + pos, sizeof payload, "{\"op\":1,\"d\":%d}", g_last_seq);
    else
        pos += kt_snprintf(payload + pos, sizeof payload, "{\"op\":1,\"d\":null}");
    ws_write_text_frame(payload, pos);
    gate_flush_send();
    g_got_ack = 0;
}

/* Process a Gateway JSON event (dispatch or control) */
static void d_process_event(const char *json_str, int json_len, u8 ws_type) {
    if (ws_type == WS_CLOSE) {
        d_set_status("Gateway closed connection");
        d_disconnect();
        return;
    }
    if (ws_type == WS_PING) {
        /* respond with pong (echo back) */
        return;
    }
    if (ws_type != WS_TEXT) return;

    JVal *root = json_parse(json_str, json_len);
    if (!root || root->type != J_OBJ) { json_free(root); return; }

    int op = (int)j_num(j_get(root, "op"));
    const JVal *d = j_get(root, "d");
    const char *t = j_str(j_get(root, "t"));  /* event type for op 0 */
    int seq = (int)j_num(j_get(root, "s"));
    if (seq > 0) g_last_seq = seq;

    switch (op) {
    case OP_HELLO: {
        g_heartbeat_interval = (int)j_num(j_get(d, "heartbeat_interval"));
        if (g_heartbeat_interval < 1000) g_heartbeat_interval = 45000;
        g_next_heartbeat = ticks() + (u32)g_heartbeat_interval;
        d_send_identify();
        break;
    }
    case OP_HEARTBEAT_ACK:
        g_got_ack = 1;
        break;
    case OP_HEARTBEAT:
        /* server requests immediate heartbeat */
        d_send_heartbeat();
        break;
    case OP_RECONNECT:
        d_set_status("Server requested reconnect");
        d_disconnect();
        d_connect();
        break;
    case OP_INVALID_SESS:
        d_set_status("Invalid session, re-identifying...");
        d_disconnect();
        d_connect();
        break;
    case OP_DISPATCH:
        if (!strcmp(t, "READY")) {
            /* extract user info */
            const JVal *user = j_get(d, "user");
            if (user) {
                kstrcpy(g_username, j_str(j_get(user, "username")), sizeof g_username);
                kstrcpy(g_userid, j_str(j_get(user, "id")), sizeof g_userid);
            }
            g_conn = DC_READY;
            d_set_status("Connected");
            /* fetch guilds via REST */
            rest_start_get(D_API_BASE "/users/@me/guilds");
            g_rest_state = REST_GET_GUILDS;
        }
        else if (!strcmp(t, "GUILD_CREATE")) {
            /* add guild to list */
            if (g_nguilds < D_MAX_GUILDS) {
                DGuild *g = &g_guilds[g_nguilds];
                kstrcpy(g->id, j_str(j_get(d, "id")), sizeof g->id);
                kstrcpy(g->name, j_str(j_get(d, "name")), sizeof g->name);
                g->id_num = snowflake(g->id);
                g_nguilds++;
            }
            /* extract channels from the guild */
            const JVal *channels = j_get(d, "channels");
            int nch = j_arr_len(channels);
            for (int i = 0; i < nch && g_nchannels < D_MAX_CHANNELS; i++) {
                const JVal *ch = j_arr(channels, i);
                int ctype = (int)j_num(j_get(ch, "type"));
                if (ctype == 0 || ctype == 5 || ctype == 15) {  /* text-like */
                    DChannel *c = &g_channels[g_nchannels++];
                    kstrcpy(c->id, j_str(j_get(ch, "id")), sizeof c->id);
                    kstrcpy(c->guild_id, j_str(j_get(d, "id")), sizeof c->guild_id);
                    kstrcpy(c->name, j_str(j_get(ch, "name")), sizeof c->name);
                    c->type = ctype;
                }
            }
            ui_wake();
        }
        else if (!strcmp(t, "MESSAGE_CREATE")) {
            /* extract message */
            const char *channel_id = j_str(j_get(d, "channel_id"));
            const JVal *author = j_get(d, "author");
            const char *author_name = author ? j_str(j_get(author, "username")) : "?";
            const char *content = j_str(j_get(d, "content"));

            /* Only store if this is the active channel */
            if (g_sel_channel >= 0 && g_sel_channel < g_nchannels &&
                !strcmp(g_channels[g_sel_channel].id, channel_id)) {
                DMsg *m = &g_msgs[g_msg_head];
                kstrcpy(m->id, j_str(j_get(d, "id")), sizeof m->id);
                kstrcpy(m->channel_id, channel_id, sizeof m->channel_id);
                kstrcpy(m->author, author_name, sizeof m->author);
                kstrcpy(m->content, content, sizeof m->content);
                m->ts = ticks();
                g_msg_head = (g_msg_head + 1) % D_MAX_MSGS;
                if (g_msg_count < D_MAX_MSGS) g_msg_count++;
                g_channel_scroll = 0;
                ui_wake();
            }
            /* update status bar with notification */
            if (g_sel_channel < 0 || strcmp(g_channels[g_sel_channel].id, channel_id)) {
                char notif[128];
                kt_snprintf(notif, sizeof notif, "New message in #%s from %s",
                            "?", author_name);
                d_set_status(notif);
            }
        }
        break;
    default:
        break;
    }

    json_free(root);
}

/* Step the Gateway connection state machine */
static void d_gate_step(void) {
    if (g_conn <= DC_OFFLINE || g_conn >= DC_ERROR) return;

    /* Timeout guard */
    if (ticks() - g_conn_t0 > 30000 && g_conn < DC_READY) {
        d_set_status("Connection timeout");
        d_disconnect();
        return;
    }

    /* --- DC_RESOLVING_API (resolve discord.com for REST gateway lookup) --- */
    if (g_conn == DC_RESOLVING_API) {
        int r = net_dns_poll(g_dns_h, &g_api_ip);
        if (r == 0) return;
        net_dns_free(g_dns_h); g_dns_h = -1;
        if (r != 1) {
            char msg[128];
            const char *err = r == -1 ? "DNS timed out" :
                              r == -2 ? "DNS: discord.com not found" :
                              r == -3 ? "DNS: no server (setdns 10.0.2.3)" : "DNS: unknown error";
            kt_snprintf(msg, sizeof msg, "%s (code %d)", err, r);
            d_set_status(msg);
            g_conn = DC_ERROR;
            return;
        }
        /* Start REST call to GET /api/v10/gateway */
        rest_start_get(D_API_BASE "/gateway");
        g_rest_state = REST_GET_GATEWAY;
        g_conn = DC_FETCH_GATEWAY_URL;
        d_set_status("Fetching gateway URL...");
        return;
    }

    /* --- DC_FETCH_GATEWAY_URL (wait for REST call to complete) --- */
    if (g_conn == DC_FETCH_GATEWAY_URL) {
        /* d_rest_done() will transition to DC_RESOLVING_GATE when done */
        return;
    }

    /* --- DC_RESOLVING_GATE --- */
    if (g_conn == DC_RESOLVING_GATE) {
        int r = net_dns_poll(g_dns_h, &g_gate_ip);
        if (r == 0) return;
        net_dns_free(g_dns_h); g_dns_h = -1;
        if (r != 1) {
            char msg[128];
            const char *err = r == -1 ? "DNS timed out" :
                              r == -2 ? "DNS: host not found" :
                              r == -3 ? "DNS: no server (setdns 10.0.2.3)" : "DNS: unknown error";
            kt_snprintf(msg, sizeof msg, "%s (code %d)", err, r);
            d_set_status(msg);
            g_conn = DC_ERROR;
            return;
        }
        g_conn = DC_CONNECTING_GATE;
        d_set_status("Connecting to gateway...");
    }

    /* --- DC_CONNECTING_GATE --- */
    if (g_conn == DC_CONNECTING_GATE) {
        if (g_gate_tcp < 0) {
            g_gate_tcp = tcp_open(g_gate_ip, D_GATE_PORT);
            if (g_gate_tcp < 0) { d_set_status("No TCP socket"); g_conn = DC_ERROR; return; }
        }
        int s = tcp_status(g_gate_tcp);
        if (s == TCP_CONNECTING) return;
        if (s != TCP_OPEN) { d_set_status("TCP connect failed"); d_disconnect(); return; }
        /* Start TLS */
        br_ssl_client_init_full(&g_gate_ssl, NULL, 0);
        br_ssl_client_reset(&g_gate_ssl, g_gate_host[0] ? g_gate_host : D_GATE_DEFAULT, 0);
        br_ssl_engine_set_io(&g_gate_ssl.eng, gate_tls_recv, gate_tls_send, NULL);
        g_gate_tls_ready = 0;
        g_conn = DC_TLS_HANDSHAKE;
        d_set_status("TLS handshake...");
    }

    /* --- DC_TLS_HANDSHAKE --- */
    if (g_conn == DC_TLS_HANDSHAKE) {
        br_ssl_engine_run(&g_gate_ssl.eng);
        int st = br_ssl_engine_current_state(&g_gate_ssl.eng);
        if (st & BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&g_gate_ssl.eng);
            char msg[128];
extern int br_last_alert;
            kt_snprintf(msg, sizeof msg, "TLS failed (err=%d, hs=%d, alert=%d)", err, g_gate_ssl.eng.hs, br_last_alert);
            d_set_status(msg);
            d_disconnect();
            return;
        }
        if (g_gate_ssl.eng.hs == TLS_HS_DONE) {
            g_gate_tls_ready = 1;
            /* Send WebSocket upgrade request */
            u8 ws_key[16];
            br_prng_bytes(ws_key, 16);
            char key_b64[32];
            b64_encode(ws_key, 16, key_b64);
            char req[512];
            int rlen = kt_snprintf(req, sizeof req,
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "\r\n",
                D_GATE_PATH, g_gate_host[0] ? g_gate_host : D_GATE_DEFAULT, key_b64);
            /* Send through TLS */
            int off = 0;
            while (off < rlen) {
                int n = br_ssl_engine_sendapp_buf(&g_gate_ssl.eng,
                    (const u8 *)req + off, (size_t)(rlen - off));
                if (n <= 0) break;
                off += n;
            }
            br_ssl_engine_run(&g_gate_ssl.eng);
            g_recv_len = 0; g_recv_pos = 0;
            g_conn = DC_WS_HANDSHAKE;
            d_set_status("WebSocket handshake...");
        }
        return;
    }

    /* --- DC_WS_HANDSHAKE --- */
    if (g_conn == DC_WS_HANDSHAKE) {
        gate_pump_recv();
        /* Look for "101" in the response and \r\n\r\n */
        if (g_recv_len < 4) return;
        if (!memcmp(g_recv_buf, "HTTP/1.1 101", 12) || !memcmp(g_recv_buf, "HTTP/1.0 101", 12)) {
            /* Find end of HTTP headers */
            for (int i = 0; i + 3 < g_recv_len; i++) {
                if (g_recv_buf[i] == '\r' && g_recv_buf[i+1] == '\n' &&
                    g_recv_buf[i+2] == '\r' && g_recv_buf[i+3] == '\n') {
                    /* Remaining data after headers is WebSocket frames */
                    g_recv_pos = i + 4;
                    g_recv_len = g_recv_len;  /* keep total, pos marks start of WS data */
                    /* Move WS data to front */
                    int ws_data_len = g_recv_len - g_recv_pos;
                    if (ws_data_len > 0) memmove(g_recv_buf, g_recv_buf + g_recv_pos, (u32)ws_data_len);
                    g_recv_len = ws_data_len;
                    g_recv_pos = 0;
                    g_conn = DC_GATE_HELLO;
                    d_set_status("Waiting for HELLO...");
                    return;
                }
            }
            /* Headers not fully received yet */
            return;
        }
        /* Not a 101 response */
        d_set_status("WebSocket upgrade rejected");
        d_disconnect();
        return;
    }

    /* --- DC_GATE_HELLO / DC_IDENTIFYING / DC_READY --- */
    if (g_conn >= DC_GATE_HELLO && g_conn <= DC_READY) {
        /* Pump receive */
        gate_pump_recv();

        /* Process complete WebSocket frames */
        while (1) {
            u8 ws_type;
            static char frame_payload[32768];
            int plen = ws_read_frame(&ws_type, frame_payload, sizeof frame_payload - 1);
            if (plen < 0) break;  /* incomplete */
            if (plen > 0 && ws_type == WS_TEXT) {
                frame_payload[plen < (int)sizeof frame_payload - 1 ? plen : (int)sizeof frame_payload - 1] = 0;
                d_process_event(frame_payload, plen, ws_type);
            } else if (ws_type == WS_CLOSE) {
                d_set_status("Gateway closed");
                d_disconnect();
                return;
            }
        }
        ws_compact_recv();

        /* Heartbeat timer */
        if (g_conn >= DC_IDENTIFYING && g_conn <= DC_READY) {
            if ((s32)(ticks() - g_next_heartbeat) >= 0) {
                if (!g_got_ack && g_conn == DC_READY) {
                    d_set_status("Heartbeat ACK timeout, reconnecting...");
                    d_disconnect();
                    d_connect();
                    return;
                }
                d_send_heartbeat();
                g_next_heartbeat = ticks() + (u32)g_heartbeat_interval;
            }
        }

        /* Flush any pending send */
        gate_flush_send();
    }
}

/* ================================================================== */
/*  REST API calls (completion handlers)                               */
/* ================================================================== */

/* Called when rest_step() returns 1; dispatch based on g_rest_state */
/* Start resolving the gateway host (D_GATE_DEFAULT if the REST lookup gave none) */
static void d_gate_fallback(const char *why) {
    if (!g_gate_host[0]) kstrcpy(g_gate_host, D_GATE_DEFAULT, sizeof g_gate_host);
    g_rest_state = REST_NONE;
    g_conn_t0 = ticks();                 /* fresh 30s budget for the real connection */
    g_dns_h = net_dns_start(g_gate_host);
    if (g_dns_h < 0) { d_set_status("DNS: resolver busy"); g_conn = DC_ERROR; return; }
    g_conn = DC_RESOLVING_GATE;
    char msg[200];
    kt_snprintf(msg, sizeof msg, "%s - using %s", why, g_gate_host);
    d_set_status(msg);
}

static void d_rest_done(void) {
    rest_finalize();
    int http = rest_http_status();
    int body_len;
    const char *body = rest_body_ptr(&body_len);

    JVal *root = (body_len > 0) ? json_parse(body, body_len) : NULL;
    if (!root) {
        if (g_rest_state == REST_GET_GATEWAY) {
            /* Gateway lookup failed: say why, then carry on with the default gateway
             * instead of sitting here until the connection timeout fires. */
            char why[120];
            if (http > 0)
                kt_snprintf(why, sizeof why, "Gateway lookup: HTTP %d", http);
            else if (starts_with(g_status, "Fetching"))
                kt_snprintf(why, sizeof why, "Gateway lookup: no response");
            else
                kt_snprintf(why, sizeof why, "%s", g_status);
            d_gate_fallback(why);
            return;
        }
        if (http >= 400) {
            char msg[64];
            kt_snprintf(msg, sizeof msg, "Discord API error: HTTP %d", http);
            d_set_status(msg);
        }
        g_rest_state = REST_NONE;
        return;
    }

    switch (g_rest_state) {
    case REST_GET_GATEWAY: {
        /* Response: {"url": "wss://gateway.discord.gg/?v=10&encoding=json"} */
        /* Extract hostname from the wss:// URL */
        const char *url = j_str(j_get(root, "url"));
        if (url && *url) {
            /* skip "wss://" or "ws://" */
            const char *host_start = url;
            if (starts_with(url, "wss://")) host_start += 6;
            else if (starts_with(url, "ws://")) host_start += 5;
            /* copy hostname until '/', ':', or end */
            int i = 0;
            while (host_start[i] && host_start[i] != '/' && host_start[i] != ':' && i < (int)sizeof(g_gate_host) - 1)
                g_gate_host[i] = host_start[i], i++;
            g_gate_host[i] = 0;
        }
        if (!g_gate_host[0])
            kstrcpy(g_gate_host, D_GATE_DEFAULT, sizeof g_gate_host);
        json_free(root);
        g_rest_state = REST_NONE;
        /* Now resolve the gateway hostname */
        g_dns_h = net_dns_start(g_gate_host);
        if (g_dns_h < 0) { d_set_status("DNS: resolver busy"); g_conn = DC_ERROR; return; }
        g_conn = DC_RESOLVING_GATE;
        {
            char msg[160];
            kt_snprintf(msg, sizeof msg, "Gateway: %s — resolving...", g_gate_host);
            d_set_status(msg);
        }
        return;
    }
    case REST_GET_GUILDS: {
        /* Array of guild objects: {id, name, ...} */
        int n = j_arr_len(root);
        g_nguilds = 0;
        for (int i = 0; i < n && g_nguilds < D_MAX_GUILDS; i++) {
            const JVal *g = j_arr(root, i);
            kstrcpy(g_guilds[g_nguilds].id, j_str(j_get(g, "id")), D_ID_LEN);
            kstrcpy(g_guilds[g_nguilds].name, j_str(j_get(g, "name")), D_NAME_LEN);
            g_guilds[g_nguilds].id_num = snowflake(g_guilds[g_nguilds].id);
            g_nguilds++;
        }
        kt_snprintf(g_status, sizeof g_status, "%d guilds loaded", g_nguilds);
        ui_wake();
        break;
    }
    case REST_GET_CHANNELS: {
        int n = j_arr_len(root);
        g_nchannels = 0;
        for (int i = 0; i < n && g_nchannels < D_MAX_CHANNELS; i++) {
            const JVal *ch = j_arr(root, i);
            int ctype = (int)j_num(j_get(ch, "type"));
            if (ctype == 0 || ctype == 5 || ctype == 15) {
                DChannel *c = &g_channels[g_nchannels++];
                kstrcpy(c->id, j_str(j_get(ch, "id")), D_ID_LEN);
                kstrcpy(c->name, j_str(j_get(ch, "name")), D_NAME_LEN);
                c->type = ctype;
                c->guild_id[0] = 0;
                if (g_sel_guild >= 0 && g_sel_guild < g_nguilds)
                    kstrcpy(c->guild_id, g_guilds[g_sel_guild].id, D_ID_LEN);
            }
        }
        kt_snprintf(g_status, sizeof g_status, "%d channels", g_nchannels);
        ui_wake();
        break;
    }
    case REST_GET_MESSAGES: {
        /* Array of message objects, newest first */
        int n = j_arr_len(root);
        g_msg_head = 0; g_msg_count = 0;
        for (int i = n - 1; i >= 0 && g_msg_count < D_MAX_MSGS; i--) {
            const JVal *msg = j_arr(root, i);
            DMsg *m = &g_msgs[g_msg_head];
            kstrcpy(m->id, j_str(j_get(msg, "id")), D_ID_LEN);
            kstrcpy(m->channel_id, j_str(j_get(msg, "channel_id")), D_ID_LEN);
            const JVal *author = j_get(msg, "author");
            kstrcpy(m->author, author ? j_str(j_get(author, "username")) : "?", D_NAME_LEN);
            kstrcpy(m->content, j_str(j_get(msg, "content")), D_MSG_LEN);
            m->ts = ticks();
            g_msg_head = (g_msg_head + 1) % D_MAX_MSGS;
            g_msg_count++;
        }
        g_channel_scroll = 0;
        kt_snprintf(g_status, sizeof g_status, "%d messages loaded", g_msg_count);
        ui_wake();
        break;
    }
    case REST_SEND_MESSAGE: {
        kt_snprintf(g_status, sizeof g_status, "Message sent");
        ui_wake();
        break;
    }
    case REST_GET_USER: {
        kstrcpy(g_username, j_str(j_get(root, "username")), sizeof g_username);
        kstrcpy(g_userid, j_str(j_get(root, "id")), sizeof g_userid);
        kt_snprintf(g_status, sizeof g_status, "Logged in as %s", g_username);
        ui_wake();
        break;
    }
    default:
        break;
    }

    json_free(root);
    g_rest_state = REST_NONE;
}

/* ================================================================== */
/*  High-level API actions                                             */
/* ================================================================== */

/* Fetch channels for the selected guild */
static void d_fetch_channels(void) {
    if (g_sel_guild < 0 || g_sel_guild >= g_nguilds) return;
    char path[256];
    kt_snprintf(path, sizeof path, D_API_BASE "/guilds/%s/channels",
                g_guilds[g_sel_guild].id);
    rest_start_get(path);
    g_rest_state = REST_GET_CHANNELS;
    d_set_status("Fetching channels...");
}

/* Fetch recent messages for the selected channel */
static void d_fetch_messages(void) {
    if (g_sel_channel < 0 || g_sel_channel >= g_nchannels) return;
    /* Clear message buffer */
    g_msg_head = 0; g_msg_count = 0;
    char path[256];
    kt_snprintf(path, sizeof path, D_API_BASE "/channels/%s/messages?limit=50",
                g_channels[g_sel_channel].id);
    rest_start_get(path);
    g_rest_state = REST_GET_MESSAGES;
    d_set_status("Fetching messages...");
}

/* Send a message to the selected channel */
static void d_send_message(const char *text) {
    if (g_sel_channel < 0 || g_sel_channel >= g_nchannels) return;
    if (!text[0]) return;
    char path[256];
    kt_snprintf(path, sizeof path, D_API_BASE "/channels/%s/messages",
                g_channels[g_sel_channel].id);
    /* Build JSON body: {"content":"..."} — escape the text */
    static char body[D_MSG_LEN + 64];
    int bp = 0;
    bp += kt_snprintf(body + bp, sizeof body - bp, "{\"content\":\"");
    for (const char *c = text; *c && bp < (int)sizeof body - 8; c++) {
        if (*c == '"')      { body[bp++]='\\'; body[bp++]='"'; }
        else if (*c == '\\'){ body[bp++]='\\'; body[bp++]='\\'; }
        else if (*c == '\n'){ body[bp++]='\\'; body[bp++]='n'; }
        else if (*c == '\r'){ body[bp++]='\\'; body[bp++]='r'; }
        else if (*c == '\t'){ body[bp++]='\\'; body[bp++]='t'; }
        else if ((u8)*c < 32) { body[bp++]=' '; }
        else body[bp++] = *c;
    }
    bp += kt_snprintf(body + bp, sizeof body - bp, "\"}");
    rest_start_post(path, body);
    g_rest_state = REST_SEND_MESSAGE;
    d_set_status("Sending message...");
}

/* ================================================================== */
/*  Token management                                                    */
/* ================================================================== */
#define D_TOKEN_PATH "/system/discord_token"

static void d_save_token(void) {
    if (g_token_len > 0) {
        fs_write_text(D_TOKEN_PATH, g_token, g_token_len);
        fs_save();
    }
}

static void d_load_token(void) {
    char buf[D_TOKEN_MAX + 4];
    int n = fs_read_text(D_TOKEN_PATH, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        /* Format: "BOT\n<token>" or "<token>" — first byte is 'B' for bot, 'U' for user */
        if (buf[0] == 'B' && buf[1] == '\n') {
            g_is_bot = 1;
            kstrcpy(g_token, buf + 2, sizeof g_token);
        } else if (buf[0] == 'U' && buf[1] == '\n') {
            g_is_bot = 0;
            kstrcpy(g_token, buf + 2, sizeof g_token);
        } else {
            /* Old format: raw token, assume user */
            g_is_bot = 0;
            kstrcpy(g_token, buf, sizeof g_token);
        }
        g_token_len = (int)strlen(g_token);
    }
}

static void d_clear_token(void) {
    g_token[0] = 0; g_token_len = 0;
    fs_write_text(D_TOKEN_PATH, "", 0);
    fs_save();
}

static void d_login(const char *token) {
    int tlen = (int)strlen(token);
    /* Detect bot token: user can prefix with "Bot " */
    if (tlen > 4 && !memcmp(token, "Bot ", 4)) {
        g_is_bot = 1;
        kstrcpy(g_token, token + 4, sizeof g_token);
    } else {
        g_is_bot = 0;
        kstrcpy(g_token, token, sizeof g_token);
    }
    g_token_len = (int)strlen(g_token);

    /* Save token with format prefix */
    char save_buf[D_TOKEN_MAX + 4];
    kt_snprintf(save_buf, sizeof save_buf, "%c\n%s", g_is_bot ? 'B' : 'U', g_token);
    fs_write_text(D_TOKEN_PATH, save_buf, (int)strlen(save_buf));
    fs_save();

    /* Clear data and connect */
    g_nguilds = 0; g_nchannels = 0; g_msg_count = 0;
    g_sel_guild = g_sel_channel = -1;
    g_view_mode = 0;
    d_connect();
}

/* ================================================================== */
/*  App lifecycle                                                      */
/* ================================================================== */
void discord_open(void) {
    static int first = 1;
    if (first) {
        first = 0;
        g_gate_tcp = -1; g_rest_tcp = -1; g_dns_h = -1; g_rest_dns_h = -1;
        g_conn = DC_OFFLINE;
        g_focused_panel = 0;
        g_view_mode = 0;
        g_sel_guild = g_sel_channel = -1;
        g_msg_head = 0; g_msg_count = 0;
        g_input[0] = 0; g_input_len = 0;
        g_status[0] = 0;
        g_nguilds = 0; g_nchannels = 0;

        /* Try loading saved token */
        d_load_token();
        if (g_token_len > 0) {
            d_connect();
        } else {
            d_set_status("Enter your Discord token to log in");
        }
    }
    ui_wake();
}

void discord_close(void) {
    /* Keep connection alive in background */
}

/* ================================================================== */
/*  Tick — called every main-loop pass from apps_tick()                */
/* ================================================================== */
void discord_tick(void) {
    net_poll();

    /* Drive Gateway connection */
    if (g_conn > DC_OFFLINE && g_conn < DC_ERROR)
        d_gate_step();

    /* Drive REST API state machine */
    if (g_rest_state != REST_NONE && g_rest_phase < 5) {
        if (rest_step()) {
            if (g_rest_phase == 5)
                d_rest_done();
        }
    }

    ui_wake();
}

/* ================================================================== */
/*  Input handler                                                      */
/* ================================================================== */
int discord_key(const Event *e) {
    /* Login prompt active */
    if (g_login_prompt.active) {
        int r = pr_key(&g_login_prompt, e);
        if (r == 1) {
            /* Confirmed — log in */
            d_login(g_login_prompt.buf);
            ui_wake();
            return 1;
        }
        if (r == 2) { /* cancelled */ ui_wake(); return 1; }
        return 1;
    }

    /* Clipboard */
    int ck = clip_key(e);
    if (ck) {
        if (g_conn != DC_READY) {                                          /* not logged in: Ctrl+V is the way to drop a token in */
            if (ck == CK_PASTE) { pr_open(&g_login_prompt, "Discord Token", "", 0); pr_paste(&g_login_prompt); ui_wake(); }
            return ck != CK_ALL;
        }
        if (ck == CK_PASTE) {
            if (g_view_mode != 1 || g_sel_channel < 0) { ui_flash("Open a channel to paste a message"); return 1; }
            g_focused_panel = 2;
            const char *c = clip_text(); int added = 0;
            for (; *c && g_input_len < (int)sizeof(g_input) - 1; c++) {      /* the message box is one line: newlines become spaces */
                if (*c == '\n') { if (g_input_len && g_input[g_input_len - 1] != ' ') { g_input[g_input_len++] = ' '; added++; } }
                else { g_input[g_input_len++] = *c; added++; }
            }
            g_input[g_input_len] = 0;
            if (*c) ui_flash("Pasted (cut off: message is full)"); else if (added) clip_notify("Pasted", added); else ui_flash("Clipboard is empty");
            ui_wake(); return 1;
        }
        if (ck == CK_COPY || ck == CK_CUT) {
            if (g_focused_panel == 2 && g_input_len > 0) {                 /* the message being typed */
                clip_set(g_input, g_input_len); clip_notify(ck == CK_CUT ? "Cut" : "Copied", clip_len());
                if (ck == CK_CUT) { g_input[0] = 0; g_input_len = 0; }
            } else if (ck == CK_COPY && g_msg_count > 0) {                 /* otherwise the loaded channel history, oldest first */
                clip_begin();
                for (int i = g_msg_count - 1; i >= 0; i--) {
                    DMsg *m = &g_msgs[(g_msg_head - 1 - i + D_MAX_MSGS) % D_MAX_MSGS];
                    clip_add(m->author, (int)strlen(m->author)); clip_add(": ", 2);
                    clip_add(m->content, (int)strlen(m->content)); clip_add("\n", 1);
                }
                clip_notify("Copied messages,", clip_len());
            } else ui_flash("Nothing to copy");
            ui_wake(); return 1;
        }
        return 0;
    }

    /* Ctrl shortcuts */
    if (e->mods & MOD_CTRL) {
        if (e->key == 'l' || e->key == 'L') {
            /* Ctrl+L = logout */
            d_clear_token();
            d_disconnect();
            g_nguilds = 0; g_nchannels = 0; g_msg_count = 0;
            g_sel_guild = g_sel_channel = -1;
            g_view_mode = 0;
            d_set_status("Logged out. Enter token to log in again.");
            pr_open(&g_login_prompt, "Discord Token", "", 0);
            ui_wake();
            return 1;
        }
        return 0;
    }

    /* Esc — go back from channel view to guild/channel list */
    if (e->key == K_ESC) {
        if (g_view_mode == 1) {
            g_view_mode = 0;
            g_focused_panel = 0;
            ui_wake();
            return 1;
        }
        return 0;
    }

    /* Not connected — any key opens login prompt */
    if (g_conn != DC_READY) {
        if (e->type == EV_KEY && e->ch >= 32 && e->ch < 127) {
            pr_open(&g_login_prompt, "Discord Token", "", 0);
            if (g_login_prompt.len < (int)sizeof(g_login_prompt.buf) - 1) {
                g_login_prompt.buf[g_login_prompt.len++] = (char)e->ch;
                g_login_prompt.buf[g_login_prompt.len] = 0;
            }
            ui_wake();
            return 1;
        }
        return 0;
    }

    /* Tab — switch focus between panels */
    if (e->key == K_TAB) {
        g_focused_panel = (g_focused_panel + 1) % 3;
        ui_wake();
        return 1;
    }

    /* If input panel is focused, handle text input */
    if (g_focused_panel == 2 && g_view_mode == 1) {
        if (e->key == K_ENTER) {
            if (g_input_len > 0) {
                d_send_message(g_input);
                g_input[0] = 0; g_input_len = 0;
            }
            ui_wake();
            return 1;
        }
        if (e->key == K_BS) {
            if (g_input_len) g_input[--g_input_len] = 0;
            ui_wake();
            return 1;
        }
        if (e->ch >= 32 && e->ch < 127 && g_input_len < (int)sizeof(g_input) - 1) {
            g_input[g_input_len++] = (char)e->ch;
            g_input[g_input_len] = 0;
            ui_wake();
            return 1;
        }
        return 0;
    }

    /* Channel list navigation */
    if (g_focused_panel == 0) {
        if (e->key == K_UP) {
            if (g_view_mode == 0) {
                /* guild selection */
                if (g_sel_guild > 0) { g_sel_guild--; d_fetch_channels(); }
            } else {
                /* channel selection */
                if (g_sel_channel > 0) { g_sel_channel--; d_fetch_messages(); }
            }
            ui_wake();
            return 1;
        }
        if (e->key == K_DOWN) {
            if (g_view_mode == 0) {
                if (g_sel_guild < g_nguilds - 1) { g_sel_guild++; d_fetch_channels(); }
            } else {
                if (g_sel_channel < g_nchannels - 1) { g_sel_channel++; d_fetch_messages(); }
            }
            ui_wake();
            return 1;
        }
        if (e->key == K_ENTER) {
            if (g_view_mode == 0 && g_sel_guild >= 0) {
                g_view_mode = 1;
                g_focused_panel = 1;
                if (g_sel_channel < 0 && g_nchannels > 0) {
                    g_sel_channel = 0;
                    d_fetch_messages();
                }
                ui_wake();
                return 1;
            }
            if (g_view_mode == 1 && g_sel_channel >= 0) {
                d_fetch_messages();
                g_focused_panel = 1;
                ui_wake();
                return 1;
            }
        }
        if (e->key == K_PGUP) {
            g_channel_list_scroll = imax(0, g_channel_list_scroll - 8);
            ui_wake(); return 1;
        }
        if (e->key == K_PGDN) {
            g_channel_list_scroll += 8;
            ui_wake(); return 1;
        }
        return 0;
    }

    /* Message panel navigation */
    if (g_focused_panel == 1 && g_view_mode == 1) {
        if (e->key == K_UP)   { g_channel_scroll = imax(0, g_channel_scroll - 1); ui_wake(); return 1; }
        if (e->key == K_DOWN) { g_channel_scroll++; ui_wake(); return 1; }
        if (e->key == K_PGUP) { g_channel_scroll = imax(0, g_channel_scroll - 10); ui_wake(); return 1; }
        if (e->key == K_PGDN) { g_channel_scroll += 10; ui_wake(); return 1; }
        if (e->key == K_HOME) { g_channel_scroll = 0; ui_wake(); return 1; }
        if (e->key == K_END)  { g_channel_scroll = g_msg_count; ui_wake(); return 1; }
        if (e->key == K_ENTER) { g_focused_panel = 2; ui_wake(); return 1; }
        return 0;
    }

    return 0;
}

/* ================================================================== */
/*  Draw                                                               */
/* ================================================================== */

/* Render a single message line, wrapping at `width` chars */
static int d_draw_msg_line(int x, int y, int w, const char *prefix, u32 pcol,
                           const char *content, u32 ccol, int prefix_w) {
    int cx = x;
    if (prefix && prefix[0]) {
        gfx_text(F_MONO, cx, y, prefix, pcol);
        cx += prefix_w;
    }
    int maxw = w - (cx - x) - 4;
    int clen = (int)strlen(content);
    int drawn = 0;
    while (drawn < clen && (cx - x) < maxw) {
        char c = content[drawn];
        char tmp[2] = { c, 0 };
        if (c < 32 || c > 126) c = '.';
        gfx_text(F_MONO, cx, y, tmp, ccol);
        cx += 8;  /* mono char width */
        drawn++;
    }
    return cx;
}

void discord_draw(int x, int y, int w, int h) {
    int lh = fonts[F_MONO].h + 2;

    /* ---- status bar (top) ----------------------------------------- */
    int sb_h = 28;
    u32 sc;
    const char *conn_str;
    switch (g_conn) {
    case DC_OFFLINE:    sc = WHITE_A(100); conn_str = "Offline";   break;
    case DC_ERROR:      sc = 0xFFFF8A80;  conn_str = "Error";     break;
    case DC_READY:      sc = 0xFF4DD0A1;  conn_str = "Connected";  break;
    default:            sc = 0xFFFFD27A;  conn_str = "Connecting"; break;
    }
    gfx_rrect(x, y, w, sb_h, 8, BLACK_A(120), BLACK_A(120), 0);
    gfx_rrect(x, y, w, sb_h, 8, WHITE_A(20), WHITE_A(20), 1);
    gfx_circle(x + 14, y + 14, 6, sc);
    gfx_text(F_MDB, x + 26, y + 6, conn_str, sc);
    if (g_conn == DC_READY && g_username[0]) {
        gfx_text(F_SM, x + 120, y + 9, "User:", WHITE_A(100));
        gfx_text(F_SM, x + 160, y + 9, g_username, WHITE_A(200));
    }
    if (g_status[0]) {
        int sw = gfx_text_w(F_SM, g_status, -1);
        gfx_text(F_SM, x + w - sw - 10, y + 9, g_status, WHITE_A(160));
    }

    /* ---- login prompt overlay (when not connected) ----------------- */
    if (g_conn != DC_READY || g_login_prompt.active) {
        /* Dark overlay */
        gfx_fill(x, y + sb_h, w, h - sb_h, BLACK_A(160));

        /* Login box */
        int bw = 500, bh = 200;
        int bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
        gfx_rrect(bx, by, bw, bh, 16, BLACK_A(200), BLACK_A(200), 0);
        gfx_rrect(bx, by, bw, bh, 16, ACC, ACC, 1);
        ui_icon(IC_DISCORD, bx + 20, by + 20, 40, 255);
        gfx_text(F_LG, bx + 70, by + 18, "Discord", 0xFFFFFFFF);
        gfx_text(F_SM, bx + 70, by + 52, "Log in with your token", WHITE_A(140));

        if (g_login_prompt.active) {
            pr_draw(&g_login_prompt, bx + 30, by + 80, bw - 60, 36);
            gfx_text_c(F_SM, bx + bw / 2, by + 130,
                "Enter your Discord token (bot or user).", WHITE_A(120));
            gfx_text_c(F_SM, bx + bw / 2, by + 148,
                "Prefix with 'Bot ' for bot tokens.", WHITE_A(100));
        } else {
            if (g_conn == DC_ERROR) {
                gfx_text_c(F_MD, bx + bw / 2, by + 90, "Connection failed.", 0xFFFF8A80);
                gfx_text_c(F_SM, bx + bw / 2, by + 120, g_status, WHITE_A(140));
            } else if (g_conn != DC_READY) {
                gfx_text_c(F_MD, bx + bw / 2, by + 90, g_status, 0xFFFFD27A);
            }
            if (ui_btn(bx + bw / 2 - 60, by + 140, 120, 36, "Log In", 1)) {
                pr_open(&g_login_prompt, "Discord Token", "", 0);
            }
        }
        gfx_noclip();
        return;
    }

    /* ---- main layout ---------------------------------------------- */
    int main_y = y + sb_h + 4;
    int main_h = h - sb_h - 4;
    int input_h = 40;
    int list_w = 220;

    /* ---- left panel: guild/channel list ---------------------------- */
    gfx_rrect(x + 4, main_y, list_w, main_h - input_h - 4, 10,
              BLACK_A(120), BLACK_A(120), 0);
    if (g_focused_panel == 0)
        gfx_rrect(x + 4, main_y, list_w, main_h - input_h - 4, 10, ACC, ACC, 1);
    else
        gfx_rrect(x + 4, main_y, list_w, main_h - input_h - 4, 10, WHITE_A(18), WHITE_A(18), 1);

    gfx_clip(x + 8, main_y + 4, list_w - 8, main_h - input_h - 12);
    int ty = main_y + 8;

    if (g_view_mode == 0) {
        /* Guild list */
        gfx_text(F_MDB, x + 14, ty, "Servers", WHITE_A(180));
        ty += lh + 4;
        int rows = (main_h - input_h - 20) / lh;
        int start = g_channel_list_scroll;
        for (int i = start; i < g_nguilds && (i - start) < rows - 1; i++) {
            int sel = (i == g_sel_guild);
            if (sel) gfx_fill(x + 10, ty - 1, list_w - 12, lh, ACC);
            char label[D_NAME_LEN + 4];
            kstrcpy(label, "  ", sizeof label);
            kstrcat(label, g_guilds[i].name, sizeof label);
            gfx_text(F_MONO, x + 14, ty, label, sel ? 0xFFFFFFFF : WHITE_A(180));
            ty += lh;
            if (ui_click_in(x + 10, ty - lh - 1, list_w - 12, lh)) {
                g_sel_guild = i;
                d_fetch_channels();
            }
        }
        if (g_nguilds == 0)
            gfx_text(F_SM, x + 14, ty, "No servers. Waiting for GUILD_CREATE...", WHITE_A(100));
    } else {
        /* Channel list */
        /* Back button */
        if (ui_btn(x + 10, ty, 40, 24, "<", 0)) {
            g_view_mode = 0; g_focused_panel = 0; ui_wake();
        }
        if (g_sel_guild >= 0 && g_sel_guild < g_nguilds)
            gfx_text(F_MDB, x + 56, ty + 2, g_guilds[g_sel_guild].name, WHITE_A(180));
        ty += lh + 4;
        int rows = (main_h - input_h - 28) / lh;
        int start = g_channel_list_scroll;
        for (int i = start; i < g_nchannels && (i - start) < rows - 1; i++) {
            int sel = (i == g_sel_channel);
            if (sel) gfx_fill(x + 10, ty - 1, list_w - 12, lh, ACC);
            char label[D_NAME_LEN + 4];
            kt_snprintf(label, sizeof label, "  #%s", g_channels[i].name);
            gfx_text(F_MONO, x + 14, ty, label, sel ? 0xFFFFFFFF : WHITE_A(170));
            ty += lh;
            if (ui_click_in(x + 10, ty - lh - 1, list_w - 12, lh)) {
                g_sel_channel = i;
                d_fetch_messages();
                g_focused_panel = 1;
            }
        }
        if (g_nchannels == 0)
            gfx_text(F_SM, x + 14, ty, "No channels. Fetching...", WHITE_A(100));
    }
    gfx_noclip();

    /* ---- right panel: messages ------------------------------------ */
    int msg_x = x + 4 + list_w + 4;
    int msg_w = w - list_w - 12;
    int msg_h = main_h - input_h - 4;
    gfx_rrect(msg_x, main_y, msg_w, msg_h, 10,
              BLACK_A(120), BLACK_A(120), 0);
    if (g_focused_panel == 1)
        gfx_rrect(msg_x, main_y, msg_w, msg_h, 10, ACC, ACC, 1);
    else
        gfx_rrect(msg_x, main_y, msg_w, msg_h, 10, WHITE_A(18), WHITE_A(18), 1);

    /* Channel header */
    int hdr_h = 32;
    gfx_rrect(msg_x, main_y, msg_w, hdr_h, 10, BLACK_A(160), BLACK_A(160), 0);
    if (g_sel_channel >= 0 && g_sel_channel < g_nchannels) {
        char hdr[D_NAME_LEN + 16];
        kt_snprintf(hdr, sizeof hdr, "# %s", g_channels[g_sel_channel].name);
        gfx_text(F_MDB, msg_x + 14, main_y + 8, hdr, WHITE_A(220));
    } else {
        gfx_text(F_MDB, msg_x + 14, main_y + 8, "Select a channel", WHITE_A(120));
    }

    /* Message list */
    gfx_clip(msg_x + 4, main_y + hdr_h, msg_w - 8, msg_h - hdr_h - 4);
    int msg_y = main_y + hdr_h + 4;
    int msg_rows = (msg_h - hdr_h - 4) / lh;

    if (g_msg_count > 0) {
        /* Messages are in ring buffer: g_msg_head is next write, count is total */
        int scroll = iclamp(g_channel_scroll, 0, imax(0, g_msg_count - msg_rows));
        int visible_start = g_msg_count - scroll - 1;  /* newest at bottom */
        int shown = 0;
        for (int i = visible_start; i >= 0 && shown < msg_rows; i--, shown++) {
            int idx = (g_msg_head - 1 - i + D_MAX_MSGS) % D_MAX_MSGS;
            if (idx < 0 || idx >= D_MAX_MSGS) continue;
            DMsg *m = &g_msgs[idx];
            int ly = msg_y + (msg_rows - 1 - shown) * lh;
            if (ly < main_y + hdr_h) break;

            /* Timestamp (from ticks delta) */
            u32 ago = ticks() - m->ts;
            char ts[16];
            if (ago < 60000) kt_snprintf(ts, sizeof ts, "%us", ago / 1000);
            else if (ago < 3600000) kt_snprintf(ts, sizeof ts, "%um", ago / 60000);
            else kt_snprintf(ts, sizeof ts, "%uh", ago / 3600000);

            /* Author in accent color, content in white */
            int author_w = gfx_text_w(F_MONO, m->author, -1);
            author_w = imin(author_w, 120);
            gfx_text_n(F_MONO, msg_x + 10, ly, m->author, D_NAME_LEN, col_lerp(ACC, 0xFFFFFFFF, 60));
            gfx_text(F_SM, msg_x + 10 + author_w + 8, ly + 2, ts, WHITE_A(80));
            d_draw_msg_line(msg_x + 10 + author_w + 56, ly, msg_w - 10,
                            "", 0, m->content, WHITE_A(200), 0);
        }
    } else if (g_sel_channel >= 0) {
        gfx_text(F_MONO, msg_x + 14, msg_y + 4,
                 "No messages. Press Enter to fetch.", WHITE_A(120));
    } else {
        gfx_text(F_MONO, msg_x + 14, msg_y + 4,
                 "Select a channel from the left panel.", WHITE_A(120));
    }

    /* Scrollbar */
    if (g_msg_count > msg_rows) {
        int sb_x2 = msg_x + msg_w - 8;
        int track = msg_h - hdr_h - 4;
        int thumb = imax(20, track * msg_rows / g_msg_count);
        int thumb_y = msg_y + (track - thumb) * g_channel_scroll / imax(1, g_msg_count - msg_rows);
        gfx_rrect(sb_x2, msg_y, 4, track, 2, WHITE_A(20), WHITE_A(20), 0);
        gfx_rrect(sb_x2, thumb_y, 4, thumb, 2, WHITE_A(80), WHITE_A(80), 0);
    }

    /* Mouse wheel scroll */
    if (g_wheel) {
        g_channel_scroll = iclamp(g_channel_scroll - g_wheel * 2, 0, g_msg_count);
        ui_wake();
    }

    gfx_noclip();

    /* ---- input bar (bottom) --------------------------------------- */
    int in_y = main_y + main_h - input_h;
    int in_w = w - 8;
    gfx_rrect(x + 4, in_y, in_w, input_h, 8, BLACK_A(140), BLACK_A(140), 0);
    if (g_focused_panel == 2)
        gfx_rrect(x + 4, in_y, in_w, input_h, 8, ACC, ACC, 1);
    else
        gfx_rrect(x + 4, in_y, in_w, input_h, 8, WHITE_A(18), WHITE_A(18), 1);

    gfx_text(F_MONO, x + 14, in_y + 10, ">", g_focused_panel == 2 ? ACC : WHITE_A(120));
    gfx_text(F_MONO, x + 34, in_y + 10, g_input, WHITE_A(220));

    /* Cursor */
    if (g_focused_panel == 2 && !g_blink) {
        int cx2 = x + 34 + gfx_text_w(F_MONO, g_input, -1);
        gfx_fill(cx2, in_y + 9, 2, fonts[F_MONO].h, ACC);
    }

    /* Placeholder text */
    if (g_input_len == 0 && g_focused_panel != 2) {
        gfx_text(F_SM, x + 34, in_y + 12,
                 "Press Tab to focus, then type a message and press Enter...",
                 WHITE_A(80));
    }

    /* Hint bar */
    gfx_text_r(F_SM, x + w - 10, in_y + 12,
               "Tab=switch  Esc=back  Ctrl+L=logout", WHITE_A(80));
}

/* ================================================================== */
/*  Static bytes (task manager)                                        */
/* ================================================================== */
u32 discord_static_bytes(void) {
    return (u32)(sizeof(g_recv_buf) + sizeof(g_send_buf) +
                 sizeof(g_rest_buf) + sizeof(g_guilds) +
                 sizeof(g_channels) + sizeof(g_msgs) +
                 sizeof(g_gate_ssl) + sizeof(g_rest_ssl));
}
