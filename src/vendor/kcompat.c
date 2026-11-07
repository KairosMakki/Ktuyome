/*
 * kcompat.c  —  implementations of libc functions declared in ktuyome_compat.h
 *
 * Compiled with the same flags as the rest of the kernel:
 *   -m32 -ffreestanding -fno-builtin -mno-sse -O2 -std=gnu11
 */
#include "ktuyome_compat.h"

/* ------------------------------------------------------------------ */
/*  Globals                                                            */
/* ------------------------------------------------------------------ */
int kt_errno = 0;
u32 kt_rand_state = 0x12345678;

/* ------------------------------------------------------------------ */
/*  kt_vsnprintf — minimal printf formatting                           */
/*  Supports: %d %i %u %x %X %o %s %c %p %%  with width/precision   */
/* ------------------------------------------------------------------ */
static void _out(char *buf, size_t cap, size_t *pos, char c) {
    if (*pos < cap - 1) buf[*pos] = c;
    (*pos)++;
}

static const char _hd[] = "0123456789abcdef";
static const char _hD[] = "0123456789ABCDEF";

static void _out_uint(char *buf, size_t cap, size_t *pos,
                      unsigned long long v, int base, int upper,
                      int width, int pad0, int sign_c) {
    char tmp[24]; int n = 0;
    const char *h = upper ? _hD : _hd;
    if (v == 0) { tmp[n++] = '0'; }
    else { unsigned long long x = v; while (x) { tmp[n++] = h[x % (unsigned)base]; x /= (unsigned)base; } }
    if (sign_c) tmp[n++] = (char)sign_c;
    /* reverse */
    for (int i = 0; i < n/2; i++) { char t = tmp[i]; tmp[i] = tmp[n-1-i]; tmp[n-1-i] = t; }
    int pad = width - n;
    if (!pad0) while (pad-- > 0) _out(buf,cap,pos,' ');
    if (sign_c && pad0) _out(buf,cap,pos,(char)sign_c);
    if (pad0) while (pad-- > 0) _out(buf,cap,pos,'0');
    for (int i = 0; i < n; i++) _out(buf,cap,pos,tmp[i]);
}

int kt_vsnprintf(char *buf, size_t cap, const char *fmt, __builtin_va_list ap) {
    if (!buf || cap == 0) return 0;
    size_t pos = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { _out(buf,cap,&pos,*fmt); continue; }
        fmt++;
        if (*fmt == '%') { _out(buf,cap,&pos,'%'); continue; }
        /* flags */
        int pad0 = 0, left = 0, plus = 0, space = 0, alt = 0;
        for (;;) {
            if (*fmt == '0') { pad0 = 1; fmt++; }
            else if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else if (*fmt == '#') { alt = 1; fmt++; }
            else break;
        }
        (void)alt; (void)plus; (void)space; (void)left;
        /* width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width*10 + (*fmt++ - '0');
        /* precision */
        int prec = -1;
        if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec*10 + (*fmt++ - '0'); }
        /* length modifier */
        int lng = 0;
        if (*fmt == 'l') { lng++; fmt++; if (*fmt == 'l') { lng++; fmt++; } }
        else if (*fmt == 'z') { fmt++; }

        char spec = *fmt;
        if (spec == 'd' || spec == 'i') {
            long long v;
            if (lng >= 2) v = (long long)__builtin_va_arg(ap, long long);
            else if (lng == 1) v = (long long)__builtin_va_arg(ap, long);
            else v = (long long)__builtin_va_arg(ap, int);
            int sc = v < 0 ? '-' : 0;
            unsigned long long uv = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
            _out_uint(buf,cap,&pos,uv,10,0,width,pad0,sc);
        } else if (spec == 'u') {
            unsigned long long v;
            if (lng >= 2) v = __builtin_va_arg(ap, unsigned long long);
            else if (lng == 1) v = (unsigned long long)__builtin_va_arg(ap, unsigned long);
            else v = (unsigned long long)__builtin_va_arg(ap, unsigned int);
            _out_uint(buf,cap,&pos,v,10,0,width,pad0,0);
        } else if (spec == 'x' || spec == 'X') {
            unsigned long long v;
            if (lng >= 2) v = __builtin_va_arg(ap, unsigned long long);
            else if (lng == 1) v = (unsigned long long)__builtin_va_arg(ap, unsigned long);
            else v = (unsigned long long)__builtin_va_arg(ap, unsigned int);
            if (alt && v) { _out(buf,cap,&pos,'0'); _out(buf,cap,&pos,spec=='x'?'x':'X'); }
            _out_uint(buf,cap,&pos,v,16,spec=='X',width,pad0,0);
        } else if (spec == 'o') {
            unsigned long long v = (unsigned long long)__builtin_va_arg(ap, unsigned int);
            _out_uint(buf,cap,&pos,v,8,0,width,pad0,0);
        } else if (spec == 'p') {
            unsigned long long v = (unsigned long long)(uintptr_t)__builtin_va_arg(ap, void*);
            _out(buf,cap,&pos,'0'); _out(buf,cap,&pos,'x');
            _out_uint(buf,cap,&pos,v,16,0,8,1,0);
        } else if (spec == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0; while (s[slen]) slen++;
            if (prec >= 0 && prec < slen) slen = prec;
            int pad = width - slen;
            if (!left) while (pad-- > 0) _out(buf,cap,&pos,' ');
            for (int i = 0; i < slen; i++) _out(buf,cap,&pos,s[i]);
            if (left)  while (pad-- > 0) _out(buf,cap,&pos,' ');
        } else if (spec == 'c') {
            char c = (char)__builtin_va_arg(ap, int);
            _out(buf,cap,&pos,c);
        } else if (spec == 'f' || spec == 'g' || spec == 'e') {
            /* minimal float: just print as integer for now */
            double v = __builtin_va_arg(ap, double);
            int sc = v < 0 ? '-' : 0; if (v < 0) v = -v;
            long long iv = (long long)v;
            _out_uint(buf,cap,&pos,(unsigned long long)iv,10,0,0,0,sc);
            _out(buf,cap,&pos,'.');
            int dp = prec >= 0 ? prec : 6;
            for (int i = 0; i < dp; i++) { v = (v - (double)(long long)v) * 10.0; _out(buf,cap,&pos,'0'+(int)v); }
        } else {
            _out(buf,cap,&pos,'%');
            _out(buf,cap,&pos,spec);
        }
    }
    buf[pos < cap ? pos : cap-1] = 0;
    return (int)pos;
}

int kt_snprintf(char *buf, size_t cap, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int r = kt_vsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int kt_sprintf(char *buf, const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap,fmt);
    int r = kt_vsnprintf(buf, 65536, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/*  strtoul / strtol / strtoll / strtoull                             */
/* ------------------------------------------------------------------ */
unsigned long kt_strtoul(const char *s, char **ep, int base) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '+') s++; else if (*s == '-') { neg = 1; s++; }
    if (base == 0) {
        if (s[0]=='0'&&(s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (s[0]=='0') { base=8; s++; }
        else base=10;
    } else if (base==16 && s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s+=2;
    unsigned long v = 0;
    while (*s) {
        int d;
        if (*s>='0'&&*s<='9') d=*s-'0';
        else if (*s>='a'&&*s<='z') d=*s-'a'+10;
        else if (*s>='A'&&*s<='Z') d=*s-'A'+10;
        else break;
        if (d >= base) break;
        v = v*(unsigned)base + (unsigned)d; s++;
    }
    if (ep) *ep = (char*)s;
    return neg ? (unsigned long)(-(long)v) : v;
}
long kt_strtol(const char *s, char **ep, int base) { return (long)kt_strtoul(s,ep,base); }

unsigned long long kt_strtoull(const char *s, char **ep, int base) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '+') s++; else if (*s == '-') { neg = 1; s++; }
    if (base == 0) {
        if (s[0]=='0'&&(s[1]=='x'||s[1]=='X')) { base=16; s+=2; }
        else if (s[0]=='0') { base=8; s++; }
        else base=10;
    } else if (base==16 && s[0]=='0'&&(s[1]=='x'||s[1]=='X')) s+=2;
    unsigned long long v = 0;
    while (*s) {
        int d;
        if (*s>='0'&&*s<='9') d=*s-'0';
        else if (*s>='a'&&*s<='z') d=*s-'a'+10;
        else if (*s>='A'&&*s<='Z') d=*s-'A'+10;
        else break;
        if (d >= base) break;
        v = v*(unsigned long long)base + (unsigned long long)d; s++;
    }
    if (ep) *ep = (char*)s;
    return neg ? (unsigned long long)(-(long long)v) : v;
}
long long kt_strtoll(const char *s, char **ep, int base) { return (long long)kt_strtoull(s,ep,base); }

/* ------------------------------------------------------------------ */
/*  strtod — convert ASCII decimal/hex to double via x87              */
/* ------------------------------------------------------------------ */
double kt_strtod(const char *s, char **ep) {
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s=='-') { neg=1; s++; } else if (*s=='+') s++;
    /* check for inf/nan */
    if ((s[0]=='i'||s[0]=='I') && (s[1]=='n'||s[1]=='N') && (s[2]=='f'||s[2]=='F')) {
        if (ep) *ep = (char*)s+3; return neg ? -INFINITY : INFINITY;
    }
    if ((s[0]=='n'||s[0]=='N') && (s[1]=='a'||s[1]=='A') && (s[2]=='n'||s[2]=='N')) {
        if (ep) *ep = (char*)s+3; return NAN;
    }
    /* hex float 0x... */
    int ishex = (s[0]=='0'&&(s[1]=='x'||s[1]=='X'));
    if (ishex) s += 2;
    const char *start = s;
    double v = 0.0, frac = 1.0; int has_dot = 0;
    while (*s) {
        int d;
        if (*s>='0'&&*s<='9') d=*s-'0';
        else if (ishex&&*s>='a'&&*s<='f') d=*s-'a'+10;
        else if (ishex&&*s>='A'&&*s<='F') d=*s-'A'+10;
        else if (*s=='.'&&!has_dot) { has_dot=1; s++; continue; }
        else break;
        if (!has_dot) v = v*(ishex?16.0:10.0) + d;
        else { frac /= (ishex?16.0:10.0); v += d * frac; }
        s++;
    }
    if (s == start) { if (ep) *ep=(char*)s; return neg?-0.0:0.0; }
    /* exponent */
    if ((!ishex&&(*s=='e'||*s=='E'))||(ishex&&(*s=='p'||*s=='P'))) {
        s++;
        int eneg = 0;
        if (*s=='-') { eneg=1; s++; } else if (*s=='+') s++;
        int ex = 0; while (*s>='0'&&*s<='9') ex=ex*10+(*s++-'0');
        double base = ishex ? 2.0 : 10.0;
        while (ex-- > 0) { if (eneg) v/=base; else v*=base; }
    }
    if (ep) *ep = (char*)s;
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ */
/*  qsort — introsort-lite (insertion sort for small n, heapsort)     */
/* ------------------------------------------------------------------ */
typedef int (*cmp_fn)(const void *, const void *);

static void _swap(char *a, char *b, size_t sz) {
    char tmp[64];
    while (sz > 0) {
        size_t n = sz < sizeof tmp ? sz : sizeof tmp;
        memcpy(tmp, a, n); memcpy(a, b, n); memcpy(b, tmp, n);
        a += n; b += n; sz -= n;
    }
}

static void _isort(char *base, size_t n, size_t sz, cmp_fn cmp) {
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        while (j > 0 && cmp(base+(j-1)*sz, base+j*sz) > 0) {
            _swap(base+(j-1)*sz, base+j*sz, sz); j--;
        }
    }
}

static void _heapify(char *base, size_t n, size_t i, size_t sz, cmp_fn cmp) {
    while (1) {
        size_t largest = i, l = 2*i+1, r = 2*i+2;
        if (l < n && cmp(base+largest*sz, base+l*sz) < 0) largest = l;
        if (r < n && cmp(base+largest*sz, base+r*sz) < 0) largest = r;
        if (largest == i) break;
        _swap(base+i*sz, base+largest*sz, sz); i = largest;
    }
}

static void _heapsort(char *base, size_t n, size_t sz, cmp_fn cmp) {
    for (size_t i = n/2; i > 0; ) { i--; _heapify(base,n,i,sz,cmp); }
    for (size_t i = n-1; i > 0; i--) { _swap(base, base+i*sz, sz); _heapify(base,i,0,sz,cmp); }
}

void kt_qsort(void *base, size_t n, size_t sz, cmp_fn cmp) {
    if (n < 2) return;
    if (n <= 16) { _isort((char*)base, n, sz, cmp); return; }
    _heapsort((char*)base, n, sz, cmp);
}

void *kt_bsearch(const void *key, const void *base, size_t n, size_t sz, cmp_fn cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi-lo)/2;
        int c = cmp(key, (const char*)base + mid*sz);
        if (c == 0) return (void*)((const char*)base + mid*sz);
        if (c < 0) hi = mid; else lo = mid+1;
    }
    return (void*)0;
}
