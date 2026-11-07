#include "ktuyome.h"

void *memcpy(void *d, const void *s, u32 n) {
    void *r = d;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s) : "c"(n >> 2) : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s) : "c"(n & 3) : "memory");
    return r;
}
void *memmove(void *d, const void *s, u32 n) {
    u8 *a = d; const u8 *b = s;
    if (a < b || a >= b + n) return memcpy(d, s, n);
    while (n--) a[n] = b[n];
    return d;
}
void *memset(void *d, int c, u32 n) {
    void *r = d; u32 v = (u8)c; v |= v << 8; v |= v << 16;
    __asm__ volatile("rep stosl" : "+D"(d) : "a"(v), "c"(n >> 2) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d) : "a"(v), "c"(n & 3) : "memory");
    return r;
}
int memcmp(const void *a, const void *b, u32 n) {
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
u32 strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
void kstrcpy(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
int kstrcat(char *dst, const char *src, int cap) {
    int n = (int)strlen(dst);
    kstrcpy(dst + n, src, cap - n);
    return (int)strlen(dst);
}
int kitoa(char *out, int v) {
    char t[12]; int n = 0, i = 0;
    unsigned u = v < 0 ? -(unsigned)v : (unsigned)v;
    if (v < 0) out[i++] = '-';
    do { t[n++] = '0' + u % 10; u /= 10; } while (u);
    while (n) out[i++] = t[--n];
    out[i] = 0;
    return i;
}
int katoi(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
int starts_with(const char *s, const char *p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}
int ends_with(const char *s, const char *suf) {
    int a = (int)strlen(s), b = (int)strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

/* x87 math (no libm in a freestanding kernel) */
float k_sqrt(float x) { float r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
float k_sin(float x)  { float r; __asm__("fsin"  : "=t"(r) : "0"(x)); return r; }
float k_cos(float x)  { float r; __asm__("fcos"  : "=t"(r) : "0"(x)); return r; }
float k_atan2(float y, float x) { float r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
float k_floor(float x) { int i = (int)x; return (float)((float)i > x ? i - 1 : i); }
float k_ceil(float x)  { int i = (int)x; return (float)((float)i < x ? i + 1 : i); }
double d_floor(double x) { long long i = (long long)x; double r = (double)i; return r > x ? r - 1 : r; }
double d_sqrt(double x) { double r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
