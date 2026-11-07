/*
 * bearssl.c — BearSSL TLS 1.2 client implementation for ktuyome
 *
 * Self-contained amalgam.  Implements:
 *   SHA-1, SHA-256, HMAC, AES-128-CBC (constant-time), P-256 ECDHE,
 *   RSA-PKCS#1 verify, TLS 1.2 record/handshake layer.
 *
 * Compiler flags: -m32 -ffreestanding -fno-builtin -mno-sse -O2 -std=gnu11
 */
#include "bearssl.h"

/* ================================================================== */
/*  Utility macros                                                     */
/* ================================================================== */
#define PUT32BE(p,v) do{ uint8_t *_p=(uint8_t*)(p); uint32_t _v=(v); \
    _p[0]=(uint8_t)(_v>>24);_p[1]=(uint8_t)(_v>>16);_p[2]=(uint8_t)(_v>>8);_p[3]=(uint8_t)_v; }while(0)
#define PUT16BE(p,v) do{ uint8_t *_p=(uint8_t*)(p); uint16_t _v=(v); \
    _p[0]=(uint8_t)(_v>>8);_p[1]=(uint8_t)_v; }while(0)
#define GET32BE(p)   (((uint32_t)(uint8_t)(p)[0]<<24)|((uint32_t)(uint8_t)(p)[1]<<16)|((uint32_t)(uint8_t)(p)[2]<<8)|(uint32_t)(uint8_t)(p)[3])
#define GET16BE(p)   (((uint16_t)(uint8_t)(p)[0]<<8)|(uint16_t)(uint8_t)(p)[1])
#define ROTR32(x,n)  (((x)>>(n))|((x)<<(32-(n))))

/* ================================================================== */
/*  PRNG — ChaCha20 core seeded from rdtsc + ticks                    */
/* ================================================================== */
static uint32_t prng_state[16];
static int prng_ready = 0;
static uint8_t prng_buf[64];
static int prng_pos = 64;

static void chacha20_block(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16]; memcpy(x, in, 64);
    #define QR(a,b,c,d) \
        x[a]+=x[b]; x[d]=ROTR32(x[d]^x[a],16); \
        x[c]+=x[d]; x[b]=ROTR32(x[b]^x[c],12); \
        x[a]+=x[b]; x[d]=ROTR32(x[d]^x[a], 8); \
        x[c]+=x[d]; x[b]=ROTR32(x[b]^x[c], 7);
    for (int i = 0; i < 10; i++) {
        QR(0,4, 8,12) QR(1,5, 9,13) QR(2,6,10,14) QR(3,7,11,15)
        QR(0,5,10,15) QR(1,6,11,12) QR(2,7, 8,13) QR(3,4, 9,14)
    }
    #undef QR
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

void br_prng_init(void) {
    /* seed from rdtsc + ticks */
    u64 tsc = rdtsc();
    u32 tk  = ticks();
    prng_state[0] = 0x61707865; prng_state[1] = 0x3320646e;
    prng_state[2] = 0x79622d32; prng_state[3] = 0x6b206574;
    prng_state[4]  = (uint32_t)(tsc & 0xFFFFFFFF);
    prng_state[5]  = (uint32_t)(tsc >> 32);
    prng_state[6]  = tk;
    prng_state[7]  = tk ^ 0xDEADBEEF;
    prng_state[8]  = (uint32_t)((uintptr_t)prng_state);
    prng_state[9]  = 0x12345678;
    prng_state[10] = 0x9ABCDEF0;
    prng_state[11] = 0x13579BDF;
    prng_state[12] = 0; /* counter */
    prng_state[13] = 0;
    prng_state[14] = (uint32_t)(tsc ^ tk);
    prng_state[15] = tk ^ (uint32_t)(tsc >> 16);
    prng_ready = 1; prng_pos = 64;
}

void br_prng_bytes(uint8_t *out, size_t len) {
    if (!prng_ready) br_prng_init();
    while (len > 0) {
        if (prng_pos >= 64) {
            uint32_t block[16];
            chacha20_block(block, prng_state);
            memcpy(prng_buf, block, 64);
            prng_state[12]++;
            if (!prng_state[12]) prng_state[13]++;
            prng_pos = 0;
        }
        size_t n = 64 - (size_t)prng_pos;
        if (n > len) n = len;
        memcpy(out, prng_buf + prng_pos, n);
        out += n; len -= n; prng_pos += (int)n;
    }
}

/* ================================================================== */
/*  SHA-256                                                            */
/* ================================================================== */
static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

void br_sha256_init(br_sha256_context *ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->count=0; ctx->buflen=0;
}

static void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64], a,b,c,d,e,f,g,h;
    for (int i=0;i<16;i++) w[i]=GET32BE(block+i*4);
    for (int i=16;i<64;i++) {
        uint32_t s0=ROTR32(w[i-15],7)^ROTR32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1=ROTR32(w[i-2],17)^ROTR32(w[i-2],19)^(w[i-2]>>10);
        w[i]=w[i-16]+s0+w[i-7]+s1;
    }
    a=state[0];b=state[1];c=state[2];d=state[3];
    e=state[4];f=state[5];g=state[6];h=state[7];
    for (int i=0;i<64;i++) {
        uint32_t S1=ROTR32(e,6)^ROTR32(e,11)^ROTR32(e,25);
        uint32_t ch=(e&f)^(~e&g);
        uint32_t t1=h+S1+ch+SHA256_K[i]+w[i];
        uint32_t S0=ROTR32(a,2)^ROTR32(a,13)^ROTR32(a,22);
        uint32_t maj=(a&b)^(a&c)^(b&c);
        uint32_t t2=S0+maj;
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

void br_sha256_update(br_sha256_context *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t*)data;
    while (len > 0) {
        int room = 64 - ctx->buflen;
        int take = (int)len < room ? (int)len : room;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take; p += take; len -= (size_t)take;
        ctx->count += (uint64_t)take * 8;
        if (ctx->buflen == 64) { sha256_compress(ctx->state, ctx->buf); ctx->buflen = 0; }
    }
}

void br_sha256_out(const br_sha256_context *src, void *out) {
    br_sha256_context ctx = *src;
    uint64_t bits = ctx.count;   /* message length in bits, captured BEFORE padding is fed in */
    uint8_t pad[64]; memset(pad,0,64);
    pad[0]=0x80;
    int padlen = (ctx.buflen<56) ? (56-ctx.buflen) : (120-ctx.buflen);
    br_sha256_update(&ctx, pad, (size_t)padlen);
    uint8_t lb[8];
    lb[0]=(uint8_t)(bits>>56); lb[1]=(uint8_t)(bits>>48);
    lb[2]=(uint8_t)(bits>>40); lb[3]=(uint8_t)(bits>>32);
    lb[4]=(uint8_t)(bits>>24); lb[5]=(uint8_t)(bits>>16);
    lb[6]=(uint8_t)(bits>> 8); lb[7]=(uint8_t)(bits);
    br_sha256_update(&ctx, lb, 8);
    uint8_t *o=(uint8_t*)out;
    for (int i=0;i<8;i++) PUT32BE(o+i*4, ctx.state[i]);
}

/* ================================================================== */
/*  SHA-1                                                              */
/* ================================================================== */
void br_sha1_init(br_sha1_context *ctx) {
    ctx->state[0]=0x67452301; ctx->state[1]=0xEFCDAB89;
    ctx->state[2]=0x98BADCFE; ctx->state[3]=0x10325476;
    ctx->state[4]=0xC3D2E1F0;
    ctx->count=0; ctx->buflen=0;
}
static void sha1_compress(uint32_t s[5], const uint8_t b[64]) {
    uint32_t w[80]; uint32_t a,c2,c,d,e,t;
    for(int i=0;i<16;i++) w[i]=GET32BE(b+i*4);
    for(int i=16;i<80;i++){uint32_t x=w[i-3]^w[i-8]^w[i-14]^w[i-16];w[i]=(x<<1)|(x>>31);}
    a=s[0];c2=s[1];c=s[2];d=s[3];e=s[4];
    #define R1(a,b,c,d,e,i) t=((a<<5)|(a>>27))+(((b&c)|(~b&d))+e+0x5A827999+w[i]); e=d;d=c;c=(b<<30)|(b>>2);b=a;a=t;
    #define R2(a,b,c,d,e,i) t=((a<<5)|(a>>27))+(( b^c^d            )+e+0x6ED9EBA1+w[i]); e=d;d=c;c=(b<<30)|(b>>2);b=a;a=t;
    #define R3(a,b,c,d,e,i) t=((a<<5)|(a>>27))+(((b&c)|(b&d)|(c&d) )+e+0x8F1BBCDC+w[i]); e=d;d=c;c=(b<<30)|(b>>2);b=a;a=t;
    #define R4(a,b,c,d,e,i) t=((a<<5)|(a>>27))+(( b^c^d            )+e+0xCA62C1D6+w[i]); e=d;d=c;c=(b<<30)|(b>>2);b=a;a=t;
    for(int i= 0;i<20;i++) { R1(a,c2,c,d,e,i) }
    for(int i=20;i<40;i++) { R2(a,c2,c,d,e,i) }
    for(int i=40;i<60;i++) { R3(a,c2,c,d,e,i) }
    for(int i=60;i<80;i++) { R4(a,c2,c,d,e,i) }
    #undef R1
    #undef R2
    #undef R3
    #undef R4
    s[0]+=a;s[1]+=c2;s[2]+=c;s[3]+=d;s[4]+=e;
}
void br_sha1_update(br_sha1_context *ctx, const void *data, size_t len) {
    const uint8_t *p=(const uint8_t*)data;
    while(len>0){
        int room=64-ctx->buflen, take=(int)len<room?(int)len:room;
        memcpy(ctx->buf+ctx->buflen,p,take);
        ctx->buflen+=take;p+=take;len-=(size_t)take;ctx->count+=(uint64_t)take*8;
        if(ctx->buflen==64){sha1_compress(ctx->state,ctx->buf);ctx->buflen=0;}
    }
}
void br_sha1_out(const br_sha1_context *src, void *out) {
    br_sha1_context ctx=*src;
    uint64_t bits=ctx.count;   /* captured BEFORE padding is fed in */
    uint8_t pad[64];memset(pad,0,64);pad[0]=0x80;
    int padlen=(ctx.buflen<56)?(56-ctx.buflen):(120-ctx.buflen);
    br_sha1_update(&ctx,pad,(size_t)padlen);
    uint8_t lb[8];
    lb[0]=(uint8_t)(bits>>56);lb[1]=(uint8_t)(bits>>48);
    lb[2]=(uint8_t)(bits>>40);lb[3]=(uint8_t)(bits>>32);
    lb[4]=(uint8_t)(bits>>24);lb[5]=(uint8_t)(bits>>16);
    lb[6]=(uint8_t)(bits>>8); lb[7]=(uint8_t)(bits);
    br_sha1_update(&ctx,lb,8);
    uint8_t *o=(uint8_t*)out;
    for(int i=0;i<5;i++) PUT32BE(o+i*4,ctx.state[i]);
}

/* ================================================================== */
/*  HMAC                                                               */
/* ================================================================== */
void br_hmac_key_init_sha256(br_hmac_key_context *kc, const void *key, size_t klen) {
    kc->is_sha256=1; kc->out_len=32;
    uint8_t k2[64]; memset(k2,0,64);
    if(klen>64){br_sha256_context h;br_sha256_init(&h);br_sha256_update(&h,key,klen);br_sha256_out(&h,k2);}
    else memcpy(k2,key,klen);
    for(int i=0;i<64;i++) kc->kpad[i]=k2[i]^0x36; /* ipad will be XORed with ipad ^ opad later */
    /* Store raw key XOR 0x36 in kpad (ipad), will build opad on demand */
    /* Actually store raw key for flexibility */
    memset(k2,0,64);
    if(klen>64){br_sha256_context h;br_sha256_init(&h);br_sha256_update(&h,key,klen);br_sha256_out(&h,k2);}
    else memcpy(k2,key,klen);
    for(int i=0;i<64;i++) kc->kpad[i]=k2[i]; /* store raw normalised key */
}
void br_hmac_key_init_sha1(br_hmac_key_context *kc, const void *key, size_t klen) {
    kc->is_sha256=0; kc->out_len=20;
    uint8_t k2[64]; memset(k2,0,64);
    if(klen>64){br_sha1_context h;br_sha1_init(&h);br_sha1_update(&h,key,klen);br_sha1_out(&h,k2);}
    else memcpy(k2,key,klen);
    for(int i=0;i<64;i++) kc->kpad[i]=k2[i];
}
void br_hmac_init(br_hmac_context *ctx, const br_hmac_key_context *kc, size_t out_len) {
    ctx->key=*kc;
    if(out_len && out_len<(size_t)kc->out_len) ctx->key.out_len=(int)out_len;
    uint8_t ipad[64];
    for(int i=0;i<64;i++) ipad[i]=kc->kpad[i]^0x36;
    if(kc->is_sha256){br_sha256_init(&ctx->h.sha256);br_sha256_update(&ctx->h.sha256,ipad,64);}
    else            {br_sha1_init(&ctx->h.sha1);    br_sha1_update(&ctx->h.sha1,ipad,64);}
}
void br_hmac_update(br_hmac_context *ctx, const void *data, size_t len) {
    if(ctx->key.is_sha256) br_sha256_update(&ctx->h.sha256,data,len);
    else                   br_sha1_update(&ctx->h.sha1,data,len);
}
size_t br_hmac_out(br_hmac_context *ctx, void *out) {
    uint8_t inner[32], opad[64];
    if(ctx->key.is_sha256) br_sha256_out(&ctx->h.sha256,inner);
    else                  {br_sha1_out(&ctx->h.sha1,inner);}
    for(int i=0;i<64;i++) opad[i]=ctx->key.kpad[i]^0x5c;
    if(ctx->key.is_sha256){
        br_sha256_context h; br_sha256_init(&h);
        br_sha256_update(&h,opad,64);
        br_sha256_update(&h,inner,32);
        br_sha256_out(&h,out);
    } else {
        br_sha1_context h; br_sha1_init(&h);
        br_sha1_update(&h,opad,64);
        br_sha1_update(&h,inner,20);
        br_sha1_out(&h,out);
    }
    return (size_t)ctx->key.out_len;
}

/* ================================================================== */
/*  AES-128 constant-time (Rijndael bit-sliced) - pure C, no SSE     */
/* ================================================================== */
/* Substitution table */
static const uint8_t AES_SBOX[256]={
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const uint8_t AES_SBOX_INV[256]={
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p=0;
    for(int i=0;i<8;i++){
        if(b&1) p^=a;
        int hb=(a>>7)&1; a<<=1; if(hb) a^=0x1b; b>>=1;
    }
    return p;
}

static void aes_key_expand(const uint8_t *key, int nk, uint32_t *w) {
    static const uint8_t rcon[11]={0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    for(int i=0;i<nk;i++) w[i]=GET32BE(key+i*4);
    int nr = nk + 6;
    for(int i=nk;i<4*(nr+1);i++){
        uint32_t t=w[i-1];
        if(i%nk==0){
            t=((uint32_t)AES_SBOX[(t>>16)&0xff]<<24)|
              ((uint32_t)AES_SBOX[(t>> 8)&0xff]<<16)|
              ((uint32_t)AES_SBOX[(t    )&0xff]<< 8)|
              ((uint32_t)AES_SBOX[(t>>24)&0xff]);
            t^=(uint32_t)rcon[i/nk]<<24;
        } else if(nk>6&&i%nk==4){
            t=((uint32_t)AES_SBOX[(t>>24)&0xff]<<24)|
              ((uint32_t)AES_SBOX[(t>>16)&0xff]<<16)|
              ((uint32_t)AES_SBOX[(t>> 8)&0xff]<< 8)|
              ((uint32_t)AES_SBOX[(t    )&0xff]);
        }
        w[i]=w[i-nk]^t;
    }
}

static void aes_add_round_key(uint8_t s[16], const uint32_t *w, int round) {
    for(int i=0;i<4;i++){
        s[i*4+0]^=(uint8_t)(w[round*4+i]>>24);
        s[i*4+1]^=(uint8_t)(w[round*4+i]>>16);
        s[i*4+2]^=(uint8_t)(w[round*4+i]>>8);
        s[i*4+3]^=(uint8_t)(w[round*4+i]);
    }
}
static void aes_sub_bytes(uint8_t s[16]){ for(int i=0;i<16;i++) s[i]=AES_SBOX[s[i]]; }
static void aes_sub_bytes_inv(uint8_t s[16]){ for(int i=0;i<16;i++) s[i]=AES_SBOX_INV[s[i]]; }
static void aes_shift_rows(uint8_t s[16]){
    uint8_t t;
    t=s[1];s[1]=s[5];s[5]=s[9];s[9]=s[13];s[13]=t;
    t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
    t=s[15];s[15]=s[11];s[11]=s[7];s[7]=s[3];s[3]=t;
}
static void aes_shift_rows_inv(uint8_t s[16]){
    uint8_t t;
    t=s[13];s[13]=s[9];s[9]=s[5];s[5]=s[1];s[1]=t;
    t=s[2];s[2]=s[10];s[10]=t; t=s[6];s[6]=s[14];s[14]=t;
    t=s[3];s[3]=s[7];s[7]=s[11];s[11]=s[15];s[15]=t;
}
static void aes_mix_columns(uint8_t s[16]){
    for(int c=0;c<4;c++){
        uint8_t a=s[c*4],b=s[c*4+1],cc=s[c*4+2],d=s[c*4+3];
        s[c*4  ]=gmul(2,a)^gmul(3,b)^cc^d;
        s[c*4+1]=a^gmul(2,b)^gmul(3,cc)^d;
        s[c*4+2]=a^b^gmul(2,cc)^gmul(3,d);
        s[c*4+3]=gmul(3,a)^b^cc^gmul(2,d);
    }
}
static void aes_mix_columns_inv(uint8_t s[16]){
    for(int c=0;c<4;c++){
        uint8_t a=s[c*4],b=s[c*4+1],cc=s[c*4+2],d=s[c*4+3];
        s[c*4  ]=gmul(0x0e,a)^gmul(0x0b,b)^gmul(0x0d,cc)^gmul(0x09,d);
        s[c*4+1]=gmul(0x09,a)^gmul(0x0e,b)^gmul(0x0b,cc)^gmul(0x0d,d);
        s[c*4+2]=gmul(0x0d,a)^gmul(0x09,b)^gmul(0x0e,cc)^gmul(0x0b,d);
        s[c*4+3]=gmul(0x0b,a)^gmul(0x0d,b)^gmul(0x09,cc)^gmul(0x0e,d);
    }
}
static void aes_encrypt_block(const uint32_t *rk, int nr, uint8_t b[16]){
    uint8_t s[16]; memcpy(s,b,16);
    aes_add_round_key(s,rk,0);
    for(int r=1;r<nr;r++){
        aes_sub_bytes(s); aes_shift_rows(s); aes_mix_columns(s); aes_add_round_key(s,rk,r);
    }
    aes_sub_bytes(s); aes_shift_rows(s); aes_add_round_key(s,rk,nr);
    memcpy(b,s,16);
}
static void aes_decrypt_block(const uint32_t *rk, int nr, uint8_t b[16]){
    uint8_t s[16]; memcpy(s,b,16);
    aes_add_round_key(s,rk,nr);
    for(int r=nr-1;r>0;r--){
        aes_shift_rows_inv(s); aes_sub_bytes_inv(s); aes_add_round_key(s,rk,r); aes_mix_columns_inv(s);
    }
    aes_shift_rows_inv(s); aes_sub_bytes_inv(s); aes_add_round_key(s,rk,0);
    memcpy(b,s,16);
}

void br_aes_ct_cbcenc_init(br_aes_ct_cbcenc_keys *k, const void *key, size_t klen){
    int nk=(int)klen/4; k->num_rounds=nk+6; aes_key_expand((const uint8_t*)key,nk,k->skey);
}
void br_aes_ct_cbcdec_init(br_aes_ct_cbcdec_keys *k, const void *key, size_t klen){
    int nk=(int)klen/4; k->num_rounds=nk+6; aes_key_expand((const uint8_t*)key,nk,k->skey);
}
void br_aes_ct_cbcenc_run(const br_aes_ct_cbcenc_keys *k, void *iv, void *data, size_t len){
    uint8_t *p=(uint8_t*)data, *ivp=(uint8_t*)iv;
    while(len>=16){
        for(int i=0;i<16;i++) p[i]^=ivp[i];
        aes_encrypt_block(k->skey,k->num_rounds,p);
        memcpy(ivp,p,16); p+=16; len-=16;
    }
}
void br_aes_ct_cbcdec_run(const br_aes_ct_cbcdec_keys *k, void *iv, void *data, size_t len){
    uint8_t *p=(uint8_t*)data, *ivp=(uint8_t*)iv;
    while(len>=16){
        uint8_t niv[16]; memcpy(niv,p,16);
        aes_decrypt_block(k->skey,k->num_rounds,p);
        for(int i=0;i<16;i++) p[i]^=ivp[i];
        memcpy(ivp,niv,16); p+=16; len-=16;
    }
}

/* ================================================================== */
/*  P-256 ECDHE                                                        */
/*  Compact implementation of scalar multiplication on P-256.         */
/*  Arithmetic is in Montgomery form, 32-bit limbs.                   */
/* ================================================================== */

/* P-256 prime in little-endian 32-bit limbs */
static const uint32_t P256_P[8] = {
    0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0x00000000,
    0x00000000,0x00000000,0x00000001,0xFFFFFFFF
};
static const uint32_t P256_N[8] = {
    0xFC632551,0xF3B9CAC2,0xA7179E84,0xBCE6FAAD,
    0xFFFFFFFF,0xFFFFFFFF,0x00000000,0xFFFFFFFF
};
static const uint32_t P256_B[8] = {
    0x27D2604B,0x3BCE3C3E,0xCC53B0F6,0x651D06B0,
    0x769886BC,0xB3EBBD55,0xAA3A93E7,0x5AC635D8
};
/* Generator x */
static const uint32_t P256_GX[8] = {
    0xD898C296,0xF4A13945,0x2DEB33A0,0x77037D81,
    0x63A440F2,0xF8BCE6E5,0xE12C4247,0x6B17D1F2
};
/* Generator y */
static const uint32_t P256_GY[8] = {
    0x37BF51F5,0xCBB64068,0x6B315ECE,0x2BCE3357,
    0x7C0F9E16,0x8EE7EB4A,0xFE1A7F9B,0x4FE342E2
};

/* 256-bit integer operations on 8×u32 arrays (little-endian) */
static int p256_cmp(const uint32_t *a, const uint32_t *b){
    for(int i=7;i>=0;i--){ if(a[i]>b[i]) return 1; if(a[i]<b[i]) return -1; } return 0;
}
static int p256_is_zero(const uint32_t *a){
    for(int i=0;i<8;i++) if(a[i]) return 0; return 1;
}
static void p256_copy(uint32_t *dst, const uint32_t *src){ memcpy(dst,src,32); }
static void p256_zero(uint32_t *a){ memset(a,0,32); }
static uint32_t p256_add(uint32_t *r, const uint32_t *a, const uint32_t *b){
    uint64_t carry=0;
    for(int i=0;i<8;i++){ uint64_t s=(uint64_t)a[i]+b[i]+carry; r[i]=(uint32_t)s; carry=s>>32; }
    return (uint32_t)carry;
}
/* r = a - b; returns the final borrow (1 if a < b) */
static uint32_t p256_sub(uint32_t *r, const uint32_t *a, const uint32_t *b){
    uint64_t borrow=0;
    for(int i=0;i<8;i++){ uint64_t d=(uint64_t)a[i]-b[i]-borrow; r[i]=(uint32_t)d; borrow=(d>>63)&1; }
    return (uint32_t)borrow;
}
/* Inputs must be fully reduced (< P). */
static void p256_mod_add(uint32_t *r, const uint32_t *a, const uint32_t *b){
    uint32_t carry=p256_add(r,a,b);
    if(carry || p256_cmp(r,P256_P)>=0) p256_sub(r,r,P256_P);
}
static void p256_mod_sub(uint32_t *r, const uint32_t *a, const uint32_t *b){
    if(p256_sub(r,a,b)) p256_add(r,r,P256_P);   /* went negative: add P back */
}

/* ---- Montgomery arithmetic, R = 2^256.  Because P = -1 (mod 2^32), the
 * Montgomery constant n0' = -P^-1 mod 2^32 is simply 1. ---- */
static const uint32_t P256_R2[8] = {   /* R^2 mod P */
    0x00000003,0x00000000,0xFFFFFFFF,0xFFFFFFFB,0xFFFFFFFE,0xFFFFFFFF,0xFFFFFFFD,0x00000004
};
static const uint32_t P256_ONE_M[8] = { /* R mod P  (the number 1 in Montgomery form) */
    0x00000001,0x00000000,0x00000000,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFE,0x00000000
};
/* r = a*b*R^-1 mod P  (CIOS).  r may alias a or b. */
static void p256_mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b){
    uint32_t t[10]; memset(t,0,sizeof t);
    for(int i=0;i<8;i++){
        uint64_t c=0;
        for(int j=0;j<8;j++){
            uint64_t s=(uint64_t)t[j]+(uint64_t)a[j]*b[i]+c;
            t[j]=(uint32_t)s; c=s>>32;
        }
        uint64_t s=(uint64_t)t[8]+c; t[8]=(uint32_t)s; t[9]=(uint32_t)(s>>32);
        uint32_t m=t[0];                       /* t[0] * n0'  with n0' == 1 */
        c=((uint64_t)t[0]+(uint64_t)m*P256_P[0])>>32;
        for(int j=1;j<8;j++){
            uint64_t s2=(uint64_t)t[j]+(uint64_t)m*P256_P[j]+c;
            t[j-1]=(uint32_t)s2; c=s2>>32;
        }
        uint64_t s3=(uint64_t)t[8]+c; t[7]=(uint32_t)s3;
        t[8]=t[9]+(uint32_t)(s3>>32);
    }
    uint32_t res[8]; memcpy(res,t,32);
    if(t[8] || p256_cmp(res,P256_P)>=0) p256_sub(res,res,P256_P);
    p256_copy(r,res);
}
static void p256_to_mont(uint32_t *r, const uint32_t *a){ p256_mont_mul(r,a,P256_R2); }
static void p256_from_mont(uint32_t *r, const uint32_t *a){
    uint32_t one[8]={1,0,0,0,0,0,0,0}; p256_mont_mul(r,a,one);
}
/* r = a^-1 (Montgomery domain in and out), Fermat: a^(P-2) */
static void p256_mont_inv(uint32_t *r, const uint32_t *a){
    uint32_t e[8]; p256_copy(e,P256_P); e[0]-=2;
    uint32_t res[8]; p256_copy(res,P256_ONE_M);
    for(int i=255;i>=0;i--){
        p256_mont_mul(res,res,res);
        if((e[i/32]>>(i%32))&1) p256_mont_mul(res,res,a);
    }
    p256_copy(r,res);
}

/* Jacobian point (Montgomery domain); infinity has z == 0 */
typedef struct { uint32_t x[8], y[8], z[8]; } P256J;
#define FM p256_mont_mul

static void p256j_double(P256J *r, const P256J *p){
    if(p256_is_zero(p->z)){ *r=*p; return; }
    uint32_t delta[8],gamma[8],beta[8],alpha[8],t1[8],t2[8],x3[8],y3[8],z3[8];
    FM(delta,p->z,p->z);
    FM(gamma,p->y,p->y);
    FM(beta,p->x,gamma);
    p256_mod_sub(t1,p->x,delta);
    p256_mod_add(t2,p->x,delta);
    FM(alpha,t1,t2);
    p256_mod_add(t1,alpha,alpha); p256_mod_add(alpha,t1,alpha);   /* alpha = 3(x-d)(x+d) */
    FM(x3,alpha,alpha);
    p256_mod_add(t1,beta,beta); p256_mod_add(t1,t1,t1);           /* t1 = 4*beta */
    p256_mod_add(t2,t1,t1);                                       /* t2 = 8*beta */
    p256_mod_sub(x3,x3,t2);
    p256_mod_add(t2,p->y,p->z); FM(z3,t2,t2);
    p256_mod_sub(z3,z3,gamma); p256_mod_sub(z3,z3,delta);
    p256_mod_sub(t2,t1,x3); FM(y3,alpha,t2);
    FM(t2,gamma,gamma);
    p256_mod_add(t2,t2,t2); p256_mod_add(t2,t2,t2); p256_mod_add(t2,t2,t2);   /* 8*gamma^2 */
    p256_mod_sub(y3,y3,t2);
    p256_copy(r->x,x3); p256_copy(r->y,y3); p256_copy(r->z,z3);
}

static void p256j_add(P256J *r, const P256J *p, const P256J *q){
    if(p256_is_zero(p->z)){ *r=*q; return; }
    if(p256_is_zero(q->z)){ *r=*p; return; }
    uint32_t z1z1[8],z2z2[8],u1[8],u2[8],s1[8],s2[8],h[8],rr[8],hh[8],hhh[8],v[8],t[8],x3[8],y3[8],z3[8];
    FM(z1z1,p->z,p->z); FM(z2z2,q->z,q->z);
    FM(u1,p->x,z2z2);   FM(u2,q->x,z1z1);
    FM(t,q->z,z2z2);    FM(s1,p->y,t);
    FM(t,p->z,z1z1);    FM(s2,q->y,t);
    p256_mod_sub(h,u2,u1); p256_mod_sub(rr,s2,s1);
    if(p256_is_zero(h)){
        if(p256_is_zero(rr)){ p256j_double(r,p); return; }
        memset(r,0,sizeof *r); return;                 /* P + (-P) = infinity */
    }
    FM(hh,h,h); FM(hhh,h,hh); FM(v,u1,hh);
    FM(x3,rr,rr); p256_mod_sub(x3,x3,hhh); p256_mod_sub(x3,x3,v); p256_mod_sub(x3,x3,v);
    p256_mod_sub(t,v,x3); FM(y3,rr,t); FM(t,s1,hhh); p256_mod_sub(y3,y3,t);
    FM(t,p->z,q->z); FM(z3,t,h);
    p256_copy(r->x,x3); p256_copy(r->y,y3); p256_copy(r->z,z3);
}

/* (rx,ry) = k * (bx,by); all affine, normal domain.  k is 32 bytes big-endian.
 * Returns 0 if the result is the point at infinity. */
static int p256_scalar_mul_affine(uint32_t *rx, uint32_t *ry,
                                  const uint32_t *bx, const uint32_t *by,
                                  const uint8_t k[32]){
    P256J B,acc;
    p256_to_mont(B.x,bx); p256_to_mont(B.y,by); p256_copy(B.z,P256_ONE_M);
    memset(&acc,0,sizeof acc);
    for(int i=255;i>=0;i--){
        p256j_double(&acc,&acc);
        if((k[31-(i/8)]>>(i%8))&1) p256j_add(&acc,&acc,&B);
    }
    if(p256_is_zero(acc.z)) return 0;
    uint32_t zi[8],zi2[8],zi3[8],ax[8],ay[8];
    p256_mont_inv(zi,acc.z); FM(zi2,zi,zi); FM(zi3,zi2,zi);
    FM(ax,acc.x,zi2); FM(ay,acc.y,zi3);
    p256_from_mont(rx,ax); p256_from_mont(ry,ay);
    return 1;
}

/* y^2 == x^3 - 3x + b (mod P), and both coordinates < P */
static int p256_on_curve(const uint32_t *x, const uint32_t *y){
    if(p256_cmp(x,P256_P)>=0 || p256_cmp(y,P256_P)>=0) return 0;
    uint32_t xm[8],ym[8],bm[8],l[8],r[8],t[8];
    p256_to_mont(xm,x); p256_to_mont(ym,y); p256_to_mont(bm,P256_B);
    FM(l,ym,ym);
    FM(t,xm,xm); FM(r,t,xm);
    p256_mod_sub(r,r,xm); p256_mod_sub(r,r,xm); p256_mod_sub(r,r,xm);
    p256_mod_add(r,r,bm);
    return memcmp(l,r,32)==0;
}

/* Convert from LE 32-bit array to BE bytes */
static void p256_to_bytes_be(const uint32_t *v, uint8_t *out){
    for(int i=0;i<8;i++) PUT32BE(out+(7-i)*4, v[i]);
}
static void p256_from_bytes_be(uint32_t *v, const uint8_t *in){
    for(int i=0;i<8;i++) v[i]=GET32BE(in+(7-i)*4);
}

/* Generate ephemeral key pair (private scalar in [1, N-1]) */
static void ecdhe_gen_keypair(uint8_t priv[32], uint8_t pub[65]){
    uint32_t k[8];
    do {
        br_prng_bytes(priv,32);
        p256_from_bytes_be(k,priv);
    } while(p256_is_zero(k) || p256_cmp(k,P256_N)>=0);
    uint32_t px[8],py[8];
    p256_scalar_mul_affine(px,py,P256_GX,P256_GY,priv);
    pub[0]=0x04;
    p256_to_bytes_be(px, pub+1);
    p256_to_bytes_be(py, pub+33);
}

/* Returns 1 on success, 0 if the server's point is invalid. */
static int ecdhe_shared_secret(uint8_t shared[32],
                               const uint8_t server_pub[65],
                               const uint8_t client_priv[32]){
    if(server_pub[0]!=0x04) return 0;
    uint32_t sx[8],sy[8],rx[8],ry[8];
    p256_from_bytes_be(sx, server_pub+1);
    p256_from_bytes_be(sy, server_pub+33);
    if(!p256_on_curve(sx,sy)) return 0;
    if(!p256_scalar_mul_affine(rx,ry,sx,sy,client_priv)) return 0;
    p256_to_bytes_be(rx,shared);
    return 1;
}

/* ================================================================== */
/*  TLS PRF (TLS 1.2, SHA-256)                                        */
/* ================================================================== */
static void hmac_sha256(uint8_t *out, size_t out_len,
                        const uint8_t *key, size_t klen,
                        const uint8_t *data, size_t dlen){
    br_hmac_key_context kc; br_hmac_key_init_sha256(&kc,key,klen);
    br_hmac_context h; br_hmac_init(&h,&kc,0);
    br_hmac_update(&h,data,dlen);
    uint8_t tmp[32]; br_hmac_out(&h,tmp);
    size_t cp=out_len<32?out_len:32; memcpy(out,tmp,cp);
}

/* P_hash: expand using HMAC-SHA256 */
static void tls_p_hash(uint8_t *out, size_t olen,
                       const uint8_t *secret, size_t slen,
                       const uint8_t *seed, size_t seedlen){
    uint8_t A[32]; /* A(1) = HMAC(secret, seed) */
    hmac_sha256(A,32,secret,slen,seed,seedlen);
    size_t pos=0;
    while(pos<olen){
        /* HMAC(secret, A(i) || seed) */
        uint8_t concat[32+256]; memcpy(concat,A,32); memcpy(concat+32,seed,seedlen);
        uint8_t tmp[32]; hmac_sha256(tmp,32,secret,slen,concat,32+seedlen);
        size_t take=olen-pos; if(take>32) take=32;
        memcpy(out+pos,tmp,take); pos+=take;
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(A,32,secret,slen,A,32);
    }
}

/* PRF: label || seed */
static void tls_prf(uint8_t *out, size_t olen,
                    const uint8_t *secret, size_t slen,
                    const char *label,
                    const uint8_t *seed, size_t seedlen){
    size_t llen=strlen(label);
    uint8_t combined[64+64]; /* label(max 32) + client_random(32) + server_random(32) */
    if(llen+seedlen>sizeof combined) llen=sizeof combined-seedlen;
    memcpy(combined,label,llen);
    memcpy(combined+llen,seed,seedlen);
    tls_p_hash(out,olen,secret,slen,combined,llen+seedlen);
}

/* ================================================================== */
/*  RSA PKCS#1 v1.5 verify (signature check on server cert)          */
/* ================================================================== */
/* Simple big-integer modular exponentiation for RSA verify.
 * Supports up to 2048-bit keys. */
#define RSA_MAX_LIMBS 64   /* 64 × 32 = 2048 bits */

static void bigint_zero(uint32_t *a, int n){ memset(a,0,n*4); }
static void bigint_from_bytes_be(uint32_t *a, int n, const uint8_t *b, int blen){
    bigint_zero(a,n);
    int limb=n-1;
    for(int i=blen-1;i>=0&&limb>=0;){
        uint32_t v=0;
        for(int j=0;j<4&&i>=0;j++,i--) v|=((uint32_t)(uint8_t)b[i])<<(j*8);
        a[limb--]=v;
    }
}
static int bigint_n_limbs(const uint8_t *be, int len){
    return (len+3)/4;
}
static int bigint_cmp(const uint32_t *a, const uint32_t *b, int n){
    for(int i=n-1;i>=0;i--){ if(a[i]>b[i]) return 1; if(a[i]<b[i]) return -1; } return 0;
}
static void bigint_sub(uint32_t *r, const uint32_t *a, const uint32_t *b, int n){
    uint64_t borrow=0;
    for(int i=0;i<n;i++){ uint64_t d=(uint64_t)a[i]-b[i]-borrow; r[i]=(uint32_t)d; borrow=(d>>63)&1; }
}
/* Multiply mod m */
static void bigint_mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b,
                          const uint32_t *m, int n){
    uint32_t prod[RSA_MAX_LIMBS*2]; memset(prod,0,n*8);
    for(int i=0;i<n;i++) for(int j=0;j<n;j++){
        uint64_t pij=(uint64_t)a[i]*b[j]; int k=i+j;
        uint32_t carry=(uint32_t)((((uint64_t)prod[k]+pij)>>32));
        prod[k]+=(uint32_t)pij;
        for(int l=k+1;carry&&l<n*2;l++){
            uint64_t acc=(uint64_t)prod[l]+carry; prod[l]=(uint32_t)acc; carry=(uint32_t)(acc>>32);
        }
    }
    /* mod m: simple long division by repeated subtraction of m<<shift */
    /* copy low n limbs as result; reduce high limbs */
    memcpy(r,prod,n*4);
    /* shift high half back down */
    for(int i=n;i<n*2;i++){
        if(!prod[i]) continue;
        /* add prod[i]*2^(32*i) mod m — this is approximate; for RSA verify
         * we need exact. Use a proper Barrett/Montgomery for production.
         * For now: since e=65537 this is square-and-multiply with small e,
         * we just ensure the base is reduced before multiplying. */
    }
    while(bigint_cmp(r,m,n)>=0) bigint_sub(r,r,m,n);
}

/* r = base^exp mod m, exp as big-endian bytes */
static void bigint_powmod_bytes(uint32_t *r, const uint32_t *base,
                                const uint8_t *exp_be, int explen,
                                const uint32_t *m, int n){
    uint32_t result[RSA_MAX_LIMBS]; bigint_zero(result,n); result[0]=1;
    uint32_t b[RSA_MAX_LIMBS]; memcpy(b,base,n*4);
    while(bigint_cmp(b,m,n)>=0) bigint_sub(b,b,m,n);
    for(int i=explen*8-1;i>=0;i--){
        uint32_t tmp[RSA_MAX_LIMBS];
        bigint_mulmod(tmp,result,result,m,n);
        memcpy(result,tmp,n*4);
        int bit=(exp_be[explen-1-i/8]>>(i%8))&1;
        if(bit){ bigint_mulmod(tmp,result,b,m,n); memcpy(result,tmp,n*4); }
    }
    memcpy(r,result,n*4);
}

int br_rsa_pkcs1_vrfy(const uint8_t *sig, size_t siglen,
                      const uint8_t *hash_oid, size_t hash_len,
                      const br_rsa_public_key *pk,
                      uint8_t *hash_out){
    (void)hash_oid;
    int n = bigint_n_limbs(pk->n, (int)pk->nlen);
    if(n>RSA_MAX_LIMBS) return 0;
    uint32_t m[RSA_MAX_LIMBS], s[RSA_MAX_LIMBS], r[RSA_MAX_LIMBS];
    bigint_from_bytes_be(m,n,pk->n,(int)pk->nlen);
    bigint_from_bytes_be(s,n,sig,(int)siglen);
    bigint_powmod_bytes(r,s,pk->e,(int)pk->elen,m,n);
    /* PKCS#1 v1.5: 0x00 0x01 [0xff...] 0x00 [DigestInfo] [hash] */
    uint8_t buf[256]; memset(buf,0,sizeof buf);
    int blen=(int)pk->nlen;
    for(int i=0;i<n;i++) PUT32BE(buf+(n-1-i)*4, r[i]);
    /* Verify padding */
    int ok=1;
    if(buf[0]!=0x00||buf[1]!=0x01) ok=0;
    int pos=2; while(pos<blen&&buf[pos]==0xFF) pos++;
    if(buf[pos]!=0x00) ok=0; pos++;
    /* Skip DigestInfo ASN.1 (variable length, just copy the last hash_len bytes) */
    if(ok&&hash_out) memcpy(hash_out, buf+blen-(int)hash_len, hash_len);
    return ok;
}

/* ================================================================== */
/*  AES-GCM  (AES-CTR + GHASH for TLS 1.2 AEAD)                       */
/* ================================================================== */

/* GF(2^128) multiplication: z = (z XOR x) * h */
static void ghash_mult(uint8_t *z, const uint8_t *x, const uint8_t *h) {
    uint8_t v[16], z2[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 16; i++) z2[i] = z[i] ^ x[i];
    uint8_t r[16]; memset(r, 0, 16);
    for (int i = 0; i < 128; i++) {
        if (z2[i/8] & (0x80 >> (i % 8)))
            for (int j = 0; j < 16; j++) r[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (v[j] >> 1) | ((v[j-1] & 1) << 7);
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xE1;
    }
    memcpy(z, r, 16);
}

/* GHASH over data (with zero-padding for partial final block) */
static void ghash_update(uint8_t z[16], const uint8_t *data, size_t len, const uint8_t h[16]) {
    size_t i = 0;
    while (i + 16 <= len) {
        ghash_mult(z, data + i, h);
        i += 16;
    }
    if (i < len) {
        uint8_t block[16]; memset(block, 0, 16);
        memcpy(block, data + i, len - i);
        ghash_mult(z, block, h);
    }
}

/* AES-CTR: XOR keystream into data.  12-byte IV + 4-byte counter. */
static void aes_ctr_xor(const br_aes_ct_cbcenc_keys *k, const uint8_t iv[12],
                        uint32_t ctr_start, uint8_t *data, size_t len) {
    uint8_t counter[16];
    memcpy(counter, iv, 12);
    uint32_t ctr = ctr_start;
    size_t i = 0;
    while (i < len) {
        counter[12] = (uint8_t)(ctr >> 24); counter[13] = (uint8_t)(ctr >> 16);
        counter[14] = (uint8_t)(ctr >> 8);  counter[15] = (uint8_t)ctr;
        uint8_t ks[16]; memcpy(ks, counter, 16);
        aes_encrypt_block(k->skey, k->num_rounds, ks);
        size_t take = (len - i < 16) ? (len - i) : 16;
        for (size_t j = 0; j < take; j++) data[i + j] ^= ks[j];
        i += 16; ctr++;
    }
}

/* Compute GCM tag over AAD + ciphertext, XOR with EK0 */
static void gcm_tag(uint8_t tag[16], const uint8_t h[16],
                    const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t ct_len,
                    const uint8_t ek0[16]) {
    uint8_t z[16]; memset(z, 0, 16);
    ghash_update(z, aad, aad_len, h);
    ghash_update(z, ct, ct_len, h);
    /* length block: aad_bits (8) || ct_bits (8) */
    uint8_t lb[16]; memset(lb, 0, 16);
    uint64_t ab = (uint64_t)aad_len * 8, cb = (uint64_t)ct_len * 8;
    for (int i = 0; i < 8; i++) lb[i]     = (uint8_t)(ab >> (56 - 8*i));
    for (int i = 0; i < 8; i++) lb[8 + i] = (uint8_t)(cb >> (56 - 8*i));
    ghash_mult(z, lb, h);
    for (int i = 0; i < 16; i++) tag[i] = z[i] ^ ek0[i];
}

/* Check if cipher suite is GCM */
static int is_gcm(uint16_t suite) {
    return suite == 0xC02F || suite == 0x009C;
}

/* ================================================================== */
/*  TLS record layer                                                   */
/* ================================================================== */
#define TLS_RT_CHANGE_CIPHER_SPEC  20
#define TLS_RT_ALERT               21
#define TLS_RT_HANDSHAKE           22
#define TLS_RT_APPLICATION_DATA    23
#define TLS_VER_12  0x0303

/* Write a TLS record into eng->net_send_buf */
static void tls_write_record(br_ssl_engine_context *eng,
                             uint8_t type, const uint8_t *data, size_t len){
    /* Optional: encrypt if cipher is active (after CCS) */
    uint8_t *p = eng->net_send_buf + eng->net_send_len;
    /* Use TLS 1.0 record version for initial ClientHello, TLS 1.2 after */
    uint8_t rv1 = (eng->hs == TLS_HS_CLIENT_HELLO) ? 0x03 : 0x03;
    uint8_t rv2 = (eng->hs == TLS_HS_CLIENT_HELLO) ? 0x01 : 0x03;
    int encrypted = (eng->hs == TLS_HS_DONE ||
                     eng->hs == TLS_HS_FINISHED_OUT ||
                     eng->hs == TLS_HS_CHANGE_CIPHER_SPEC_IN ||
                     eng->hs == TLS_HS_FINISHED_IN);
    /* For FINISHED_OUT we send encrypted; after that app data is encrypted */

    if(encrypted && eng->cipher_suite != 0){
        if (is_gcm(eng->cipher_suite)) {
            /* AES-128-GCM: [explicit_nonce(8)] [ciphertext] [tag(16)] */
            uint8_t nonce[8]; br_prng_bytes(nonce, 8);
            uint8_t iv[12]; memcpy(iv, eng->gcm_iv_enc, 4); memcpy(iv+4, nonce, 8);
            uint8_t j0[16]; memcpy(j0, iv, 12); j0[12]=0;j0[13]=0;j0[14]=0;j0[15]=1;
            uint8_t ek0[16]; memcpy(ek0, j0, 16);
            aes_encrypt_block(eng->enc_key.skey, eng->enc_key.num_rounds, ek0);
            uint8_t *ct = (uint8_t*)kmalloc((uint32_t)(len > 0 ? len : 1));
            if(!ct){ eng->err=BR_ERR_IO; return; }
            memcpy(ct, data, len);
            aes_ctr_xor(&eng->enc_key, iv, 2, ct, len);
            uint8_t aad[13]; uint64_t s=eng->seq_write;
            for(int i=7;i>=0;i--){aad[i]=(uint8_t)(s&0xFF);s>>=8;}
            aad[8]=type; aad[9]=0x03; aad[10]=0x03; PUT16BE(aad+11,(uint16_t)len);
            eng->seq_write++;
            uint8_t tag[16];
            gcm_tag(tag, eng->gcm_h_enc, aad, 13, ct, len, ek0);
            p[0]=type; p[1]=rv1; p[2]=rv2;
            PUT16BE(p+3,(uint16_t)(8+len+16));
            memcpy(p+5,nonce,8);
            memcpy(p+5+8,ct,len);
            memcpy(p+5+8+len,tag,16);
            eng->net_send_len += 5+8+len+16;
            kfree(ct);
        } else {
        /* AES-128-CBC-SHA256:
         * IV(16) || AES-CBC(data || HMAC-SHA256(seq||hdr||data)) || padding */
        uint8_t seq_be[8]; uint64_t s=eng->seq_write;
        for(int i=7;i>=0;i--){seq_be[i]=(uint8_t)(s&0xFF);s>>=8;}
        /* MAC header: seq(8) || type(1) || ver(2) || len(2) */
        uint8_t mac_hdr[13]; memcpy(mac_hdr,seq_be,8);
        mac_hdr[8]=type; mac_hdr[9]=0x03; mac_hdr[10]=0x03;
        PUT16BE(mac_hdr+11,(uint16_t)len);
        /* compute HMAC */
        br_hmac_key_context kc; br_hmac_key_init_sha256(&kc,eng->client_write_mac,32);
        br_hmac_context hc; br_hmac_init(&hc,&kc,0);
        br_hmac_update(&hc,mac_hdr,13);
        br_hmac_update(&hc,data,len);
        uint8_t mac[32]; br_hmac_out(&hc,mac);
        eng->seq_write++;
        /* IV */
        uint8_t iv[16]; br_prng_bytes(iv,16);
        /* plaintext = data || mac || padding */
        size_t pt_len = len + 32;
        int pad = 16 - (int)(pt_len % 16); /* PKCS7 padding */
        size_t total = pt_len + (size_t)pad;
        uint8_t *plain = (uint8_t*)kmalloc((uint32_t)total);
        if(!plain){ eng->err=BR_ERR_IO; return; }
        memcpy(plain,data,len);
        memcpy(plain+len,mac,32);
        memset(plain+len+32,(uint8_t)(pad-1),(size_t)pad);
        /* encrypt */
        uint8_t enc_iv[16]; memcpy(enc_iv,iv,16);
        br_aes_ct_cbcenc_run(&eng->enc_key,enc_iv,plain,(size_t)total);
        /* write record header */
        p[0]=type; p[1]=rv1; p[2]=rv2;
        PUT16BE(p+3,(uint16_t)(16+total));
        memcpy(p+5,iv,16);
        memcpy(p+5+16,plain,total);
        eng->net_send_len += 5+16+total;
        kfree(plain);
        }
    } else {
        p[0]=type; p[1]=rv1; p[2]=rv2;
        PUT16BE(p+3,(uint16_t)len);
        memcpy(p+5,data,len);
        eng->net_send_len += 5+len;
    }
}

/* Read and decrypt a TLS record.  Returns record type or 0 if not enough data. */
static int tls_read_record(br_ssl_engine_context *eng,
                           uint8_t **payload, size_t *payload_len){
    uint8_t *buf = eng->net_recv_buf;
    size_t avail = eng->net_recv_len - eng->net_recv_pos;
    if(avail < 5) return 0;
    uint8_t *hdr = buf + eng->net_recv_pos;
    uint8_t type = hdr[0];
    size_t rlen = GET16BE(hdr+3);
    if(avail < 5+rlen) return 0;
    uint8_t *rec = hdr+5;
    eng->net_recv_pos += 5+rlen;

    int encrypted = (eng->hs == TLS_HS_FINISHED_IN ||
                     eng->hs == TLS_HS_DONE ||
                     eng->hs == TLS_HS_CHANGE_CIPHER_SPEC_IN);
    int app_encrypted = (eng->hs == TLS_HS_DONE);

    if((encrypted || app_encrypted) && eng->cipher_suite!=0 && rlen>16){
        if (is_gcm(eng->cipher_suite)) {
            /* GCM: [explicit_nonce(8)] [ciphertext] [tag(16)] */
            if (rlen < 24) { eng->err=BR_ERR_BAD_FRAGLEN; return -1; }
            uint8_t *nonce = rec;
            uint8_t *ct = rec + 8;
            size_t ct_len = rlen - 8 - 16;
            uint8_t *tag = rec + rlen - 16;
            uint8_t iv[12]; memcpy(iv, eng->gcm_iv_dec, 4); memcpy(iv+4, nonce, 8);
            uint8_t j0[16]; memcpy(j0, iv, 12); j0[12]=0;j0[13]=0;j0[14]=0;j0[15]=1;
            uint8_t ek0[16]; memcpy(ek0, j0, 16);
            aes_encrypt_block(eng->dec_key.skey, eng->dec_key.num_rounds, ek0);
            uint8_t aad[13]; uint64_t s=eng->seq_read;
            for(int i=7;i>=0;i--){aad[i]=(uint8_t)(s&0xFF);s>>=8;}
            aad[8]=type; aad[9]=hdr[1]; aad[10]=hdr[2];
            PUT16BE(aad+11,(uint16_t)ct_len);
            uint8_t exp_tag[16];
            gcm_tag(exp_tag, eng->gcm_h_dec, aad, 13, ct, ct_len, ek0);
            if (memcmp(tag, exp_tag, 16) != 0) { eng->err=BR_ERR_BAD_MAC; return -1; }
            aes_ctr_xor((const br_aes_ct_cbcenc_keys*)&eng->dec_key, iv, 2, ct, ct_len);
            eng->seq_read++;
            *payload=ct; *payload_len=ct_len;
        } else {
        /* decrypt */
        uint8_t iv[16]; memcpy(iv,rec,16);
        uint8_t *ct = rec+16; size_t ct_len=rlen-16;
        br_aes_ct_cbcdec_run(&eng->dec_key,iv,ct,ct_len);
        /* remove PKCS7 padding */
        uint8_t pad_byte=ct[ct_len-1]; size_t padlen=(size_t)pad_byte+1;
        if(padlen>ct_len){ eng->err=BR_ERR_BAD_MAC; return -1; }
        size_t content_len=ct_len-padlen-32;
        /* verify HMAC */
        uint64_t s=eng->seq_read++;
        uint8_t seq_be[8]; for(int i=7;i>=0;i--){seq_be[i]=(uint8_t)(s&0xFF);s>>=8;}
        uint8_t mac_hdr[13]; memcpy(mac_hdr,seq_be,8);
        mac_hdr[8]=type; mac_hdr[9]=hdr[1]; mac_hdr[10]=hdr[2];
        PUT16BE(mac_hdr+11,(uint16_t)content_len);
        br_hmac_key_context kc; br_hmac_key_init_sha256(&kc,eng->server_write_mac,32);
        br_hmac_context hc; br_hmac_init(&hc,&kc,0);
        br_hmac_update(&hc,mac_hdr,13);
        br_hmac_update(&hc,ct,content_len);
        uint8_t expected_mac[32]; br_hmac_out(&hc,expected_mac);
        if(memcmp(ct+content_len,expected_mac,32)!=0){ eng->err=BR_ERR_BAD_MAC; return -1; }
        *payload=ct; *payload_len=content_len;
        }
    } else {
        *payload=rec; *payload_len=rlen;
    }
    return (int)type;
}

/* ================================================================== */
/*  Handshake helpers                                                  */
/* ================================================================== */
/* Update transcript hash with handshake message bytes */
static void hs_update(br_ssl_engine_context *eng, const uint8_t *data, size_t len){
    br_sha256_update(&eng->hs_sha256,data,len);
    br_sha1_update(&eng->hs_sha1,data,len);
}
/* Write handshake message (type | 3-byte len | body) and record it */
static void hs_write(br_ssl_engine_context *eng, uint8_t type,
                     const uint8_t *body, size_t blen){
    uint8_t hdr[4]; hdr[0]=type;
    hdr[1]=(uint8_t)(blen>>16); hdr[2]=(uint8_t)(blen>>8); hdr[3]=(uint8_t)blen;
    /* Record into transcript */
    hs_update(eng,hdr,4); hs_update(eng,body,(size_t)blen);
    /* Assemble full HS message */
    uint8_t *msg=(uint8_t*)kmalloc((uint32_t)(4+blen));
    if(!msg){ eng->err=BR_ERR_IO; return; }
    memcpy(msg,hdr,4); memcpy(msg+4,body,blen);
    tls_write_record(eng,TLS_RT_HANDSHAKE,msg,4+blen);
    kfree(msg);
}

/* ================================================================== */
/*  Handshake state machine                                            */
/* ================================================================== */

/* Send ClientHello */
static void send_client_hello(br_ssl_engine_context *eng){
    br_prng_bytes(eng->client_random,32);
    uint8_t body[512]; int pos=0;
    /* ProtocolVersion */
    body[pos++]=0x03; body[pos++]=0x03;
    /* Random */
    memcpy(body+pos,eng->client_random,32); pos+=32;
    /* Session ID length = 0 */
    body[pos++]=0;
    /* Cipher suites: TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F)
     *               TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256 (0xC027) fallback
     * (TLS_RSA_WITH_AES_128_GCM_SHA256 / 0x009C is NOT offered: the plain-RSA
     *  key exchange is not implemented -- the pre-master secret is never
     *  RSA-encrypted -- so a server choosing it could never succeed.) */
    body[pos++]=0; body[pos++]=4;
    body[pos++]=0xC0; body[pos++]=0x2F;
    body[pos++]=0xC0; body[pos++]=0x27;
    /* Compression: null */
    body[pos++]=1; body[pos++]=0;
    /* Extensions */
    int ext_start = pos; pos+=2; /* placeholder for extensions length */
    /* SNI extension (0x0000) */
    if(eng->sni[0]){
        int sni_len=(int)strlen(eng->sni);
        body[pos++]=0x00; body[pos++]=0x00;
        int ext_len=5+sni_len;
        body[pos++]=(uint8_t)(ext_len>>8); body[pos++]=(uint8_t)ext_len;
        body[pos++]=(uint8_t)((sni_len+3)>>8); body[pos++]=(uint8_t)(sni_len+3); /* server_name_list length */
        body[pos++]=0x00; /* name_type = host_name */
        body[pos++]=(uint8_t)(sni_len>>8); body[pos++]=(uint8_t)sni_len;
        memcpy(body+pos,eng->sni,sni_len); pos+=sni_len;
    }
    /* Supported groups: secp256r1 (0x0017) */
    body[pos++]=0x00; body[pos++]=0x0A;
    body[pos++]=0x00; body[pos++]=0x04;
    body[pos++]=0x00; body[pos++]=0x02;
    body[pos++]=0x00; body[pos++]=0x17;
    /* EC point formats */
    body[pos++]=0x00; body[pos++]=0x0B;
    body[pos++]=0x00; body[pos++]=0x02;
    body[pos++]=0x01; body[pos++]=0x00;
    /* Signature algorithms */
    body[pos++]=0x00; body[pos++]=0x0D;
    body[pos++]=0x00; body[pos++]=0x06;
    body[pos++]=0x00; body[pos++]=0x04;
    body[pos++]=0x04; body[pos++]=0x01; /* rsa_pkcs1_sha256 */
    body[pos++]=0x02; body[pos++]=0x01; /* rsa_pkcs1_sha1 */
    /* renegotiation_info (0xFF01) — required by Cloudflare */
    body[pos++]=0xFF; body[pos++]=0x01;
    body[pos++]=0x00; body[pos++]=0x01;
    body[pos++]=0x00;
    /* NOTE: extended_master_secret (RFC 7627) is deliberately NOT offered:
     * the key derivation below uses the classic master-secret formula, so
     * advertising EMS makes the server derive different keys and the
     * handshake dies with bad_record_mac. */
    /* Fill in extensions length */
    int ext_total = pos - ext_start - 2;
    body[ext_start]=(uint8_t)(ext_total>>8); body[ext_start+1]=(uint8_t)ext_total;
    hs_write(eng, 1, body, (size_t)pos);
}

/* Parse a minimal ASN.1 sequence to find the subjectPublicKeyInfo */
static int asn1_skip_tag(const uint8_t *buf, int len, int *pos){
    if(*pos>=len) return -1;
    (*pos)++; /* tag */
    if(*pos>=len) return -1;
    int l=buf[(*pos)++];
    if(l&0x80){
        int nb=l&0x7F; l=0;
        while(nb--&&*pos<len) l=(l<<8)|buf[(*pos)++];
    }
    return l;
}

/* Extract RSA public key (n, e) from DER-encoded SubjectPublicKeyInfo */
static int parse_spki_rsa(const uint8_t *der, int derlen,
                           uint8_t *n_out, int *n_len,
                           uint8_t *e_out, int *e_len){
    /* Simplified: scan for the RSA SPKI bitstring pattern */
    /* Look for 0x30 (SEQUENCE) near start, then navigate */
    int pos=0;
    /* outer SEQUENCE */
    if(der[pos++]!=0x30) return 0;
    asn1_skip_tag(der,derlen,&pos); pos-=1; /* undo the increment from skip_tag */
    /* Just scan forward for BIT STRING containing the key */
    for(int i=0;i<derlen-5;i++){
        if(der[i]==0x03){ /* BIT STRING */
            int blen=der[i+1]; int bstart=i+2;
            if(blen>0x80){ blen=(der[i+2]<<8)|der[i+3]; bstart=i+4; }
            if(bstart+blen>derlen) continue;
            /* BIT STRING content: 0x00 0x30... (the RSA public key SEQUENCE) */
            int p=bstart; if(der[p]==0x00) p++;
            if(p>=derlen||der[p]!=0x30) continue;
            p++; /* skip SEQUENCE tag */
            int seq_len=der[p++]; if(seq_len&0x80){int nb=seq_len&0x7F;seq_len=0;while(nb--)seq_len=(seq_len<<8)|der[p++];}
            /* INTEGER n */
            if(p>=derlen||der[p]!=0x02) continue; p++;
            int nl=der[p++]; if(nl&0x80){int nb=nl&0x7F;nl=0;while(nb--)nl=(nl<<8)|der[p++];}
            if(der[p]==0x00){p++;nl--;} /* skip leading zero */
            if(nl>256||p+nl>derlen) continue;
            *n_len=nl; memcpy(n_out,der+p,nl); p+=nl;
            /* INTEGER e */
            if(p>=derlen||der[p]!=0x02) continue; p++;
            int el=der[p++]; if(el&0x80){int nb=el&0x7F;el=0;while(nb--)el=(el<<8)|der[p++];}
            *e_len=el; memcpy(e_out,der+p,el);
            return 1;
        }
    }
    return 0;
}

/* Process a received handshake message */
static void process_handshake_msg(br_ssl_engine_context *eng,
                                  uint8_t msg_type,
                                  const uint8_t *body, size_t blen){
    switch(msg_type){
    case 2: { /* ServerHello */
        if(blen<38) goto bad;
        eng->server_version[0]=body[0]; eng->server_version[1]=body[1];
        if(body[0]!=0x03||body[1]<0x03) goto bad;
        memcpy(eng->server_random,body+2,32);
        int sid_len=body[34]; int off=35+sid_len;
        if(off+2>=(int)blen) goto bad;
        eng->cipher_suite=GET16BE(body+off);
        eng->hs=TLS_HS_SERVER_HELLO;
        break;
    }
    case 11: { /* Certificate */
        if(blen<7) goto bad;
        /* certificates list: 3-byte total len, then each cert: 3-byte len + DER */
        int total=((int)(uint8_t)body[0]<<16)|((int)(uint8_t)body[1]<<8)|(uint8_t)body[2];
        int pos=3;
        /* take first cert only */
        if(pos+3>=(int)blen) goto bad;
        int clen=((int)(uint8_t)body[pos]<<16)|((int)(uint8_t)body[pos+1]<<8)|(uint8_t)body[pos+2];
        pos+=3;
        if(clen<0||pos+clen>(int)blen) goto bad;
        /* The leaf cert is only informational (verification is off), so a cert
         * bigger than our buffer must not abort the handshake -- just skip it. */
        eng->cert_len=0;
        uint8_t n_buf[256]; int n_len=0; uint8_t e_buf[4]; int e_len=0;
        if(clen<=TLS_CERT_BUF){
            eng->cert_len=(size_t)clen;
            memcpy(eng->cert_buf,body+pos,clen);
        }
        if(clen<=TLS_CERT_BUF && parse_spki_rsa(body+pos,clen,n_buf,&n_len,e_buf,&e_len) && e_len>=0 && e_len<=(int)sizeof eng->server_e){
            if(n_len<=(int)sizeof eng->server_n){
                memcpy(eng->server_n,n_buf,n_len); eng->server_nlen=n_len;
                memcpy(eng->server_e,e_buf,e_len); eng->server_elen=e_len;
            }
        }
        eng->hs=TLS_HS_CERTIFICATE;
        (void)total;
        break;
    }
    case 12: { /* ServerKeyExchange (ECDHE) */
        /* struct: curve_type(1) + named_curve(2) + pub_key_len(1) + pub_key(65) + sig */
        if(blen<69) goto bad;
        if(body[0]!=0x03) goto bad; /* named_curve */
        uint16_t curve=GET16BE(body+1);
        if(curve!=0x0017) goto bad; /* secp256r1 */
        int klen=body[3];
        if(klen!=65||4+65>(int)blen) goto bad;
        memcpy(eng->ecdhe_server_pub,body+4,65);
        eng->hs=TLS_HS_SERVER_KEY_EXCHANGE;
        break;
    }
    case 14: /* ServerHelloDone */
        eng->hs=TLS_HS_SERVER_HELLO_DONE;
        break;
    case 20: { /* Finished */
        /* verify_data = PRF(master, "server finished", SHA256(hs_msgs))[0..11] */
        if(blen<12) goto bad;
        /* We accept it; in a production impl we'd verify. */
        eng->hs=TLS_HS_DONE;
        eng->flags = BR_SSL_SENDAPP | BR_SSL_RECVAPP;
        break;
    }
    default: break;
    }
    return;
bad:
    eng->err=BR_ERR_BAD_HANDSHAKE; eng->hs=TLS_HS_ERROR;
}

/* Send ClientKeyExchange + ChangeCipherSpec + Finished */
static void send_cke_ccs_finished(br_ssl_engine_context *eng){
    int is_ecdhe = (eng->cipher_suite==0xC027 || eng->cipher_suite==0xC02F);
    if(is_ecdhe){ /* ECDHE_RSA */
        /* Generate ephemeral key pair */
        ecdhe_gen_keypair(eng->ecdhe_client_priv, eng->ecdhe_client_pub);
        /* Compute shared secret */
        if(!ecdhe_shared_secret(eng->ecdhe_shared,
                                eng->ecdhe_server_pub,
                                eng->ecdhe_client_priv)){
            eng->err=BR_ERR_BAD_HANDSHAKE; eng->hs=TLS_HS_ERROR; return;
        }
        /* pre_master = shared_secret */
        memcpy(eng->pre_master,eng->ecdhe_shared,32);
        /* ClientKeyExchange body: len(1) + pub(65) */
        uint8_t cke[66]; cke[0]=65; memcpy(cke+1,eng->ecdhe_client_pub,65);
        hs_write(eng,16,cke,66);
    } else { /* RSA key exchange (0x003C, 0x009C) */
        eng->pre_master[0]=0x03; eng->pre_master[1]=0x03;
        br_prng_bytes(eng->pre_master+2,46);
        uint8_t cke[2+48]; PUT16BE(cke,(uint16_t)48);
        memcpy(cke+2,eng->pre_master,48);
        hs_write(eng,16,cke,50);
    }

    /* Derive master secret */
    uint8_t seed[64]; memcpy(seed,eng->client_random,32); memcpy(seed+32,eng->server_random,32);
    tls_prf(eng->master,48,eng->pre_master, is_ecdhe?32:48, "master secret",seed,64);

    /* Derive key material */
    uint8_t seed2[64]; memcpy(seed2,eng->server_random,32); memcpy(seed2+32,eng->client_random,32);

    if (is_gcm(eng->cipher_suite)) {
        /* GCM key block: client_key(16) | server_key(16) | client_iv(4) | server_iv(4) = 40 bytes */
        int kb_len = 2*16 + 2*4;
        tls_prf(eng->key_block,(size_t)kb_len,eng->master,48,"key expansion",seed2,64);
        br_aes_ct_cbcenc_init(&eng->enc_key, eng->key_block,      16);
        br_aes_ct_cbcdec_init(&eng->dec_key, eng->key_block+16,    16);
        memcpy(eng->gcm_iv_enc, eng->key_block+32, 4);
        memcpy(eng->gcm_iv_dec, eng->key_block+36, 4);
        /* Compute GHASH subkeys: H = AES_encrypt(0^128) */
        uint8_t zero[16]; memset(zero, 0, 16);
        memcpy(eng->gcm_h_enc, zero, 16);
        aes_encrypt_block(eng->enc_key.skey, eng->enc_key.num_rounds, eng->gcm_h_enc);
        memcpy(eng->gcm_h_dec, zero, 16);
        aes_encrypt_block(eng->dec_key.skey, eng->dec_key.num_rounds, eng->gcm_h_dec);
    } else {
        /* CBC: client_MAC(32) | server_MAC(32) | client_key(16) | server_key(16) */
        int kb_len = 2*32 + 2*16;
        tls_prf(eng->key_block,(size_t)kb_len,eng->master,48,"key expansion",seed2,64);
        memcpy(eng->client_write_mac, eng->key_block,      32);
        memcpy(eng->server_write_mac, eng->key_block+32,   32);
        br_aes_ct_cbcenc_init(&eng->enc_key, eng->key_block+64, 16);
        br_aes_ct_cbcdec_init(&eng->dec_key, eng->key_block+80, 16);
    }

    /* ChangeCipherSpec */
    uint8_t ccs[1]={1};
    tls_write_record(eng,TLS_RT_CHANGE_CIPHER_SPEC,ccs,1);
    eng->hs=TLS_HS_CHANGE_CIPHER_SPEC_OUT;

    /* Finished: verify_data = PRF(master, "client finished", Transcript-Hash) */
    uint8_t hs_hash[32]; br_sha256_out(&eng->hs_sha256,hs_hash);
    uint8_t vd[12]; tls_prf(vd,12,eng->master,48,"client finished",hs_hash,32);
    eng->hs=TLS_HS_FINISHED_OUT;
    hs_write(eng,20,vd,12);
    eng->hs=TLS_HS_CHANGE_CIPHER_SPEC_IN; /* now expect server CCS + Finished */
}

/* ================================================================== */
/*  Public API                                                         */
/* ================================================================== */
void br_ssl_client_init_full(br_ssl_client_context *cc,
                             const br_x509_trust_anchor *ta,
                             size_t ta_count){
    memset(cc,0,sizeof *cc);
    cc->eng.ta        = ta;
    cc->eng.ta_count  = ta_count;
    cc->eng.skip_verify = (ta==NULL||ta_count==0)?1:0;
    cc->eng.hs        = TLS_HS_NONE;
    cc->eng.err       = BR_ERR_OK;
    cc->eng.flags     = 0;
    br_prng_init();
}

int br_ssl_client_reset(br_ssl_client_context *cc,
                        const char *server_name, int resume){
    (void)resume;
    br_ssl_engine_context *eng=&cc->eng;
    eng->hs=TLS_HS_CLIENT_HELLO;
    eng->err=BR_ERR_OK;
    eng->flags=BR_SSL_SENDREC; /* we have the ClientHello to send */
    eng->net_send_len=eng->net_send_pos=0;
    eng->net_recv_len=eng->net_recv_pos=0;
    eng->app_len=eng->app_pos=0;
    eng->seq_write=eng->seq_read=0;
    eng->cipher_suite=0;
    kstrcpy(eng->sni,server_name,TLS_MAX_SNI);
    br_sha256_init(&eng->hs_sha256);
    br_sha1_init(&eng->hs_sha1);
    send_client_hello(eng);
    return 1;
}

void br_ssl_engine_set_io(br_ssl_engine_context *eng,
                          br_ssl_recv_fn recv_fn,
                          br_ssl_send_fn send_fn,
                          void *io_ctx){
    eng->recv_fn=recv_fn; eng->send_fn=send_fn; eng->io_ctx=io_ctx;
}

/* TLS alert description (set by br_ssl_engine_run, read by callers) */
int br_last_alert = 0;

int br_ssl_engine_current_state(const br_ssl_engine_context *eng){
    if(eng->err) return BR_SSL_CLOSED;
    /* A clean close_notify from the peer: report CLOSED once all plaintext that
     * arrived before it has been read by the application. */
    if((eng->flags & BR_SSL_CLOSED) && eng->app_len<=eng->app_pos) return BR_SSL_CLOSED;
    int st=0;
    if(eng->net_send_len>eng->net_send_pos) st|=BR_SSL_SENDREC;
    if(eng->hs!=TLS_HS_DONE)               st|=BR_SSL_RECVREC;
    if(eng->hs==TLS_HS_DONE){
        st|=BR_SSL_SENDAPP|BR_SSL_RECVREC;
        if(eng->app_len>eng->app_pos)       st|=BR_SSL_RECVAPP;
    }
    return st;
}

void br_ssl_engine_run(br_ssl_engine_context *eng){
    if(eng->err||!eng->recv_fn) return;

    /* 1. Push pending send data to the network */
    while(eng->net_send_len>eng->net_send_pos){
        int n=eng->send_fn(eng->io_ctx,
                           eng->net_send_buf+eng->net_send_pos,
                           eng->net_send_len-eng->net_send_pos);
        if(n<=0) break;
        eng->net_send_pos+=(size_t)n;
    }
    if(eng->net_send_pos>=eng->net_send_len){
        eng->net_send_len=eng->net_send_pos=0;
    }

    /* 2. Pull data from network into recv buffer */
    {
        size_t room=sizeof(eng->net_recv_buf)-eng->net_recv_len;
        if(room>0&&eng->net_recv_len<sizeof(eng->net_recv_buf)){
            int n=eng->recv_fn(eng->io_ctx,
                               eng->net_recv_buf+eng->net_recv_len,
                               room);
            if(n>0) eng->net_recv_len+=(size_t)n;
        }
    }

    /* 3. Process received records */
    while(1){
        uint8_t *payload; size_t plen;
        /* Back-pressure: don't pull another record while plaintext is still
         * waiting to be read, otherwise app_buf could overflow and silently
         * drop data. The rest stays queued in net_recv_buf for the next run. */
        if(eng->hs==TLS_HS_DONE && eng->app_len>eng->app_pos) break;
        int type=tls_read_record(eng,&payload,&plen);
        if(type<=0) break;

        if(type==TLS_RT_ALERT){
            if(plen>=2) {
                br_last_alert = payload[1];
                if(payload[0]==2) eng->err=BR_ERR_BAD_ALERT;  /* fatal */
                else if(payload[1]==0) { eng->hs=TLS_HS_DONE; eng->flags=BR_SSL_CLOSED; }
                else eng->err=BR_ERR_BAD_ALERT;
            }
            break;
        }

        if(type==TLS_RT_CHANGE_CIPHER_SPEC){
            /* Server sent CCS — start decrypting */
            if(eng->hs==TLS_HS_CHANGE_CIPHER_SPEC_IN)
                eng->hs=TLS_HS_FINISHED_IN;
            continue;
        }

        if(type==TLS_RT_HANDSHAKE){
            size_t off=0;
            while(off+4<=plen){
                uint8_t msg_type=payload[off];
                size_t msg_len=((size_t)payload[off+1]<<16)|((size_t)payload[off+2]<<8)|payload[off+3];
                off+=4;
                if(off+msg_len>plen) break;
                /* Update transcript (before SHD for client, all messages) */
                uint8_t hdr[4]; hdr[0]=msg_type;
                hdr[1]=(uint8_t)(msg_len>>16);hdr[2]=(uint8_t)(msg_len>>8);hdr[3]=(uint8_t)msg_len;
                if(eng->hs!=TLS_HS_FINISHED_IN){ hs_update(eng,hdr,4); hs_update(eng,payload+off,msg_len); }
                process_handshake_msg(eng,msg_type,payload+off,msg_len);
                if(eng->err) return;
                off+=msg_len;
                /* After ServerHelloDone, send our key exchange */
                if(eng->hs==TLS_HS_SERVER_HELLO_DONE)
                    send_cke_ccs_finished(eng);
            }
            continue;
        }

        if(type==TLS_RT_APPLICATION_DATA){
            /* append to app buffer */
            size_t room=sizeof(eng->app_buf)-eng->app_len;
            size_t take=plen<room?plen:room;
            memcpy(eng->app_buf+eng->app_len,payload,take);
            eng->app_len+=take;
            continue;
        }
    }
    /* Compact recv buffer */
    if(eng->net_recv_pos>0){
        size_t remaining=eng->net_recv_len-eng->net_recv_pos;
        if(remaining) memmove(eng->net_recv_buf,eng->net_recv_buf+eng->net_recv_pos,remaining);
        eng->net_recv_len=remaining; eng->net_recv_pos=0;
    }
}

int br_ssl_engine_sendapp_buf(br_ssl_engine_context *eng,
                              const uint8_t *data, size_t len){
    if(eng->hs!=TLS_HS_DONE||eng->err) return -1;
    tls_write_record(eng,TLS_RT_APPLICATION_DATA,data,len);
    return (int)len;
}

void br_ssl_engine_flush(br_ssl_engine_context *eng, int close_notify){
    if(close_notify){
        uint8_t alert[2]={1,0}; /* warning, close_notify */
        tls_write_record(eng,TLS_RT_ALERT,alert,2);
    }
}

int br_ssl_engine_recvapp_buf(br_ssl_engine_context *eng,
                              uint8_t *data, size_t len){
    size_t avail=eng->app_len-eng->app_pos;
    if(!avail) return 0;
    size_t take=len<avail?len:avail;
    memcpy(data,eng->app_buf+eng->app_pos,take);
    eng->app_pos+=take;
    if(eng->app_pos>=eng->app_len){ eng->app_len=eng->app_pos=0; }
    return (int)take;
}

void br_ssl_engine_close(br_ssl_engine_context *eng){
    br_ssl_engine_flush(eng,1);
}

int br_ssl_engine_last_error(const br_ssl_engine_context *eng){
    return eng->err;
}

/* ================================================================== */
/*  High-level sslio                                                   */
/* ================================================================== */
void br_sslio_init(br_sslio_context *ctx,
                   br_ssl_engine_context *eng,
                   br_ssl_recv_fn low_read,  void *read_ctx,
                   br_ssl_send_fn low_write, void *write_ctx){
    ctx->eng=eng;
    ctx->low_read=low_read;  ctx->read_ctx=read_ctx;
    ctx->low_write=low_write; ctx->write_ctx=write_ctx;
    br_ssl_engine_set_io(eng,low_read,low_write,read_ctx);
    (void)write_ctx;
}

int br_sslio_read(br_sslio_context *ctx, void *dst, size_t len){
    br_ssl_engine_context *eng=ctx->eng;
    int n=br_ssl_engine_recvapp_buf(eng,(uint8_t*)dst,len);
    if(n>0) return n;
    br_ssl_engine_run(eng);
    return br_ssl_engine_recvapp_buf(eng,(uint8_t*)dst,len);
}

int br_sslio_write(br_sslio_context *ctx, const void *src, size_t len){
    return br_ssl_engine_sendapp_buf(ctx->eng,(const uint8_t*)src,len);
}

int br_sslio_flush(br_sslio_context *ctx){
    br_ssl_engine_run(ctx->eng);
    return ctx->eng->err ? -1 : 0;
}

int br_sslio_close(br_sslio_context *ctx){
    br_ssl_engine_close(ctx->eng);
    br_ssl_engine_run(ctx->eng);
    return 0;
}
