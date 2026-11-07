/*
 * ktuyome_compat.h  —  libc compatibility shims for BearSSL and QuickJS
 *
 * This header is included at the top of every vendor translation unit.
 * It maps standard C library symbols onto ktuyome's own implementations
 * and provides the handful of functions the libraries need that ktuyome
 * does not otherwise supply (setjmp/longjmp, strtod, qsort, …).
 *
 * Compile-time constraints inherited from the kernel build:
 *   -m32  -ffreestanding  -fno-builtin  -mno-sse  -mno-mmx  -mno-sse2
 *   -mfpmath=387  -fno-stack-protector  -O2  -std=gnu11
 */
#pragma once
#include "../ktuyome.h"   /* u8/u16/u32/u64, memcpy, memset, etc. */

/* ------------------------------------------------------------------ */
/*  Sized integer aliases expected by many C libraries                 */
/* ------------------------------------------------------------------ */
typedef u8  uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef s8  int8_t;
typedef s16 int16_t;
typedef s32 int32_t;
typedef unsigned int  size_t;
typedef int           ssize_t;
typedef int           ptrdiff_t;
typedef unsigned int  uintptr_t;
typedef int           intptr_t;
typedef unsigned long long uint64_t_check; /* sanity */

#define SIZE_MAX  0xFFFFFFFFU
#define INT_MAX   0x7FFFFFFF
#define INT_MIN   (-0x7FFFFFFF - 1)
#define UINT_MAX  0xFFFFFFFFU
#define LONG_MAX  0x7FFFFFFF
#define ULONG_MAX 0xFFFFFFFFU

/* ------------------------------------------------------------------ */
/*  Heap allocator (implemented in kmalloc.c)                          */
/* ------------------------------------------------------------------ */
void *kmalloc(size_t n);
void *krealloc(void *p, size_t n);
void  kfree(void *p);
void *kcalloc(size_t nmemb, size_t sz);

/* Redirect standard names used by vendor code */
#define malloc(n)        kmalloc(n)
#define free(p)          kfree(p)
#define realloc(p,n)     krealloc(p,n)
#define calloc(nm,sz)    kcalloc(nm,sz)

/* ------------------------------------------------------------------ */
/*  String / memory extras                                             */
/* ------------------------------------------------------------------ */
/* strdup */
static inline char *strdup(const char *s) {
    u32 n = strlen(s) + 1;
    char *p = (char *)kmalloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* strndup */
static inline char *strndup(const char *s, size_t maxn) {
    size_t n = 0; while (n < maxn && s[n]) n++;
    char *p = (char *)kmalloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = 0; }
    return p;
}

/* strchr */
static inline char *strchr(const char *s, int c) {
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return (unsigned char)c == 0 ? (char *)s : (char *)0;
}

/* strncmp */
static inline int strncmp(const char *a, const char *b, size_t n) {
    for (; n && *a && *a == *b; a++, b++, n--);
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

/* strncpy  (NB: does NOT guarantee NUL termination — match C standard) */
static inline char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

/* strnlen */
static inline size_t strnlen(const char *s, size_t maxn) {
    size_t n = 0; while (n < maxn && s[n]) n++; return n;
}

/* strrchr */
static inline char *strrchr(const char *s, int c) {
    const char *last = (char *)0;
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) last = s;
    if ((unsigned char)c == 0) return (char *)s;
    return (char *)last;
}

/* strstr */
static inline char *strstr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return (char *)0;
}

/* strcpy — just an alias; ktuyome has kstrcpy (bounded), this is unbounded */
static inline char *strcpy(char *dst, const char *src) {
    char *d = dst; while ((*d++ = *src++)); return dst;
}

/* strcat */
static inline char *strcat(char *dst, const char *src) {
    char *d = dst; while (*d) d++; while ((*d++ = *src++)); return dst;
}

/* sprintf / snprintf — tiny subset (d, u, s, c, x, X, p, %) */
int kt_snprintf(char *buf, size_t cap, const char *fmt, ...);
int kt_sprintf(char *buf, const char *fmt, ...);
#define snprintf kt_snprintf
#define sprintf  kt_sprintf

/* vsnprintf */
int kt_vsnprintf(char *buf, size_t cap, const char *fmt, __builtin_va_list ap);
#define vsnprintf kt_vsnprintf

/* stdio stubs — no actual I/O; used only for error paths in vendor code */
typedef int FILE;
#define stderr ((FILE*)2)
#define stdout ((FILE*)1)
#define stdin  ((FILE*)0)
static inline int fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
static inline int printf(const char *fmt, ...) { (void)fmt; return 0; }
static inline int fputs(const char *s, FILE *f) { (void)s; (void)f; return 0; }
static inline int fputc(int c, FILE *f) { (void)c; (void)f; return 0; }
static inline int fflush(FILE *f) { (void)f; return 0; }
static inline void perror(const char *s) { (void)s; }
static inline int puts(const char *s) { (void)s; return 0; }
#define abort() do { while(1) __asm__("hlt"); } while(0)
#define exit(c)  abort()

/* ------------------------------------------------------------------ */
/*  errno (single-threaded: global variable)                           */
/* ------------------------------------------------------------------ */
extern int kt_errno;
#define errno kt_errno
#define ENOMEM   12
#define EINVAL   22
#define ENOSYS   38
#define ENOENT    2
#define ERANGE   34
#define EILSEQ   84
#define EOVERFLOW 75

/* ------------------------------------------------------------------ */
/*  assert                                                             */
/* ------------------------------------------------------------------ */
#define assert(x) do { if (!(x)) { panic_screen("assert failed", (u32)__LINE__, 0); } } while(0)
#define NDEBUG 1   /* disable assertions in vendor code for size/speed */

/* ------------------------------------------------------------------ */
/*  setjmp / longjmp  (x86-32 via inline asm)                         */
/*  jmp_buf layout: [ebx, esi, edi, ebp, esp, eip]  (6 × u32)        */
/* ------------------------------------------------------------------ */
typedef u32 jmp_buf[6];

static inline int __attribute__((returns_twice)) setjmp(jmp_buf env) {
    int ret;
    __asm__ volatile (
        "movl %%ebx, 0(%1)\n"
        "movl %%esi, 4(%1)\n"
        "movl %%edi, 8(%1)\n"
        "movl %%ebp,12(%1)\n"
        "movl %%esp,16(%1)\n"
        "movl $1f, 20(%1)\n"  /* save return address */
        "movl $0,%0\n"
        "jmp 2f\n"
        "1: movl $1,%0\n"
        "2:\n"
        : "=r"(ret)
        : "r"(env)
        : "memory"
    );
    return ret;
}

static inline __attribute__((noreturn)) void longjmp(jmp_buf env, int val) {
    (void)val;
    __asm__ volatile (
        "movl 0(%0),%%ebx\n"
        "movl 4(%0),%%esi\n"
        "movl 8(%0),%%edi\n"
        "movl 12(%0),%%ebp\n"
        "movl 16(%0),%%esp\n"
        "jmp *20(%0)\n"
        :
        : "r"(env)
        :
    );
    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/*  strtod / strtol / strtoul / strtoll / strtoull                    */
/* ------------------------------------------------------------------ */
double kt_strtod(const char *s, char **endptr);
long   kt_strtol(const char *s, char **endptr, int base);
unsigned long kt_strtoul(const char *s, char **endptr, int base);
long long kt_strtoll(const char *s, char **endptr, int base);
unsigned long long kt_strtoull(const char *s, char **endptr, int base);
#define strtod(s,e)   kt_strtod(s,e)
#define strtol(s,e,b) kt_strtol(s,e,b)
#define strtoul(s,e,b) kt_strtoul(s,e,b)
#define strtoll(s,e,b)  kt_strtoll(s,e,b)
#define strtoull(s,e,b) kt_strtoull(s,e,b)
#define atoi(s)  katoi(s)
#define atof(s)  kt_strtod(s,(char**)0)
#define atol(s)  ((long)katoi(s))

/* ------------------------------------------------------------------ */
/*  qsort / bsearch                                                    */
/* ------------------------------------------------------------------ */
void kt_qsort(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void *kt_bsearch(const void *key, const void *base, size_t nmemb, size_t size,
                 int (*compar)(const void *, const void *));
#define qsort   kt_qsort
#define bsearch kt_bsearch

/* ------------------------------------------------------------------ */
/*  Math functions needed by BearSSL / QuickJS                        */
/* ------------------------------------------------------------------ */
/* Use x87 for everything — -mno-sse is mandatory */
#define NAN     (0.0/0.0)
#define INFINITY (1.0/0.0)
#define HUGE_VAL INFINITY

static inline double kt_fabs(double x)  { double r; __asm__("fabs" : "=t"(r) : "0"(x)); return r; }
static inline double kt_floor(double x) { return d_floor(x); }
static inline double kt_ceil(double x)  { double r = d_floor(x); return r < x ? r + 1.0 : r; }
static inline double kt_sqrt(double x)  { return d_sqrt(x); }
static inline double kt_pow(double x, double y);   /* forward — defined in kcompat.c */
static inline double kt_log(double x)   { double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x)); return r; }
static inline double kt_log2(double x)  { double r; __asm__("fld1; fxch; fyl2x"  : "=t"(r) : "0"(x)); return r; }
static inline double kt_exp(double x) {
    if (x > 700.0) return INFINITY;
    if (x < -700.0) return 0.0;
    /* e^x = 2^(x * log2(e)) */
    double t = x * 1.4426950408889634; /* x * log2(e) */
    int n = (int)kt_floor(t);
    double f = t - (double)n;
    /* 2^f via Taylor series of e^(f*ln2) */
    double fl = f * 0.6931471805599453;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 20; i++) { term *= fl / (double)i; sum += term; }
    /* scale by 2^n */
    if (n >= 0) { for (int i = 0; i < n && i < 1024; i++) sum *= 2.0; }
    else        { for (int i = 0; i < -n && i < 1024; i++) sum *= 0.5; }
    return sum;
}
static inline double kt_sin(double x)   { double r; __asm__("fsin"  : "=t"(r) : "0"(x)); return r; }
static inline double kt_cos(double x)   { double r; __asm__("fcos"  : "=t"(r) : "0"(x)); return r; }
static inline double kt_tan(double x)   { double r; __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"(x) : "st"); return r; }
static inline double kt_atan(double x)  { double r; __asm__("fld1; fpatan" : "=t"(r) : "0"(x)); return r; }
static inline double kt_atan2(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y)); return r; }
static inline double kt_asin(double x)  { return kt_atan2(x, kt_sqrt(1.0 - x*x)); }
static inline double kt_acos(double x)  { return kt_atan2(kt_sqrt(1.0 - x*x), x); }
static inline double kt_trunc(double x) { double r; __asm__("frndint" : "=t"(r) : "0"(x)); return r; }
static inline double kt_round(double x) { return x >= 0 ? kt_floor(x + 0.5) : kt_ceil(x - 0.5); }
static inline double kt_fmod(double x, double y) {
    if (y == 0.0) return NAN;
    double q = x / y;
    int n = (int)q;
    return x - (double)n * y;
}
static inline int    kt_isnan(double x)  { return x != x; }
static inline int    kt_isinf(double x)  { return x == x && (x - x) != 0.0; }
static inline int    kt_isfinite(double x){ return !kt_isnan(x) && !kt_isinf(x); }
static inline double kt_ldexp(double x, int exp) { double r; __asm__("fscale" : "=t"(r) : "0"(x), "u"((double)exp)); return r; }
static inline double kt_frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    int e = (int)kt_floor(kt_log2(kt_fabs(x))) + 1;
    *exp = e;
    return x * kt_ldexp(1.0, -e);
}

/* pow via exp/log */
static inline double kt_pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;
    if (x < 0.0) {
        /* integer exponents only for negative base */
        long long iy = (long long)y;
        if ((double)iy != y) return 0.0/0.0; /* NaN */
        double r = kt_exp(y * kt_log(-x));
        return (iy & 1) ? -r : r;
    }
    return kt_exp(y * kt_log(x));
}

/* Redirect math names */
#define fabs(x)    kt_fabs(x)
#define floor(x)   kt_floor(x)
#define ceil(x)    kt_ceil(x)
#define sqrt(x)    kt_sqrt(x)
#define pow(x,y)   kt_pow(x,y)
#define log(x)     kt_log(x)
#define log2(x)    kt_log2(x)
#define exp(x)     kt_exp(x)
#define sin(x)     kt_sin(x)
#define cos(x)     kt_cos(x)
#define tan(x)     kt_tan(x)
#define atan(x)    kt_atan(x)
#define atan2(y,x) kt_atan2(y,x)
#define asin(x)    kt_asin(x)
#define acos(x)    kt_acos(x)
#define trunc(x)   kt_trunc(x)
#define round(x)   kt_round(x)
#define fmod(x,y)  kt_fmod(x,y)
#define isnan(x)   kt_isnan(x)
#define isinf(x)   kt_isinf(x)
#define isfinite(x) kt_isfinite(x)
#define ldexp(x,e)  kt_ldexp(x,e)
#define frexp(x,e)  kt_frexp(x,e)

#define M_PI    3.14159265358979323846
#define M_E     2.71828182845904523536
#define M_LN2   0.69314718055994530942
#define M_LOG2E 1.44269504088896340736

/* ------------------------------------------------------------------ */
/*  time / clock stubs (BearSSL needs wall clock for cert validation)  */
/* ------------------------------------------------------------------ */
typedef u32 time_t;
typedef u32 clock_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
/* Returns seconds since 2000-01-01 00:00:00 UTC (good enough for cert expiry) */
static inline time_t time(time_t *t) {
    int h, m, s, day, mon, year;
    rtc_read(&h, &m, &s, &day, &mon, &year);
    /* days since 2000 */
    int y = year - 2000;
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int yday = 0; for (int i = 0; i < mon - 1; i++) yday += mdays[i]; yday += day - 1;
    if (leap && mon > 2) yday++;
    int days = y*365 + (y+3)/4 + yday;
    time_t ts = (time_t)(days * 86400 + h*3600 + m*60 + s);
    if (t) *t = ts;
    return ts;
}
#define clock() ((clock_t)ticks())
#define CLOCKS_PER_SEC 1000

struct timeval  { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
static inline int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (tv) { tv->tv_sec = (long)time((time_t*)0); tv->tv_usec = (long)(ticks() % 1000) * 1000; }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Random number generator  (xorshift32 seeded from rdtsc)           */
/* ------------------------------------------------------------------ */
extern u32 kt_rand_state;
static inline void kt_srand(u32 seed) { kt_rand_state = seed ? seed : 1; }
static inline u32  kt_rand(void) {
    kt_rand_state ^= kt_rand_state << 13;
    kt_rand_state ^= kt_rand_state >> 17;
    kt_rand_state ^= kt_rand_state << 5;
    return kt_rand_state;
}
#define rand()  ((int)(kt_rand() & 0x7FFFFFFF))
#define srand(s) kt_srand((u32)(s))
#define RAND_MAX 0x7FFFFFFF

/* ------------------------------------------------------------------ */
/*  Misc                                                               */
/* ------------------------------------------------------------------ */
#define offsetof(T,m) __builtin_offsetof(T,m)
#define static_assert(e,m) _Static_assert(e,m)
#define va_list  __builtin_va_list
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

/* ctype */
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isxdigit(int c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); }
static inline int isalpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static inline int isalnum(int c) { return isalpha(c)||isdigit(c); }
static inline int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static inline int isupper(int c) { return c>='A'&&c<='Z'; }
static inline int islower(int c) { return c>='a'&&c<='z'; }
static inline int toupper(int c) { return islower(c)?c-32:c; }
static inline int tolower(int c) { return isupper(c)?c+32:c; }
static inline int iscntrl(int c) { return (unsigned)c < 32 || c == 127; }
static inline int isprint(int c) { return c >= 32 && c < 127; }
static inline int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }

/* limits.h subset */
#define CHAR_BIT  8
#define UCHAR_MAX 255
#define SCHAR_MAX 127
#define SCHAR_MIN (-128)

/* Suppress some gcc warnings common in vendor code */
#define __unused __attribute__((unused))
#define __packed __attribute__((packed))
