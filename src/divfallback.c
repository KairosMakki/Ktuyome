/* 64-bit division helpers. Weak, so a real 32-bit libgcc (gcc-multilib) wins when present;
   lets the kernel link even on toolchains that only ship the 64-bit libgcc. */
typedef unsigned long long u64_t;
typedef long long s64_t;

__attribute__((weak)) u64_t __udivmoddi4(u64_t n, u64_t d, u64_t *rem) {
    u64_t q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~0ull; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ull << i; }
    }
    if (rem) *rem = r;
    return q;
}
__attribute__((weak)) u64_t __udivdi3(u64_t n, u64_t d) { return __udivmoddi4(n, d, 0); }
__attribute__((weak)) u64_t __umoddi3(u64_t n, u64_t d) { u64_t r; __udivmoddi4(n, d, &r); return r; }
__attribute__((weak)) s64_t __divdi3(s64_t n, s64_t d) {
    int neg = (n < 0) ^ (d < 0);
    u64_t q = __udivmoddi4(n < 0 ? -(u64_t)n : (u64_t)n, d < 0 ? -(u64_t)d : (u64_t)d, 0);
    return neg ? -(s64_t)q : (s64_t)q;
}
__attribute__((weak)) s64_t __moddi3(s64_t n, s64_t d) {
    u64_t r;
    __udivmoddi4(n < 0 ? -(u64_t)n : (u64_t)n, d < 0 ? -(u64_t)d : (u64_t)d, &r);
    return n < 0 ? -(s64_t)r : (s64_t)r;
}
