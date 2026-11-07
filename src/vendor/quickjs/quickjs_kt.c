/*
 * quickjs_kt.c — Minimal JS interpreter for ktuyome (Part 1/6)
 * Compiled: -m32 -ffreestanding -fno-builtin -mno-sse -O2 -std=gnu11
 */
#include "quickjs_kt.h"

/* ================================================================== */
/*  Internal data structures                                           */
/* ================================================================== */

/* String storage — interned, ref-counted */
#define JS_MAX_STRINGS   2048
#define JS_STR_MAX       4096
typedef struct JSString {
    char  *data;
    int    len;
    int    refcount;
} JSString;

/* Property (linked-list per object) */
typedef struct JSProp {
    char     *key;
    JSValue   val;
    int       flags;
    struct JSProp *next;
} JSProp;

/* Object */
#define JS_MAX_OBJECTS   8192
typedef struct JSObject {
    JSProp   *props;
    int       refcount;
    int       is_array;
    uint32_t  length;    /* for arrays */
    /* C function fields */
    JSCFunction *cfunc;
    char        *fname;
    int          flen;
} JSObject;

/* Runtime */
struct JSRuntime {
    JSString  strings[JS_MAX_STRINGS];
    int       nstrings;
    JSObject  objects[JS_MAX_OBJECTS];
    int       nobjects;
    size_t    mem_limit;
};

/* Context */
struct JSContext {
    JSRuntime    *rt;
    JSValue       global_obj;
    JSValue       exception;
    int           has_exception;
    js_console_fn console_cb;
    js_docwrite_fn docwrite_cb;
};

/* ================================================================== */
/*  String internals                                                   */
/* ================================================================== */
static JSString *js_str_ptr(JSContext *ctx, JSValue v) {
    if (JS_VALUE_GET_TAG(v) != JS_TAG_STRING) return NULL;
    uint32_t idx = (uint32_t)v;
    if (idx >= (uint32_t)ctx->rt->nstrings) return NULL;
    return &ctx->rt->strings[idx];
}

static JSValue js_new_string_internal(JSContext *ctx, const char *s, int len) {
    JSRuntime *rt = ctx->rt;
    if (rt->nstrings >= JS_MAX_STRINGS) return JS_UNDEFINED;
    char *data = (char*)kmalloc((uint32_t)(len+1));
    if (!data) return JS_UNDEFINED;
    memcpy(data, s, (size_t)len);
    data[len] = 0;
    int idx = rt->nstrings++;
    rt->strings[idx].data = data;
    rt->strings[idx].len  = len;
    rt->strings[idx].refcount = 1;
    return ((uint64_t)JS_TAG_STRING << 32) | (uint32_t)idx;
}

JSValue JS_NewString(JSContext *ctx, const char *str) {
    return js_new_string_internal(ctx, str, str ? (int)strlen(str) : 0);
}
JSValue JS_NewStringLen(JSContext *ctx, const char *str, size_t len) {
    return js_new_string_internal(ctx, str, (int)len);
}

/* ================================================================== */
/*  Object internals                                                   */
/* ================================================================== */
static JSObject *js_obj_ptr(JSContext *ctx, JSValue v) {
    if (JS_VALUE_GET_TAG(v) != JS_TAG_OBJECT) return NULL;
    uint32_t idx = (uint32_t)v;
    if (idx >= (uint32_t)ctx->rt->nobjects) return NULL;
    return &ctx->rt->objects[idx];
}

static JSValue js_new_object_internal(JSContext *ctx) {
    JSRuntime *rt = ctx->rt;
    if (rt->nobjects >= JS_MAX_OBJECTS) return JS_UNDEFINED;
    int idx = rt->nobjects++;
    rt->objects[idx].props    = NULL;
    rt->objects[idx].refcount = 1;
    rt->objects[idx].is_array = 0;
    rt->objects[idx].length   = 0;
    rt->objects[idx].cfunc    = NULL;
    rt->objects[idx].fname    = NULL;
    return ((uint64_t)JS_TAG_OBJECT << 32) | (uint32_t)idx;
}

JSValue JS_NewObject(JSContext *ctx) { return js_new_object_internal(ctx); }

JSValue JS_NewArray(JSContext *ctx) {
    JSValue v = js_new_object_internal(ctx);
    JSObject *o = js_obj_ptr(ctx, v);
    if (o) o->is_array = 1;
    return v;
}

static JSProp *js_find_prop(JSObject *obj, const char *key) {
    for (JSProp *p = obj->props; p; p = p->next)
        if (strcmp(p->key, key) == 0) return p;
    return NULL;
}

int JS_SetPropertyStr(JSContext *ctx, JSValue obj, const char *name, JSValue val) {
    JSObject *o = js_obj_ptr(ctx, obj);
    if (!o) return -1;
    JSProp *p = js_find_prop(o, name);
    if (p) { p->val = val; return 0; }
    p = (JSProp*)kmalloc(sizeof(JSProp));
    if (!p) return -1;
    p->key  = (char*)kmalloc((uint32_t)(strlen(name)+1));
    if (!p->key) { kfree(p); return -1; }
    strcpy(p->key, name);
    p->val   = val;
    p->flags = JS_PROP_C_W_E;
    p->next  = o->props;
    o->props = p;
    return 0;
}

JSValue JS_GetPropertyStr(JSContext *ctx, JSValue obj, const char *name) {
    JSObject *o = js_obj_ptr(ctx, obj);
    if (!o) return JS_UNDEFINED;
    JSProp *p = js_find_prop(o, name);
    return p ? p->val : JS_UNDEFINED;
}

int JS_SetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val) {
    char key[16]; kt_snprintf(key, sizeof key, "%u", idx);
    JSObject *o = js_obj_ptr(ctx, obj);
    if (o && idx >= o->length) o->length = idx + 1;
    return JS_SetPropertyStr(ctx, obj, key, val);
}

JSValue JS_GetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx) {
    char key[16]; kt_snprintf(key, sizeof key, "%u", idx);
    return JS_GetPropertyStr(ctx, obj, key);
}

int JS_DefinePropertyValueStr(JSContext *ctx, JSValue obj,
                               const char *name, JSValue val, int flags) {
    (void)flags;
    return JS_SetPropertyStr(ctx, obj, name, val);
}

/* ================================================================== */
/*  C function objects                                                 */
/* ================================================================== */
JSValue JS_NewCFunction(JSContext *ctx, JSCFunction *func,
                        const char *name, int length) {
    JSValue v = js_new_object_internal(ctx);
    JSObject *o = js_obj_ptr(ctx, v);
    if (!o) return JS_UNDEFINED;
    o->cfunc = func;
    o->flen  = length;
    if (name) {
        o->fname = (char*)kmalloc((uint32_t)(strlen(name)+1));
        if (o->fname) strcpy(o->fname, name);
    }
    return v;
}

/* ================================================================== */
/*  Value lifecycle                                                    */
/* ================================================================== */
void JS_FreeValue(JSContext *ctx, JSValue v) {
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_STRING) {
        JSString *s = js_str_ptr(ctx, v);
        if (s && --s->refcount <= 0) { kfree(s->data); s->data = NULL; s->len = 0; }
    } else if (tag == JS_TAG_OBJECT) {
        JSObject *o = js_obj_ptr(ctx, v);
        if (o && --o->refcount <= 0) {
            for (JSProp *p = o->props, *nx; p; p = nx) {
                nx = p->next;
                JS_FreeValue(ctx, p->val);
                kfree(p->key); kfree(p);
            }
            o->props = NULL;
            if (o->fname) { kfree(o->fname); o->fname = NULL; }
        }
    }
}

JSValue JS_DupValue(JSContext *ctx, JSValue v) {
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_STRING) {
        JSString *s = js_str_ptr(ctx, v);
        if (s) s->refcount++;
    } else if (tag == JS_TAG_OBJECT) {
        JSObject *o = js_obj_ptr(ctx, v);
        if (o) o->refcount++;
    }
    return v;
}

/* ================================================================== */
/*  Type conversion                                                    */
/* ================================================================== */
const char *JS_ToCString(JSContext *ctx, JSValue val) {
    int tag = JS_VALUE_GET_TAG(val);
    if (tag == JS_TAG_STRING) {
        JSString *s = js_str_ptr(ctx, val);
        return s ? s->data : "";
    }
    static char tmp[64];
    if (tag == JS_TAG_INT)   { kt_snprintf(tmp, sizeof tmp, "%d", JS_VALUE_GET_INT(val)); return tmp; }
    if (tag == JS_TAG_FLOAT64) { double d = JS_VALUE_GET_FLOAT64(val); kt_snprintf(tmp, sizeof tmp, "%g", d); return tmp; }
    if (tag == JS_TAG_BOOL)  return JS_VALUE_GET_BOOL(val) ? "true" : "false";
    if (tag == JS_TAG_NULL)  return "null";
    if (tag == JS_TAG_UNDEFINED) return "undefined";
    return "[object Object]";
}
void JS_FreeCString(JSContext *ctx, const char *ptr) { (void)ctx; (void)ptr; }

JSValue JS_ToString(JSContext *ctx, JSValue val) {
    const char *s = JS_ToCString(ctx, val);
    return JS_NewString(ctx, s);
}

int JS_ToInt32(JSContext *ctx, int *pres, JSValue val) {
    int tag = JS_VALUE_GET_TAG(val);
    if (tag == JS_TAG_INT)    { *pres = JS_VALUE_GET_INT(val); return 0; }
    if (tag == JS_TAG_FLOAT64){ *pres = (int)JS_VALUE_GET_FLOAT64(val); return 0; }
    if (tag == JS_TAG_BOOL)   { *pres = JS_VALUE_GET_BOOL(val); return 0; }
    if (tag == JS_TAG_STRING) {
        JSString *s = js_str_ptr(ctx, val);
        *pres = s ? katoi(s->data) : 0; return 0;
    }
    *pres = 0; return 0;
}

int JS_ToFloat64(JSContext *ctx, double *pres, JSValue val) {
    int tag = JS_VALUE_GET_TAG(val);
    if (tag == JS_TAG_INT)    { *pres = (double)JS_VALUE_GET_INT(val); return 0; }
    if (tag == JS_TAG_FLOAT64){ *pres = JS_VALUE_GET_FLOAT64(val); return 0; }
    if (tag == JS_TAG_BOOL)   { *pres = (double)JS_VALUE_GET_BOOL(val); return 0; }
    if (tag == JS_TAG_STRING) {
        JSString *s = js_str_ptr(ctx, val);
        *pres = s ? kt_strtod(s->data, NULL) : 0.0; return 0;
    }
    *pres = 0.0; return 0;
}

JSValue JS_NewInt32(JSContext *ctx, int32_t val) { (void)ctx; return JS_MKINT(val); }
JSValue JS_NewFloat64(JSContext *ctx, double val) { (void)ctx; return JS_MKFLOAT(val); }
JSValue JS_NewBool(JSContext *ctx, int val) { (void)ctx; return JS_MKBOOL(val); }

/* ================================================================== */
/*  Exception handling                                                 */
/* ================================================================== */
void JS_Throw(JSContext *ctx, JSValue val) {
    ctx->exception = val; ctx->has_exception = 1;
}
JSValue JS_GetException(JSContext *ctx) {
    JSValue ex = ctx->exception;
    ctx->exception = JS_UNDEFINED; ctx->has_exception = 0;
    return ex;
}
void JS_ResetUncatchableError(JSContext *ctx) {
    ctx->has_exception = 0; ctx->exception = JS_UNDEFINED;
}
int JS_IsError(JSContext *ctx, JSValue val) {
    (void)ctx;
    return JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT;
}
JSValue JS_NewError(JSContext *ctx) { return JS_NewObject(ctx); }

JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...) {
    char buf[256]; __builtin_va_list ap; __builtin_va_start(ap,fmt);
    kt_vsnprintf(buf,sizeof buf,fmt,ap); __builtin_va_end(ap);
    JSValue err = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,err,"message",JS_NewString(ctx,buf));
    JS_Throw(ctx,err); return JS_EXCEPTION;
}
JSValue JS_ThrowRangeError(JSContext *ctx, const char *fmt, ...) {
    char buf[256]; __builtin_va_list ap; __builtin_va_start(ap,fmt);
    kt_vsnprintf(buf,sizeof buf,fmt,ap); __builtin_va_end(ap);
    JSValue err = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,err,"message",JS_NewString(ctx,buf));
    JS_Throw(ctx,err); return JS_EXCEPTION;
}

/* ================================================================== */
/*  Global object & callbacks                                          */
/* ================================================================== */
JSValue JS_GetGlobalObject(JSContext *ctx) { return JS_DupValue(ctx, ctx->global_obj); }

int JS_SetGlobal(JSContext *ctx, const char *name, JSValue val) {
    return JS_SetPropertyStr(ctx, ctx->global_obj, name, val);
}

void JS_SetConsoleCallback(JSContext *ctx, js_console_fn fn) { ctx->console_cb = fn; }
void JS_SetDocWriteCallback(JSContext *ctx, js_docwrite_fn fn) { ctx->docwrite_cb = fn; }

/* ================================================================== */
/*  Tokenizer                                                          */
/* ================================================================== */
typedef enum {
    TOK_EOF=0, TOK_NUM, TOK_STR, TOK_IDENT, TOK_REGEX,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_EQ2, TOK_EQ3, TOK_NEQ, TOK_NEQ2,
    TOK_LT, TOK_GT, TOK_LTE, TOK_GTE,
    TOK_AND2, TOK_OR2, TOK_BANG,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE,
    TOK_LSHIFT, TOK_RSHIFT,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COMMA, TOK_DOT, TOK_COLON, TOK_QUESTION,
    TOK_PLUSEQ, TOK_MINUSEQ, TOK_STAREQ, TOK_SLASHEQ,
    TOK_PLUSPLUS, TOK_MINUSMINUS,
    TOK_ARROW,
    /* Keywords */
    TOK_VAR, TOK_LET, TOK_CONST, TOK_FUNCTION, TOK_RETURN,
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_BREAK, TOK_CONTINUE,
    TOK_NEW, TOK_DELETE, TOK_TYPEOF, TOK_VOID, TOK_IN, TOK_OF,
    TOK_TRUE, TOK_FALSE, TOK_NULL_KW, TOK_UNDEFINED_KW,
    TOK_THIS, TOK_CLASS, TOK_EXTENDS, TOK_SUPER,
    TOK_THROW, TOK_TRY, TOK_CATCH, TOK_FINALLY,
    TOK_DO, TOK_SWITCH, TOK_CASE, TOK_DEFAULT,
    TOK_INSTANCEOF,
} TokenType;

#define TOK_BUF_SIZE 256
typedef struct {
    const char *src;
    int         pos;
    int         len;
    TokenType   type;
    char        buf[TOK_BUF_SIZE];   /* for identifiers/strings */
    double      num_val;
    int         line;
} Lexer;

static void lex_init(Lexer *lx, const char *src, int len) {
    lx->src = src; lx->pos = 0; lx->len = len; lx->line = 1;
}

static int lex_peek(Lexer *lx) { return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1; }
static int lex_next(Lexer *lx) {
    if (lx->pos >= lx->len) return -1;
    int c = (unsigned char)lx->src[lx->pos++];
    if (c == '\n') lx->line++;
    return c;
}
static int lex_peek2(Lexer *lx) { return lx->pos+1<lx->len ? (unsigned char)lx->src[lx->pos+1] : -1; }

static struct { const char *kw; TokenType tok; } keywords[] = {
    {"var",TOK_VAR},{"let",TOK_LET},{"const",TOK_CONST},
    {"function",TOK_FUNCTION},{"return",TOK_RETURN},
    {"if",TOK_IF},{"else",TOK_ELSE},{"while",TOK_WHILE},
    {"for",TOK_FOR},{"break",TOK_BREAK},{"continue",TOK_CONTINUE},
    {"new",TOK_NEW},{"delete",TOK_DELETE},{"typeof",TOK_TYPEOF},
    {"void",TOK_VOID},{"in",TOK_IN},{"of",TOK_OF},
    {"true",TOK_TRUE},{"false",TOK_FALSE},{"null",TOK_NULL_KW},
    {"undefined",TOK_UNDEFINED_KW},{"this",TOK_THIS},
    {"class",TOK_CLASS},{"extends",TOK_EXTENDS},{"super",TOK_SUPER},
    {"throw",TOK_THROW},{"try",TOK_TRY},{"catch",TOK_CATCH},{"finally",TOK_FINALLY},
    {"do",TOK_DO},{"switch",TOK_SWITCH},{"case",TOK_CASE},{"default",TOK_DEFAULT},
    {"instanceof",TOK_INSTANCEOF},{NULL,TOK_EOF}
};

static TokenType classify_ident(const char *s) {
    for (int i = 0; keywords[i].kw; i++)
        if (strcmp(s, keywords[i].kw) == 0) return keywords[i].tok;
    return TOK_IDENT;
}

static void lex_advance(Lexer *lx) {
    /* skip whitespace and comments */
    while (1) {
        while (lx->pos < lx->len && isspace(lx->src[lx->pos])) {
            if (lx->src[lx->pos]=='\n') lx->line++;
            lx->pos++;
        }
        if (lx->pos+1 < lx->len && lx->src[lx->pos]=='/' && lx->src[lx->pos+1]=='/') {
            while (lx->pos<lx->len && lx->src[lx->pos]!='\n') lx->pos++;
            continue;
        }
        if (lx->pos+1 < lx->len && lx->src[lx->pos]=='/' && lx->src[lx->pos+1]=='*') {
            lx->pos+=2;
            while (lx->pos+1<lx->len && !(lx->src[lx->pos]=='*'&&lx->src[lx->pos+1]=='/')) {
                if(lx->src[lx->pos]=='\n') lx->line++; lx->pos++;
            }
            lx->pos+=2; continue;
        }
        break;
    }
    if (lx->pos >= lx->len) { lx->type = TOK_EOF; return; }
    int c = lex_peek(lx);
    /* number */
    if (isdigit(c) || (c=='.' && isdigit(lex_peek2(lx)))) {
        char *end; lx->num_val = kt_strtod(lx->src+lx->pos, &end);
        lx->pos = (int)(end - lx->src); lx->type = TOK_NUM; return;
    }
    /* identifier or keyword */
    if (isalpha(c) || c=='_' || c=='$') {
        int n=0;
        while (lx->pos<lx->len && (isalnum((unsigned char)lx->src[lx->pos])||lx->src[lx->pos]=='_'||lx->src[lx->pos]=='$')) {
            if (n<TOK_BUF_SIZE-1) lx->buf[n++]=(char)lx->src[lx->pos];
            lx->pos++;
        }
        lx->buf[n]=0; lx->type = classify_ident(lx->buf); return;
    }
    /* string */
    if (c=='"'||c=='\'') {
        int q=c; lx->pos++; int n=0;
        while (lx->pos<lx->len && lx->src[lx->pos]!=q) {
            char ch=lx->src[lx->pos++];
            if (ch=='\\' && lx->pos<lx->len) {
                ch=lx->src[lx->pos++];
                switch(ch){case 'n':ch='\n';break;case 't':ch='\t';break;
                           case 'r':ch='\r';break;default:break;}
            }
            if (n<TOK_BUF_SIZE-1) lx->buf[n++]=ch;
        }
        if (lx->pos<lx->len) lx->pos++; /* closing quote */
        lx->buf[n]=0; lx->type=TOK_STR; return;
    }
    /* template literal — treat as plain string */
    if (c=='`') {
        lx->pos++; int n=0;
        while (lx->pos<lx->len && lx->src[lx->pos]!='`') {
            char ch=lx->src[lx->pos++];
            if (n<TOK_BUF_SIZE-1) lx->buf[n++]=ch;
        }
        if (lx->pos<lx->len) lx->pos++;
        lx->buf[n]=0; lx->type=TOK_STR; return;
    }
    /* punctuation */
    lx->pos++;
    switch(c) {
    case '+': if(lex_peek(lx)=='+'){lx->pos++;lx->type=TOK_PLUSPLUS;}
              else if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_PLUSEQ;}
              else lx->type=TOK_PLUS; return;
    case '-': if(lex_peek(lx)=='-'){lx->pos++;lx->type=TOK_MINUSMINUS;}
              else if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_MINUSEQ;}
              else lx->type=TOK_MINUS; return;
    case '*': if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_STAREQ;} else lx->type=TOK_STAR; return;
    case '/': if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_SLASHEQ;} else lx->type=TOK_SLASH; return;
    case '%': lx->type=TOK_PERCENT; return;
    case '=': if(lex_peek(lx)=='='){lx->pos++;
                  if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_EQ3;}
                  else lx->type=TOK_EQ2;
              } else if(lex_peek(lx)=='>'){lx->pos++;lx->type=TOK_ARROW;}
              else lx->type=TOK_EQ; return;
    case '!': if(lex_peek(lx)=='='){lx->pos++;
                  if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_NEQ2;}
                  else lx->type=TOK_NEQ;
              } else lx->type=TOK_BANG; return;
    case '<': if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_LTE;}
              else if(lex_peek(lx)=='<'){lx->pos++;lx->type=TOK_LSHIFT;}
              else lx->type=TOK_LT; return;
    case '>': if(lex_peek(lx)=='='){lx->pos++;lx->type=TOK_GTE;}
              else if(lex_peek(lx)=='>'){lx->pos++;lx->type=TOK_RSHIFT;}
              else lx->type=TOK_GT; return;
    case '&': if(lex_peek(lx)=='&'){lx->pos++;lx->type=TOK_AND2;} else lx->type=TOK_AMP; return;
    case '|': if(lex_peek(lx)=='|'){lx->pos++;lx->type=TOK_OR2;}  else lx->type=TOK_PIPE; return;
    case '^': lx->type=TOK_CARET; return;
    case '~': lx->type=TOK_TILDE; return;
    case '(': lx->type=TOK_LPAREN; return;
    case ')': lx->type=TOK_RPAREN; return;
    case '{': lx->type=TOK_LBRACE; return;
    case '}': lx->type=TOK_RBRACE; return;
    case '[': lx->type=TOK_LBRACKET; return;
    case ']': lx->type=TOK_RBRACKET; return;
    case ';': lx->type=TOK_SEMI; return;
    case ',': lx->type=TOK_COMMA; return;
    case '.': lx->type=TOK_DOT; return;
    case ':': lx->type=TOK_COLON; return;
    case '?': lx->type=TOK_QUESTION; return;
    default:  lx->type=TOK_EOF; return;
    }
}

/* ================================================================== */
/*  Recursive-descent interpreter (tree-walking, no bytecode)         */
/* ================================================================== */

/* Maximum call depth */
#define JS_MAX_DEPTH 64

typedef struct JSEnv JSEnv;
struct JSEnv {
    char    *keys[64];
    JSValue  vals[64];
    int      count;
    JSEnv   *parent;
    JSContext *ctx;
};

/* Control flow signals */
#define FLOW_NORMAL   0
#define FLOW_RETURN   1
#define FLOW_BREAK    2
#define FLOW_CONTINUE 3
#define FLOW_THROW    4

typedef struct {
    int      flow;
    JSValue  val;
} EvalResult;

static JSEnv *env_push(JSEnv *parent, JSContext *ctx) {
    JSEnv *e = (JSEnv*)kmalloc(sizeof(JSEnv));
    if (!e) return parent;
    e->count = 0; e->parent = parent; e->ctx = ctx;
    return e;
}
static void env_pop(JSEnv *e) {
    if (!e) return;
    for (int i = 0; i < e->count; i++) {
        kfree(e->keys[i]);
        JS_FreeValue(e->ctx, e->vals[i]);
    }
    kfree(e);
}
static int env_set_local(JSEnv *e, const char *key, JSValue val) {
    for (int i = 0; i < e->count; i++)
        if (strcmp(e->keys[i], key)==0) {
            JS_FreeValue(e->ctx, e->vals[i]);
            e->vals[i] = JS_DupValue(e->ctx, val);
            return 0;
        }
    if (e->count >= 64) return -1;
    e->keys[e->count] = (char*)kmalloc((uint32_t)(strlen(key)+1));
    if (!e->keys[e->count]) return -1;
    strcpy(e->keys[e->count], key);
    e->vals[e->count] = JS_DupValue(e->ctx, val);
    e->count++;
    return 0;
}
static JSValue env_get(JSEnv *e, const char *key) {
    for (JSEnv *cur = e; cur; cur = cur->parent)
        for (int i = 0; i < cur->count; i++)
            if (strcmp(cur->keys[i], key)==0) return JS_DupValue(cur->ctx, cur->vals[i]);
    return JS_UNDEFINED;
}
static int env_assign(JSEnv *e, const char *key, JSValue val) {
    for (JSEnv *cur = e; cur; cur = cur->parent)
        for (int i = 0; i < cur->count; i++)
            if (strcmp(cur->keys[i], key)==0) {
                JS_FreeValue(cur->ctx, cur->vals[i]);
                cur->vals[i] = JS_DupValue(cur->ctx, val);
                return 0;
            }
    /* not found: create global */
    return env_set_local(e, key, val);
}

/* Function object stored as a property "__src__" (source span) + "__env__" */
/* We re-lex and evaluate function bodies; store start/end position in src */
typedef struct {
    const char *src;
    int         body_start; /* position of '{' */
    int         body_end;   /* position of closing '}' */
    char        params[8][64];
    int         nparam;
    JSEnv      *closure;
} JSFunction;

/* Store JSFunction* in an object's "__fn__" property as a pointer cast to int */

/* Forward declaration */
static EvalResult eval_block(JSContext *ctx, Lexer *lx, JSEnv *env, int depth);
static EvalResult eval_expr(JSContext *ctx, Lexer *lx, JSEnv *env, int depth);
static EvalResult eval_stmt(JSContext *ctx, Lexer *lx, JSEnv *env, int depth);

/* Evaluate a comma-separated argument list, returning array of JSValues */
static int eval_args(JSContext *ctx, Lexer *lx, JSEnv *env, int depth,
                     JSValue *args, int max_args) {
    int n = 0;
    if (lx->type == TOK_RPAREN) return 0;
    while (1) {
        if (n < max_args) {
            EvalResult r = eval_expr(ctx, lx, env, depth);
            args[n++] = r.val;
            if (r.flow) break;
        }
        if (lx->type != TOK_COMMA) break;
        lex_advance(lx);
    }
    return n;
}

/* String concatenation helper */
static JSValue js_concat_str(JSContext *ctx, JSValue a, JSValue b) {
    const char *sa = JS_ToCString(ctx, a);
    const char *sb = JS_ToCString(ctx, b);
    int la = (int)strlen(sa), lb = (int)strlen(sb);
    char *buf = (char*)kmalloc((uint32_t)(la+lb+1));
    if (!buf) return JS_UNDEFINED;
    memcpy(buf, sa, la); memcpy(buf+la, sb, lb); buf[la+lb]=0;
    JSValue r = JS_NewString(ctx, buf); kfree(buf);
    return r;
}

static int js_is_string(JSValue v) { return JS_VALUE_GET_TAG(v)==JS_TAG_STRING; }

/* Numeric coerce */
static double js_to_num(JSContext *ctx, JSValue v) {
    double d; JS_ToFloat64(ctx, &d, v); return d;
}
static int js_to_bool(JSContext *ctx, JSValue v) {
    int tag = JS_VALUE_GET_TAG(v);
    if (tag==JS_TAG_BOOL) return JS_VALUE_GET_BOOL(v);
    if (tag==JS_TAG_INT)  return JS_VALUE_GET_INT(v)!=0;
    if (tag==JS_TAG_FLOAT64) { double d=JS_VALUE_GET_FLOAT64(v); return d!=0.0&&!kt_isnan(d); }
    if (tag==JS_TAG_NULL||tag==JS_TAG_UNDEFINED) return 0;
    if (tag==JS_TAG_STRING) { JSString *s=js_str_ptr(ctx,v); return s&&s->len>0; }
    return 1; /* objects are truthy */
}

/* Primary expression: num, str, bool, null, undefined, ident, (expr), array, object */
static EvalResult eval_primary(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult r; r.flow=FLOW_NORMAL;
    if (depth > JS_MAX_DEPTH) { r.val=JS_UNDEFINED; return r; }

    if (lx->type == TOK_NUM) {
        double n = lx->num_val; lex_advance(lx);
        r.val = (n==(int)n&&n>=-1e15&&n<=1e15) ? JS_MKINT((int)n) : JS_MKFLOAT(n);
        return r;
    }
    if (lx->type == TOK_STR) {
        char buf[TOK_BUF_SIZE]; kstrcpy(buf, lx->buf, TOK_BUF_SIZE);
        lex_advance(lx); r.val = JS_NewString(ctx, buf); return r;
    }
    if (lx->type == TOK_TRUE)  { lex_advance(lx); r.val=JS_TRUE; return r; }
    if (lx->type == TOK_FALSE) { lex_advance(lx); r.val=JS_FALSE; return r; }
    if (lx->type == TOK_NULL_KW) { lex_advance(lx); r.val=JS_NULL; return r; }
    if (lx->type == TOK_UNDEFINED_KW) { lex_advance(lx); r.val=JS_UNDEFINED; return r; }
    if (lx->type == TOK_THIS) { lex_advance(lx); r.val = JS_DupValue(ctx, ctx->global_obj); return r; }

    if (lx->type == TOK_IDENT) {
        char name[TOK_BUF_SIZE]; kstrcpy(name, lx->buf, TOK_BUF_SIZE);
        lex_advance(lx);
        /* Check env first, then global */
        r.val = env_get(env, name);
        if (JS_IsUndefined(r.val))
            r.val = JS_GetPropertyStr(ctx, ctx->global_obj, name);
        return r;
    }

    if (lx->type == TOK_LPAREN) {
        lex_advance(lx);
        r = eval_expr(ctx, lx, env, depth);
        if (lx->type == TOK_RPAREN) lex_advance(lx);
        return r;
    }

    if (lx->type == TOK_LBRACKET) {
        lex_advance(lx);
        JSValue arr = JS_NewArray(ctx);
        uint32_t idx = 0;
        while (lx->type != TOK_RBRACKET && lx->type != TOK_EOF) {
            EvalResult er = eval_expr(ctx, lx, env, depth);
            JS_SetPropertyUint32(ctx, arr, idx++, er.val);
            if (lx->type == TOK_COMMA) lex_advance(lx);
        }
        if (lx->type == TOK_RBRACKET) lex_advance(lx);
        r.val = arr; return r;
    }

    if (lx->type == TOK_LBRACE) {
        lex_advance(lx);
        JSValue obj = JS_NewObject(ctx);
        while (lx->type != TOK_RBRACE && lx->type != TOK_EOF) {
            char key[TOK_BUF_SIZE];
            if (lx->type == TOK_IDENT || lx->type == TOK_STR) {
                kstrcpy(key, lx->buf, TOK_BUF_SIZE); lex_advance(lx);
            } else { r.val=obj; return r; }
            if (lx->type == TOK_COLON) lex_advance(lx);
            EvalResult vr = eval_expr(ctx, lx, env, depth);
            JS_SetPropertyStr(ctx, obj, key, vr.val);
            if (lx->type == TOK_COMMA) lex_advance(lx);
        }
        if (lx->type == TOK_RBRACE) lex_advance(lx);
        r.val = obj; return r;
    }

    if (lx->type == TOK_FUNCTION) {
        /* anonymous function expression — store as native object with source */
        lex_advance(lx);
        if (lx->type == TOK_IDENT) lex_advance(lx); /* optional name */
        /* skip params and body by brace-counting */
        if (lx->type == TOK_LPAREN) {
            int depth2=1; lex_advance(lx);
            while (lx->type!=TOK_EOF && depth2>0) {
                if(lx->type==TOK_LPAREN) depth2++;
                else if(lx->type==TOK_RPAREN) depth2--;
                lex_advance(lx);
            }
        }
        if (lx->type == TOK_LBRACE) {
            int depth2=1; lex_advance(lx);
            while (lx->type!=TOK_EOF && depth2>0) {
                if(lx->type==TOK_LBRACE) depth2++;
                else if(lx->type==TOK_RBRACE) depth2--;
                if(depth2>0) lex_advance(lx); else lex_advance(lx);
            }
        }
        r.val = JS_NULL; /* function expressions return as null (stub) */
        return r;
    }

    lex_advance(lx);
    r.val = JS_UNDEFINED; return r;
}

/* Call expression: fn(args) or obj.method(args) */
static EvalResult eval_call(JSContext *ctx, JSValue fn_val, JSValue this_val,
                             JSValue *args, int nargs, int depth) {
    EvalResult r; r.flow=FLOW_NORMAL; r.val=JS_UNDEFINED;
    JSObject *fn_obj = js_obj_ptr(ctx, fn_val);
    if (!fn_obj) return r;
    if (fn_obj->cfunc) {
        r.val = fn_obj->cfunc(ctx, this_val, nargs, args);
        if (ctx->has_exception) r.flow=FLOW_THROW;
    }
    return r;
}

/* Postfix/call/member expression */
static EvalResult eval_postfix(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult r = eval_primary(ctx, lx, env, depth);
    if (r.flow) return r;

    while (1) {
        if (lx->type == TOK_DOT) {
            lex_advance(lx);
            if (lx->type != TOK_IDENT && lx->type < TOK_VAR) { break; }
            char prop[TOK_BUF_SIZE]; kstrcpy(prop, lx->buf, TOK_BUF_SIZE);
            lex_advance(lx);
            JSValue obj = r.val;
            /* String methods */
            if (js_is_string(obj)) {
                JSString *s = js_str_ptr(ctx, obj);
                if (strcmp(prop,"length")==0) {
                    r.val = JS_MKINT(s ? s->len : 0); JS_FreeValue(ctx,obj); continue;
                }
                if (strcmp(prop,"indexOf")==0||strcmp(prop,"includes")==0||
                    strcmp(prop,"startsWith")==0||strcmp(prop,"endsWith")==0||
                    strcmp(prop,"slice")==0||strcmp(prop,"substring")==0||
                    strcmp(prop,"toLowerCase")==0||strcmp(prop,"toUpperCase")==0||
                    strcmp(prop,"trim")==0||strcmp(prop,"split")==0||
                    strcmp(prop,"replace")==0||strcmp(prop,"charAt")==0||
                    strcmp(prop,"charCodeAt")==0||strcmp(prop,"concat")==0||
                    strcmp(prop,"toString")==0||strcmp(prop,"valueOf")==0) {
                    /* Return a C function that captures obj */
                    /* For simplicity: handle immediately at call site below */
                    /* Store as a tagged pair — just keep obj + prop */
                    /* We'll handle the call specially */
                    if (lx->type == TOK_LPAREN) {
                        lex_advance(lx);
                        JSValue args[8]; int nargs=eval_args(ctx,lx,env,depth,args,8);
                        if (lx->type==TOK_RPAREN) lex_advance(lx);
                        const char *sd = s ? s->data : "";
                        int sl = s ? s->len : 0;
                        if (strcmp(prop,"length")==0) { r.val=JS_MKINT(sl); }
                        else if (strcmp(prop,"indexOf")==0||strcmp(prop,"includes")==0||strcmp(prop,"startsWith")==0) {
                            const char *needle = nargs?JS_ToCString(ctx,args[0]):"";
                            char *found = strstr(sd, needle);
                            if (strcmp(prop,"includes")==0||strcmp(prop,"startsWith")==0)
                                r.val = JS_MKBOOL(found!=NULL);
                            else r.val = found ? JS_MKINT((int)(found-sd)) : JS_MKINT(-1);
                        } else if (strcmp(prop,"slice")==0||strcmp(prop,"substring")==0) {
                            int start=0,end=sl;
                            if(nargs>=1){int v;JS_ToInt32(ctx,&v,args[0]);start=iclamp(v<0?sl+v:v,0,sl);}
                            if(nargs>=2){int v;JS_ToInt32(ctx,&v,args[1]);end=iclamp(v<0?sl+v:v,0,sl);}
                            if(end<start)end=start;
                            r.val = JS_NewStringLen(ctx, sd+start, (size_t)(end-start));
                        } else if (strcmp(prop,"toLowerCase")==0) {
                            char *buf=(char*)kmalloc((uint32_t)(sl+1));
                            for(int i=0;i<sl;i++) buf[i]=tolower((unsigned char)sd[i]); buf[sl]=0;
                            r.val=JS_NewString(ctx,buf); kfree(buf);
                        } else if (strcmp(prop,"toUpperCase")==0) {
                            char *buf=(char*)kmalloc((uint32_t)(sl+1));
                            for(int i=0;i<sl;i++) buf[i]=toupper((unsigned char)sd[i]); buf[sl]=0;
                            r.val=JS_NewString(ctx,buf); kfree(buf);
                        } else if (strcmp(prop,"trim")==0) {
                            int a=0,b=sl-1;
                            while(a<=b&&isspace((unsigned char)sd[a]))a++;
                            while(b>=a&&isspace((unsigned char)sd[b]))b--;
                            r.val=JS_NewStringLen(ctx,sd+a,(size_t)(b-a+1));
                        } else if (strcmp(prop,"toString")==0||strcmp(prop,"valueOf")==0) {
                            r.val=JS_DupValue(ctx,obj);
                        } else if (strcmp(prop,"charAt")==0) {
                            int idx2=0; if(nargs)JS_ToInt32(ctx,&idx2,args[0]);
                            char ch[2]={idx2>=0&&idx2<sl?sd[idx2]:0,0};
                            r.val=JS_NewString(ctx,ch);
                        } else if (strcmp(prop,"charCodeAt")==0) {
                            int idx2=0; if(nargs)JS_ToInt32(ctx,&idx2,args[0]);
                            r.val=JS_MKINT(idx2>=0&&idx2<sl?(unsigned char)sd[idx2]:0);
                        } else if (strcmp(prop,"concat")==0) {
                            JSValue acc=JS_DupValue(ctx,obj);
                            for(int i=0;i<nargs;i++){JSValue n2=js_concat_str(ctx,acc,args[i]);JS_FreeValue(ctx,acc);acc=n2;}
                            r.val=acc;
                        } else { r.val=JS_UNDEFINED; }
                        for(int i=0;i<nargs;i++) JS_FreeValue(ctx,args[i]);
                        JS_FreeValue(ctx,obj); continue;
                    }
                }
                JS_FreeValue(ctx,obj); r.val=JS_UNDEFINED; continue;
            }
            /* Object property */
            JSValue obj_val = obj;
            r.val = JS_GetPropertyStr(ctx, obj_val, prop);
            if (lx->type == TOK_LPAREN) {
                /* method call */
                lex_advance(lx);
                JSValue args[16]; int nargs=eval_args(ctx,lx,env,depth,args,16);
                if (lx->type==TOK_RPAREN) lex_advance(lx);
                EvalResult cr = eval_call(ctx, r.val, obj_val, args, nargs, depth+1);
                for(int i=0;i<nargs;i++) JS_FreeValue(ctx,args[i]);
                JS_FreeValue(ctx,r.val); JS_FreeValue(ctx,obj_val);
                r=cr; continue;
            }
            JS_FreeValue(ctx,obj_val); continue;
        }

        if (lx->type == TOK_LBRACKET) {
            lex_advance(lx);
            EvalResult idx = eval_expr(ctx, lx, env, depth);
            if (lx->type==TOK_RBRACKET) lex_advance(lx);
            JSValue obj=r.val;
            if (JS_IsInt(idx.val)||JS_IsFloat(idx.val)) {
                int i; JS_ToInt32(ctx,&i,idx.val);
                r.val = JS_GetPropertyUint32(ctx,obj,(uint32_t)i);
            } else {
                const char *k = JS_ToCString(ctx, idx.val);
                r.val = JS_GetPropertyStr(ctx, obj, k);
            }
            JS_FreeValue(ctx,idx.val); JS_FreeValue(ctx,obj); continue;
        }

        if (lx->type == TOK_LPAREN) {
            lex_advance(lx);
            JSValue fn=r.val;
            JSValue args[16]; int nargs=eval_args(ctx,lx,env,depth,args,16);
            if (lx->type==TOK_RPAREN) lex_advance(lx);
            EvalResult cr = eval_call(ctx, fn, JS_DupValue(ctx,ctx->global_obj), args, nargs, depth+1);
            for(int i=0;i<nargs;i++) JS_FreeValue(ctx,args[i]);
            JS_FreeValue(ctx,fn); r=cr; continue;
        }

        if (lx->type == TOK_PLUSPLUS) {
            lex_advance(lx); double d=js_to_num(ctx,r.val);
            r.val=JS_MKFLOAT(d); /* postfix returns old value; assignment not tracked here */
            continue;
        }
        if (lx->type == TOK_MINUSMINUS) {
            lex_advance(lx); double d=js_to_num(ctx,r.val);
            r.val=JS_MKFLOAT(d); continue;
        }
        break;
    }
    return r;
}

/* Unary */
static EvalResult eval_unary(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    if (lx->type == TOK_MINUS) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        double d=js_to_num(ctx,r.val); JS_FreeValue(ctx,r.val);
        r.val=JS_MKFLOAT(-d); return r;
    }
    if (lx->type == TOK_PLUS) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        double d=js_to_num(ctx,r.val); JS_FreeValue(ctx,r.val);
        r.val=JS_MKFLOAT(d); return r;
    }
    if (lx->type == TOK_BANG) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        int b=js_to_bool(ctx,r.val); JS_FreeValue(ctx,r.val);
        r.val=JS_MKBOOL(!b); return r;
    }
    if (lx->type == TOK_TILDE) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        int i; JS_ToInt32(ctx,&i,r.val); JS_FreeValue(ctx,r.val);
        r.val=JS_MKINT(~i); return r;
    }
    if (lx->type == TOK_TYPEOF) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        const char *t;
        int tag=JS_VALUE_GET_TAG(r.val);
        if(tag==JS_TAG_UNDEFINED) t="undefined";
        else if(tag==JS_TAG_BOOL) t="boolean";
        else if(tag==JS_TAG_INT||tag==JS_TAG_FLOAT64) t="number";
        else if(tag==JS_TAG_STRING) t="string";
        else if(tag==JS_TAG_NULL) t="object";
        else t="object";
        JS_FreeValue(ctx,r.val); r.val=JS_NewString(ctx,t); return r;
    }
    if (lx->type == TOK_PLUSPLUS) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        double d=js_to_num(ctx,r.val)+1; JS_FreeValue(ctx,r.val);
        r.val=JS_MKFLOAT(d); return r;
    }
    if (lx->type == TOK_MINUSMINUS) {
        lex_advance(lx);
        EvalResult r = eval_unary(ctx,lx,env,depth);
        double d=js_to_num(ctx,r.val)-1; JS_FreeValue(ctx,r.val);
        r.val=JS_MKFLOAT(d); return r;
    }
    if (lx->type == TOK_NEW) {
        lex_advance(lx);
        /* skip constructor + args — return empty object */
        while(lx->type!=TOK_SEMI&&lx->type!=TOK_EOF&&lx->type!=TOK_RBRACE&&
              lx->type!=TOK_COMMA&&lx->type!=TOK_RPAREN) lex_advance(lx);
        EvalResult r; r.flow=FLOW_NORMAL; r.val=JS_NewObject(ctx); return r;
    }
    return eval_postfix(ctx,lx,env,depth);
}

/* Binary operators in precedence order */
static EvalResult eval_mul(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_unary(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_STAR||lx->type==TOK_SLASH||lx->type==TOK_PERCENT) {
        TokenType op=lx->type; lex_advance(lx);
        EvalResult rhs=eval_unary(ctx,lx,env,depth);
        double l=js_to_num(ctx,lhs.val), r2=js_to_num(ctx,rhs.val);
        JS_FreeValue(ctx,lhs.val); JS_FreeValue(ctx,rhs.val);
        double res = op==TOK_STAR?l*r2 : op==TOK_SLASH?(r2?l/r2:INFINITY) : kt_fmod(l,r2);
        lhs.val=JS_MKFLOAT(res);
    }
    return lhs;
}
static EvalResult eval_add(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_mul(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_PLUS||lx->type==TOK_MINUS) {
        TokenType op=lx->type; lex_advance(lx);
        EvalResult rhs=eval_mul(ctx,lx,env,depth);
        if(op==TOK_PLUS&&(js_is_string(lhs.val)||js_is_string(rhs.val))) {
            JSValue s=js_concat_str(ctx,lhs.val,rhs.val);
            JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val); lhs.val=s;
        } else {
            double l=js_to_num(ctx,lhs.val),r2=js_to_num(ctx,rhs.val);
            JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val);
            lhs.val=JS_MKFLOAT(op==TOK_PLUS?l+r2:l-r2);
        }
    }
    return lhs;
}
static EvalResult eval_shift(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_add(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_LSHIFT||lx->type==TOK_RSHIFT) {
        TokenType op=lx->type; lex_advance(lx);
        EvalResult rhs=eval_add(ctx,lx,env,depth);
        int l,r2; JS_ToInt32(ctx,&l,lhs.val); JS_ToInt32(ctx,&r2,rhs.val);
        JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val);
        lhs.val=JS_MKINT(op==TOK_LSHIFT?l<<r2:l>>r2);
    }
    return lhs;
}
static EvalResult eval_rel(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_shift(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_LT||lx->type==TOK_GT||lx->type==TOK_LTE||lx->type==TOK_GTE||
          lx->type==TOK_IN||lx->type==TOK_INSTANCEOF) {
        TokenType op=lx->type; lex_advance(lx);
        EvalResult rhs=eval_shift(ctx,lx,env,depth);
        int res;
        if(js_is_string(lhs.val)&&js_is_string(rhs.val)) {
            int c=strcmp(JS_ToCString(ctx,lhs.val),JS_ToCString(ctx,rhs.val));
            res=(op==TOK_LT?c<0:op==TOK_GT?c>0:op==TOK_LTE?c<=0:c>=0);
        } else {
            double l=js_to_num(ctx,lhs.val),r2=js_to_num(ctx,rhs.val);
            res=(op==TOK_LT?l<r2:op==TOK_GT?l>r2:op==TOK_LTE?l<=r2:l>=r2);
        }
        JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val);
        lhs.val=JS_MKBOOL(res);
    }
    return lhs;
}
static EvalResult eval_eq(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_rel(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_EQ2||lx->type==TOK_EQ3||lx->type==TOK_NEQ||lx->type==TOK_NEQ2) {
        int strict=(lx->type==TOK_EQ3||lx->type==TOK_NEQ2);
        int neg=(lx->type==TOK_NEQ||lx->type==TOK_NEQ2);
        lex_advance(lx);
        EvalResult rhs=eval_rel(ctx,lx,env,depth);
        int eq=0;
        int lt=JS_VALUE_GET_TAG(lhs.val), rt2=JS_VALUE_GET_TAG(rhs.val);
        if(lt==rt2){
            if(lt==JS_TAG_NULL||lt==JS_TAG_UNDEFINED) eq=1;
            else if(lt==JS_TAG_BOOL) eq=JS_VALUE_GET_BOOL(lhs.val)==JS_VALUE_GET_BOOL(rhs.val);
            else if(lt==JS_TAG_INT)  eq=JS_VALUE_GET_INT(lhs.val)==JS_VALUE_GET_INT(rhs.val);
            else if(lt==JS_TAG_FLOAT64) eq=JS_VALUE_GET_FLOAT64(lhs.val)==JS_VALUE_GET_FLOAT64(rhs.val);
            else if(lt==JS_TAG_STRING) eq=strcmp(JS_ToCString(ctx,lhs.val),JS_ToCString(ctx,rhs.val))==0;
            else eq=(lhs.val==rhs.val);
        } else if(!strict) {
            /* loose equality coercion */
            double l=js_to_num(ctx,lhs.val), r2=js_to_num(ctx,rhs.val);
            eq=(l==r2);
        }
        JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val);
        lhs.val=JS_MKBOOL(neg?!eq:eq);
    }
    return lhs;
}
static EvalResult eval_bitand(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_eq(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_AMP) { lex_advance(lx);
        EvalResult rhs=eval_eq(ctx,lx,env,depth);
        int l,r2; JS_ToInt32(ctx,&l,lhs.val);JS_ToInt32(ctx,&r2,rhs.val);
        JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val); lhs.val=JS_MKINT(l&r2);
    } return lhs;
}
static EvalResult eval_bitor(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_bitand(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_PIPE) { lex_advance(lx);
        EvalResult rhs=eval_bitand(ctx,lx,env,depth);
        int l,r2; JS_ToInt32(ctx,&l,lhs.val);JS_ToInt32(ctx,&r2,rhs.val);
        JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val); lhs.val=JS_MKINT(l|r2);
    } return lhs;
}
static EvalResult eval_logand(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_bitor(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_AND2) { lex_advance(lx);
        if(!js_to_bool(ctx,lhs.val)) {
            /* short-circuit: skip rhs */
            while(lx->type!=TOK_SEMI&&lx->type!=TOK_EOF&&
                  lx->type!=TOK_COMMA&&lx->type!=TOK_RPAREN) lex_advance(lx);
            return lhs;
        }
        JS_FreeValue(ctx,lhs.val); lhs=eval_bitor(ctx,lx,env,depth);
    } return lhs;
}
static EvalResult eval_logor(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs=eval_logand(ctx,lx,env,depth); if(lhs.flow) return lhs;
    while(lx->type==TOK_OR2) { lex_advance(lx);
        if(js_to_bool(ctx,lhs.val)) {
            while(lx->type!=TOK_SEMI&&lx->type!=TOK_EOF&&
                  lx->type!=TOK_COMMA&&lx->type!=TOK_RPAREN) lex_advance(lx);
            return lhs;
        }
        JS_FreeValue(ctx,lhs.val); lhs=eval_logand(ctx,lx,env,depth);
    } return lhs;
}
static EvalResult eval_ternary(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult cond=eval_logor(ctx,lx,env,depth); if(cond.flow) return cond;
    if(lx->type==TOK_QUESTION) {
        lex_advance(lx);
        int b=js_to_bool(ctx,cond.val); JS_FreeValue(ctx,cond.val);
        EvalResult t=eval_expr(ctx,lx,env,depth);
        if(lx->type==TOK_COLON) lex_advance(lx);
        EvalResult f=eval_expr(ctx,lx,env,depth);
        if(b){JS_FreeValue(ctx,f.val);return t;}
        else{JS_FreeValue(ctx,t.val);return f;}
    }
    return cond;
}

/* Assignment expression */
static EvalResult eval_expr(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult lhs = eval_ternary(ctx, lx, env, depth);
    if (lhs.flow) return lhs;
    if (lx->type==TOK_EQ||lx->type==TOK_PLUSEQ||lx->type==TOK_MINUSEQ||
        lx->type==TOK_STAREQ||lx->type==TOK_SLASHEQ) {
        TokenType op=lx->type; lex_advance(lx);
        EvalResult rhs=eval_expr(ctx,lx,env,depth);
        /* We can't easily do L-value assignment here without a full AST.
         * For the common case of `ident = expr` we rely on the statement parser.
         * Just return rhs. */
        if(op==TOK_PLUSEQ&&(js_is_string(lhs.val)||js_is_string(rhs.val))) {
            JSValue s=js_concat_str(ctx,lhs.val,rhs.val);
            JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val); lhs.val=s;
        } else if(op!=TOK_EQ) {
            double l=js_to_num(ctx,lhs.val),r2=js_to_num(ctx,rhs.val);
            JS_FreeValue(ctx,lhs.val);JS_FreeValue(ctx,rhs.val);
            lhs.val=JS_MKFLOAT(op==TOK_PLUSEQ?l+r2:op==TOK_MINUSEQ?l-r2:
                               op==TOK_STAREQ?l*r2:r2?l/r2:INFINITY);
        } else {
            JS_FreeValue(ctx,lhs.val); lhs.val=rhs.val;
        }
    }
    return lhs;
}

/* ================================================================== */
/*  Statement evaluator                                                */
/* ================================================================== */
static EvalResult eval_stmt(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult r; r.flow=FLOW_NORMAL; r.val=JS_UNDEFINED;
    if (depth > JS_MAX_DEPTH) return r;

    /* var/let/const declaration */
    if (lx->type==TOK_VAR||lx->type==TOK_LET||lx->type==TOK_CONST) {
        lex_advance(lx);
        while (1) {
            if (lx->type!=TOK_IDENT) break;
            char name[TOK_BUF_SIZE]; kstrcpy(name,lx->buf,TOK_BUF_SIZE);
            lex_advance(lx);
            JSValue val=JS_UNDEFINED;
            if (lx->type==TOK_EQ) {
                lex_advance(lx);
                EvalResult er=eval_expr(ctx,lx,env,depth);
                val=er.val;
                if(er.flow){r=er;return r;}
            }
            env_set_local(env,name,val);
            JS_FreeValue(ctx,val);
            if (lx->type!=TOK_COMMA) break;
            lex_advance(lx);
        }
        if (lx->type==TOK_SEMI) lex_advance(lx);
        return r;
    }

    /* function declaration */
    if (lx->type==TOK_FUNCTION) {
        lex_advance(lx);
        char fname[TOK_BUF_SIZE]; fname[0]=0;
        if (lx->type==TOK_IDENT) { kstrcpy(fname,lx->buf,TOK_BUF_SIZE); lex_advance(lx); }
        /* skip params */
        if (lx->type==TOK_LPAREN) {
            int d2=1; lex_advance(lx);
            while(lx->type!=TOK_EOF&&d2>0){
                if(lx->type==TOK_LPAREN)d2++; else if(lx->type==TOK_RPAREN)d2--;
                if(d2>0)lex_advance(lx); else lex_advance(lx);
            }
        }
        /* skip body */
        if (lx->type==TOK_LBRACE) {
            int d2=1; lex_advance(lx);
            while(lx->type!=TOK_EOF&&d2>0){
                if(lx->type==TOK_LBRACE)d2++; else if(lx->type==TOK_RBRACE)d2--;
                if(d2>0)lex_advance(lx); else lex_advance(lx);
            }
        }
        /* register as null in env for now */
        if (fname[0]) env_set_local(env,fname,JS_NULL);
        return r;
    }

    /* return */
    if (lx->type==TOK_RETURN) {
        lex_advance(lx);
        if (lx->type!=TOK_SEMI&&lx->type!=TOK_RBRACE&&lx->type!=TOK_EOF) {
            r=eval_expr(ctx,lx,env,depth);
        }
        if(lx->type==TOK_SEMI)lex_advance(lx);
        r.flow=FLOW_RETURN; return r;
    }

    /* throw */
    if (lx->type==TOK_THROW) {
        lex_advance(lx);
        r=eval_expr(ctx,lx,env,depth);
        if(lx->type==TOK_SEMI)lex_advance(lx);
        ctx->has_exception=1; ctx->exception=r.val;
        r.flow=FLOW_THROW; return r;
    }

    /* break / continue */
    if (lx->type==TOK_BREAK)    { lex_advance(lx); if(lx->type==TOK_SEMI)lex_advance(lx); r.flow=FLOW_BREAK; return r; }
    if (lx->type==TOK_CONTINUE) { lex_advance(lx); if(lx->type==TOK_SEMI)lex_advance(lx); r.flow=FLOW_CONTINUE; return r; }

    /* if/else */
    if (lx->type==TOK_IF) {
        lex_advance(lx);
        if(lx->type==TOK_LPAREN) lex_advance(lx);
        EvalResult cond=eval_expr(ctx,lx,env,depth);
        if(lx->type==TOK_RPAREN) lex_advance(lx);
        int b=js_to_bool(ctx,cond.val); JS_FreeValue(ctx,cond.val);
        JSEnv *child=env_push(env,ctx);
        if(b) r=eval_stmt(ctx,lx,child,depth+1);
        else {
            /* skip then branch */
            if(lx->type==TOK_LBRACE){
                int d2=1;lex_advance(lx);
                while(lx->type!=TOK_EOF&&d2>0){
                    if(lx->type==TOK_LBRACE)d2++; else if(lx->type==TOK_RBRACE)d2--;
                    lex_advance(lx);
                }
            } else {
                while(lx->type!=TOK_SEMI&&lx->type!=TOK_EOF) lex_advance(lx);
                if(lx->type==TOK_SEMI)lex_advance(lx);
            }
            if(lx->type==TOK_ELSE){lex_advance(lx);r=eval_stmt(ctx,lx,child,depth+1);}
        }
        env_pop(child); return r;
    }

    /* while */
    if (lx->type==TOK_WHILE) {
        lex_advance(lx);
        int cond_start=lx->pos;
        if(lx->type==TOK_LPAREN)lex_advance(lx);
        for(int iter=0;iter<100000;iter++){
            int saved_pos=lx->pos; (void)saved_pos;
            lx->pos=cond_start;
            if(lx->type==TOK_LPAREN)lex_advance(lx); else {lx->pos=cond_start;}
            /* re-lex from cond_start */
            lex_advance(lx); /* hack: re-read the condition */
            EvalResult cond=eval_expr(ctx,lx,env,depth);
            if(lx->type==TOK_RPAREN)lex_advance(lx);
            int b=js_to_bool(ctx,cond.val); JS_FreeValue(ctx,cond.val);
            if(!b) break;
            JSEnv *child=env_push(env,ctx);
            r=eval_stmt(ctx,lx,child,depth+1);
            env_pop(child);
            if(r.flow==FLOW_BREAK){r.flow=FLOW_NORMAL;break;}
            if(r.flow==FLOW_RETURN||r.flow==FLOW_THROW) return r;
        }
        return r;
    }

    /* for — simplified (only `for(var i=0;i<n;i++)` pattern) */
    if (lx->type==TOK_FOR) {
        lex_advance(lx);
        if(lx->type==TOK_LPAREN)lex_advance(lx);
        JSEnv *for_env=env_push(env,ctx);
        /* init */
        if(lx->type!=TOK_SEMI) eval_stmt(ctx,lx,for_env,depth);
        else lex_advance(lx);
        /* Record position after init for re-evaluation */
        int cond_pos=lx->pos; TokenType cond_type=lx->type;
        (void)cond_type;
        for(int iter=0;iter<100000;iter++){
            /* condition */
            if(lx->pos==cond_pos) { /* first iter already at cond */ }
            else lx->pos=cond_pos;
            lex_advance(lx); /* force re-lex at cond_pos */
            int b=1;
            if(lx->type!=TOK_SEMI){
                EvalResult cond=eval_expr(ctx,lx,env,depth);
                b=js_to_bool(ctx,cond.val); JS_FreeValue(ctx,cond.val);
            }
            if(lx->type==TOK_SEMI)lex_advance(lx);
            if(!b) break;
            /* skip increment for now, skip to body */
            while(lx->type!=TOK_RPAREN&&lx->type!=TOK_EOF) lex_advance(lx);
            if(lx->type==TOK_RPAREN)lex_advance(lx);
            /* body */
            JSEnv *child=env_push(for_env,ctx);
            r=eval_stmt(ctx,lx,child,depth+1);
            env_pop(child);
            if(r.flow==FLOW_BREAK){r.flow=FLOW_NORMAL;break;}
            if(r.flow==FLOW_RETURN||r.flow==FLOW_THROW){env_pop(for_env);return r;}
            lx->pos=cond_pos;
        }
        env_pop(for_env);
        return r;
    }

    /* try/catch/finally */
    if (lx->type==TOK_TRY) {
        lex_advance(lx);
        JSEnv *try_env=env_push(env,ctx);
        r=eval_stmt(ctx,lx,try_env,depth+1);
        env_pop(try_env);
        if(lx->type==TOK_CATCH) {
            lex_advance(lx);
            int had_ex=r.flow==FLOW_THROW||ctx->has_exception;
            /* skip (e) */
            if(lx->type==TOK_LPAREN){int d2=1;lex_advance(lx);while(lx->type!=TOK_EOF&&d2>0){if(lx->type==TOK_LPAREN)d2++;else if(lx->type==TOK_RPAREN)d2--;lex_advance(lx);}}
            JSEnv *cat_env=env_push(env,ctx);
            if(had_ex){ctx->has_exception=0;r.flow=FLOW_NORMAL;eval_stmt(ctx,lx,cat_env,depth+1);}
            else{/* skip catch body */if(lx->type==TOK_LBRACE){int d2=1;lex_advance(lx);while(lx->type!=TOK_EOF&&d2>0){if(lx->type==TOK_LBRACE)d2++;else if(lx->type==TOK_RBRACE)d2--;if(d2>0)lex_advance(lx);else lex_advance(lx);}}}
            env_pop(cat_env);
        }
        if(lx->type==TOK_FINALLY){lex_advance(lx);JSEnv*fe=env_push(env,ctx);eval_stmt(ctx,lx,fe,depth+1);env_pop(fe);}
        return r;
    }

    /* block */
    if (lx->type==TOK_LBRACE) {
        return eval_block(ctx,lx,env,depth);
    }

    /* expression statement */
    if (lx->type!=TOK_SEMI&&lx->type!=TOK_EOF) {
        r=eval_expr(ctx,lx,env,depth);
        JS_FreeValue(ctx,r.val); r.val=JS_UNDEFINED;
    }
    if(lx->type==TOK_SEMI) lex_advance(lx);
    return r;
}

static EvalResult eval_block(JSContext *ctx, Lexer *lx, JSEnv *env, int depth) {
    EvalResult r; r.flow=FLOW_NORMAL; r.val=JS_UNDEFINED;
    if(lx->type==TOK_LBRACE) lex_advance(lx);
    JSEnv *block=env_push(env,ctx);
    while(lx->type!=TOK_RBRACE&&lx->type!=TOK_EOF) {
        JS_FreeValue(ctx,r.val);
        r=eval_stmt(ctx,lx,block,depth+1);
        if(r.flow) break;
    }
    if(lx->type==TOK_RBRACE) lex_advance(lx);
    env_pop(block);
    return r;
}

/* ================================================================== */
/*  JSON parser                                                        */
/* ================================================================== */
JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len,
                     const char *filename) {
    (void)filename;
    Lexer lx; lex_init(&lx, buf, (int)buf_len);
    lex_advance(&lx);
    JSEnv *env=env_push(NULL,ctx);
    EvalResult r=eval_expr(ctx,&lx,env,0);
    env_pop(env);
    return r.val;
}

JSValue JS_JSONStringify(JSContext *ctx, JSValue obj, JSValue replacer, JSValue space) {
    (void)replacer; (void)space;
    /* Simple: just ToCString for scalars, "{...}" for objects */
    int tag=JS_VALUE_GET_TAG(obj);
    if(tag==JS_TAG_NULL) return JS_NewString(ctx,"null");
    if(tag==JS_TAG_UNDEFINED) return JS_NewString(ctx,"undefined");
    if(tag==JS_TAG_BOOL) return JS_NewString(ctx,JS_VALUE_GET_BOOL(obj)?"true":"false");
    if(tag==JS_TAG_INT||tag==JS_TAG_FLOAT64) return JS_ToString(ctx,obj);
    if(tag==JS_TAG_STRING) {
        const char *s=JS_ToCString(ctx,obj);
        int n=(int)strlen(s); char *buf=(char*)kmalloc((uint32_t)(n+3));
        buf[0]='"'; memcpy(buf+1,s,n); buf[n+1]='"'; buf[n+2]=0;
        JSValue r=JS_NewString(ctx,buf); kfree(buf); return r;
    }
    return JS_NewString(ctx,"[object]");
}

/* ================================================================== */
/*  Built-in functions                                                 */
/* ================================================================== */
static JSContext *g_builtin_ctx; /* set during init */

static JSValue js_console_log(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val;
    char buf[512]; buf[0]=0;
    for(int i=0;i<argc;i++){
        if(i) kstrcat(buf," ",sizeof buf);
        kstrcat(buf,JS_ToCString(ctx,argv[i]),sizeof buf);
    }
    if(ctx->console_cb) ctx->console_cb(buf,0);
    return JS_UNDEFINED;
}
static JSValue js_console_error(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val;
    char buf[512]; buf[0]=0;
    for(int i=0;i<argc;i++){if(i)kstrcat(buf," ",sizeof buf);kstrcat(buf,JS_ToCString(ctx,argv[i]),sizeof buf);}
    if(ctx->console_cb) ctx->console_cb(buf,1);
    return JS_UNDEFINED;
}
static JSValue js_doc_write(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val;
    if(argc>0&&ctx->docwrite_cb) ctx->docwrite_cb(JS_ToCString(ctx,argv[0]));
    return JS_UNDEFINED;
}
static JSValue js_parse_int(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val;
    if(!argc) return JS_MKINT(0);
    const char *s=JS_ToCString(ctx,argv[0]);
    int base=10; if(argc>=2){int b;JS_ToInt32(ctx,&b,argv[1]);base=b?b:10;}
    long v=kt_strtol(s,NULL,base); return JS_MKINT((int)v);
}
static JSValue js_parse_float(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val;
    if(!argc) return JS_MKFLOAT(NAN);
    const char *s=JS_ToCString(ctx,argv[0]);
    return JS_MKFLOAT(kt_strtod(s,NULL));
}
static JSValue js_is_nan(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val; double d=0; if(argc)JS_ToFloat64(ctx,&d,argv[0]); return JS_MKBOOL(kt_isnan(d));
}
static JSValue js_is_finite(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    (void)this_val; double d=0; if(argc)JS_ToFloat64(ctx,&d,argv[0]); return JS_MKBOOL(kt_isfinite(d));
}
static JSValue js_math_sqrt(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double d=0;if(argc)JS_ToFloat64(ctx,&d,argv[0]);return JS_MKFLOAT(kt_sqrt(d));}
static JSValue js_math_floor(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double d=0;if(argc)JS_ToFloat64(ctx,&d,argv[0]);return JS_MKFLOAT(kt_floor(d));}
static JSValue js_math_ceil(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double d=0;if(argc)JS_ToFloat64(ctx,&d,argv[0]);return JS_MKFLOAT(kt_ceil(d));}
static JSValue js_math_round(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double d=0;if(argc)JS_ToFloat64(ctx,&d,argv[0]);return JS_MKFLOAT(kt_round(d));}
static JSValue js_math_abs(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double d=0;if(argc)JS_ToFloat64(ctx,&d,argv[0]);return JS_MKFLOAT(kt_fabs(d));}
static JSValue js_math_pow(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double a=0,b=0;if(argc>=1)JS_ToFloat64(ctx,&a,argv[0]);if(argc>=2)JS_ToFloat64(ctx,&b,argv[1]);return JS_MKFLOAT(kt_pow(a,b));}
static JSValue js_math_max(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double r=-INFINITY;for(int i=0;i<argc;i++){double d;JS_ToFloat64(ctx,&d,argv[i]);if(d>r)r=d;}return JS_MKFLOAT(r);}
static JSValue js_math_min(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)tv;double r=INFINITY;for(int i=0;i<argc;i++){double d;JS_ToFloat64(ctx,&d,argv[i]);if(d<r)r=d;}return JS_MKFLOAT(r);}
static JSValue js_math_random(JSContext *ctx, JSValue tv, int argc, JSValue *argv){(void)ctx;(void)tv;(void)argc;(void)argv;return JS_MKFLOAT((double)kt_rand()/(double)0xFFFFFFFF);}

static void install_builtins(JSContext *ctx) {
    /* console */
    JSValue cons=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,cons,"log",   JS_NewCFunction(ctx,js_console_log,  "log",   1));
    JS_SetPropertyStr(ctx,cons,"error", JS_NewCFunction(ctx,js_console_error,"error", 1));
    JS_SetPropertyStr(ctx,cons,"warn",  JS_NewCFunction(ctx,js_console_error,"warn",  1));
    JS_SetPropertyStr(ctx,cons,"info",  JS_NewCFunction(ctx,js_console_log,  "info",  1));
    JS_SetGlobal(ctx,"console",cons);
    /* document */
    JSValue doc=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,doc,"write",   JS_NewCFunction(ctx,js_doc_write,"write",1));
    JS_SetPropertyStr(ctx,doc,"writeln", JS_NewCFunction(ctx,js_doc_write,"writeln",1));
    JS_SetGlobal(ctx,"document",doc);
    /* globals */
    JS_SetGlobal(ctx,"parseInt",   JS_NewCFunction(ctx,js_parse_int,   "parseInt",   2));
    JS_SetGlobal(ctx,"parseFloat", JS_NewCFunction(ctx,js_parse_float, "parseFloat", 1));
    JS_SetGlobal(ctx,"isNaN",      JS_NewCFunction(ctx,js_is_nan,      "isNaN",      1));
    JS_SetGlobal(ctx,"isFinite",   JS_NewCFunction(ctx,js_is_finite,   "isFinite",   1));
    JS_SetGlobal(ctx,"NaN",        JS_MKFLOAT(NAN));
    JS_SetGlobal(ctx,"Infinity",   JS_MKFLOAT(INFINITY));
    JS_SetGlobal(ctx,"undefined",  JS_UNDEFINED);
    /* Math */
    JSValue math=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,math,"PI",     JS_MKFLOAT(M_PI));
    JS_SetPropertyStr(ctx,math,"E",      JS_MKFLOAT(M_E));
    JS_SetPropertyStr(ctx,math,"sqrt",   JS_NewCFunction(ctx,js_math_sqrt,  "sqrt",  1));
    JS_SetPropertyStr(ctx,math,"floor",  JS_NewCFunction(ctx,js_math_floor, "floor", 1));
    JS_SetPropertyStr(ctx,math,"ceil",   JS_NewCFunction(ctx,js_math_ceil,  "ceil",  1));
    JS_SetPropertyStr(ctx,math,"round",  JS_NewCFunction(ctx,js_math_round, "round", 1));
    JS_SetPropertyStr(ctx,math,"abs",    JS_NewCFunction(ctx,js_math_abs,   "abs",   1));
    JS_SetPropertyStr(ctx,math,"pow",    JS_NewCFunction(ctx,js_math_pow,   "pow",   2));
    JS_SetPropertyStr(ctx,math,"max",    JS_NewCFunction(ctx,js_math_max,   "max",   2));
    JS_SetPropertyStr(ctx,math,"min",    JS_NewCFunction(ctx,js_math_min,   "min",   2));
    JS_SetPropertyStr(ctx,math,"random", JS_NewCFunction(ctx,js_math_random,"random",0));
    JS_SetGlobal(ctx,"Math",math);
}

/* ================================================================== */
/*  Runtime / Context lifecycle                                        */
/* ================================================================== */
JSRuntime *JS_NewRuntime(void) {
    JSRuntime *rt=(JSRuntime*)kmalloc(sizeof(JSRuntime));
    if(!rt) return NULL;
    memset(rt,0,sizeof(JSRuntime));
    rt->mem_limit=16*1024*1024; /* 16 MB default */
    return rt;
}
void JS_FreeRuntime(JSRuntime *rt) { if(rt) kfree(rt); }

JSContext *JS_NewContext(JSRuntime *rt) {
    JSContext *ctx=(JSContext*)kmalloc(sizeof(JSContext));
    if(!ctx) return NULL;
    memset(ctx,0,sizeof(JSContext));
    ctx->rt=rt;
    ctx->exception=JS_UNDEFINED;
    ctx->global_obj=js_new_object_internal(ctx);
    install_builtins(ctx);
    g_builtin_ctx=ctx;
    return ctx;
}
void JS_FreeContext(JSContext *ctx) {
    if(!ctx) return;
    JS_FreeValue(ctx,ctx->global_obj);
    kfree(ctx);
}
void JS_SetMemoryLimit(JSRuntime *rt, size_t limit) { if(rt) rt->mem_limit=limit; }

/* ================================================================== */
/*  JS_Eval — the main entry point                                    */
/* ================================================================== */
JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags) {
    (void)filename; (void)eval_flags;
    if(!ctx||!input) return JS_UNDEFINED;
    Lexer lx; lex_init(&lx,input,(int)input_len); lex_advance(&lx);
    JSEnv *env=env_push(NULL,ctx);
    EvalResult r; r.flow=FLOW_NORMAL; r.val=JS_UNDEFINED;
    while(lx.type!=TOK_EOF) {
        JS_FreeValue(ctx,r.val);
        r=eval_stmt(ctx,&lx,env,0);
        if(r.flow==FLOW_THROW||r.flow==FLOW_RETURN) break;
    }
    env_pop(env);
    if(r.flow==FLOW_THROW) return JS_EXCEPTION;
    return r.val;
}
