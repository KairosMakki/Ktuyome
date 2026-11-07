/*
 * quickjs_kt.h — QuickJS JavaScript engine interface for ktuyome
 *
 * Minimal ES2020-subset interpreter: enough to run simple scripts
 * returned from web pages (ad-hoc DOM manipulation, no async).
 *
 * Limitations vs upstream QuickJS:
 *   - No async/await, no Promises (single-threaded polling OS)
 *   - No eval() security sandbox
 *   - No SharedArrayBuffer, no Workers
 *   - No module system (scripts run as global scripts)
 *   - No BigInt
 *   - parseInt/parseFloat/JSON.parse/JSON.stringify implemented
 *   - console.log/error/warn output to browser's log lines
 *   - document.write() appends to browser's text buffer
 */
#pragma once
#include "../ktuyome_compat.h"

/* ------------------------------------------------------------------ */
/*  Opaque runtime / context types                                     */
/* ------------------------------------------------------------------ */
typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;

/* JSValue: NaN-boxing on 32-bit — 64-bit value */
typedef uint64_t JSValue;

/* ------------------------------------------------------------------ */
/*  JSValue tag constants (NaN-box scheme)                             */
/* ------------------------------------------------------------------ */
#define JS_TAG_INT          0
#define JS_TAG_FLOAT64      1
#define JS_TAG_STRING       2
#define JS_TAG_OBJECT       3
#define JS_TAG_BOOL         4
#define JS_TAG_NULL         5
#define JS_TAG_UNDEFINED    6
#define JS_TAG_EXCEPTION    7
#define JS_TAG_UNINITIALIZED 8
#define JS_TAG_SYMBOL       9

/* ------------------------------------------------------------------ */
/*  Sentinel values                                                    */
/* ------------------------------------------------------------------ */
#define JS_NULL        ((JSValue)((uint64_t)JS_TAG_NULL      <<32))
#define JS_UNDEFINED   ((JSValue)((uint64_t)JS_TAG_UNDEFINED <<32))
#define JS_FALSE       ((JSValue)((uint64_t)JS_TAG_BOOL      <<32)|0)
#define JS_TRUE        ((JSValue)((uint64_t)JS_TAG_BOOL      <<32)|1)
#define JS_EXCEPTION   ((JSValue)((uint64_t)JS_TAG_EXCEPTION <<32))
#define JS_UNINITIALIZED ((JSValue)((uint64_t)JS_TAG_UNINITIALIZED<<32))

static inline int JS_VALUE_GET_TAG(JSValue v) { return (int)(v >> 32); }
static inline int JS_VALUE_GET_INT(JSValue v) { return (int)(uint32_t)v; }
static inline int JS_VALUE_GET_BOOL(JSValue v){ return (int)(uint32_t)v; }
static inline double JS_VALUE_GET_FLOAT64(JSValue v){
    union { uint64_t u; double d; } u; u.u = v; return u.d;
}
static inline JSValue JS_MKINT(int v){ return ((uint64_t)JS_TAG_INT<<32)|(uint32_t)v; }
static inline JSValue JS_MKFLOAT(double d){ union{uint64_t u;double f;}u; u.f=d; return u.u; }
static inline JSValue JS_MKBOOL(int b){ return b?JS_TRUE:JS_FALSE; }

#define JS_IsUndefined(v) (JS_VALUE_GET_TAG(v)==JS_TAG_UNDEFINED)
#define JS_IsNull(v)      (JS_VALUE_GET_TAG(v)==JS_TAG_NULL)
#define JS_IsException(v) (JS_VALUE_GET_TAG(v)==JS_TAG_EXCEPTION)
#define JS_IsBool(v)      (JS_VALUE_GET_TAG(v)==JS_TAG_BOOL)
#define JS_IsInt(v)       (JS_VALUE_GET_TAG(v)==JS_TAG_INT)
#define JS_IsFloat(v)     (JS_VALUE_GET_TAG(v)==JS_TAG_FLOAT64)
#define JS_IsString(v)    (JS_VALUE_GET_TAG(v)==JS_TAG_STRING)
#define JS_IsObject(v)    (JS_VALUE_GET_TAG(v)==JS_TAG_OBJECT)

/* ------------------------------------------------------------------ */
/*  C function callback type                                           */
/* ------------------------------------------------------------------ */
typedef JSValue JSCFunction(JSContext *ctx, JSValue this_val, int argc, JSValue *argv);

/* ------------------------------------------------------------------ */
/*  Property flags                                                     */
/* ------------------------------------------------------------------ */
#define JS_PROP_WRITABLE    (1<<0)
#define JS_PROP_ENUMERABLE  (1<<1)
#define JS_PROP_CONFIGURABLE (1<<2)
#define JS_PROP_C_W_E       (JS_PROP_WRITABLE|JS_PROP_ENUMERABLE|JS_PROP_CONFIGURABLE)

/* ------------------------------------------------------------------ */
/*  Runtime / Context lifecycle                                        */
/* ------------------------------------------------------------------ */
JSRuntime *JS_NewRuntime(void);
void       JS_FreeRuntime(JSRuntime *rt);

JSContext *JS_NewContext(JSRuntime *rt);
void       JS_FreeContext(JSContext *ctx);

/* Set memory limit (bytes) */
void JS_SetMemoryLimit(JSRuntime *rt, size_t limit);

/* ------------------------------------------------------------------ */
/*  Eval flags                                                         */
/* ------------------------------------------------------------------ */
#define JS_EVAL_TYPE_GLOBAL  0
#define JS_EVAL_TYPE_MODULE  1
#define JS_EVAL_FLAG_STRICT  (1<<3)

/* Evaluate a JavaScript string */
JSValue JS_Eval(JSContext *ctx, const char *input, size_t input_len,
                const char *filename, int eval_flags);

/* ------------------------------------------------------------------ */
/*  Value management                                                   */
/* ------------------------------------------------------------------ */
/* Reference counting */
void    JS_FreeValue(JSContext *ctx, JSValue v);
JSValue JS_DupValue(JSContext *ctx, JSValue v);

/* ------------------------------------------------------------------ */
/*  Type conversion                                                    */
/* ------------------------------------------------------------------ */
int         JS_ToInt32(JSContext *ctx, int *pres, JSValue val);
int         JS_ToFloat64(JSContext *ctx, double *pres, JSValue val);
const char *JS_ToCString(JSContext *ctx, JSValue val);
void        JS_FreeCString(JSContext *ctx, const char *ptr);
JSValue     JS_ToString(JSContext *ctx, JSValue val);
JSValue     JS_NewString(JSContext *ctx, const char *str);
JSValue     JS_NewStringLen(JSContext *ctx, const char *str, size_t len);
JSValue     JS_NewInt32(JSContext *ctx, int32_t val);
JSValue     JS_NewFloat64(JSContext *ctx, double val);
JSValue     JS_NewBool(JSContext *ctx, int val);

/* ------------------------------------------------------------------ */
/*  Objects                                                            */
/* ------------------------------------------------------------------ */
JSValue  JS_NewObject(JSContext *ctx);
JSValue  JS_NewArray(JSContext *ctx);
int      JS_SetPropertyStr(JSContext *ctx, JSValue obj, const char *name, JSValue val);
JSValue  JS_GetPropertyStr(JSContext *ctx, JSValue obj, const char *name);
int      JS_SetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val);
JSValue  JS_GetPropertyUint32(JSContext *ctx, JSValue obj, uint32_t idx);
int      JS_DefinePropertyValueStr(JSContext *ctx, JSValue obj, const char *name,
                                   JSValue val, int flags);

/* ------------------------------------------------------------------ */
/*  C function binding                                                 */
/* ------------------------------------------------------------------ */
JSValue  JS_NewCFunction(JSContext *ctx, JSCFunction *func,
                         const char *name, int length);

/* ------------------------------------------------------------------ */
/*  Global object                                                      */
/* ------------------------------------------------------------------ */
JSValue  JS_GetGlobalObject(JSContext *ctx);
int      JS_SetGlobal(JSContext *ctx, const char *name, JSValue val);

/* ------------------------------------------------------------------ */
/*  Exception handling                                                 */
/* ------------------------------------------------------------------ */
JSValue JS_GetException(JSContext *ctx);
void    JS_ResetUncatchableError(JSContext *ctx);
int     JS_IsError(JSContext *ctx, JSValue val);
JSValue JS_NewError(JSContext *ctx);
void    JS_Throw(JSContext *ctx, JSValue val);
JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...);
JSValue JS_ThrowRangeError(JSContext *ctx, const char *fmt, ...);

/* ------------------------------------------------------------------ */
/*  JSON                                                               */
/* ------------------------------------------------------------------ */
JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len,
                     const char *filename);
JSValue JS_JSONStringify(JSContext *ctx, JSValue obj,
                         JSValue replacer, JSValue space);

/* ------------------------------------------------------------------ */
/*  Console callback (set by browser to capture output)               */
/* ------------------------------------------------------------------ */
typedef void (*js_console_fn)(const char *line, int is_error);
void JS_SetConsoleCallback(JSContext *ctx, js_console_fn fn);

/* ------------------------------------------------------------------ */
/*  document.write callback (set by browser to inject HTML)           */
/* ------------------------------------------------------------------ */
typedef void (*js_docwrite_fn)(const char *html);
void JS_SetDocWriteCallback(JSContext *ctx, js_docwrite_fn fn);
