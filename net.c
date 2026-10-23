/* net -- one machine's substrate, reachable from another, live.
 *
 * Shared memory ends at the edge of a machine. This is what stands at that
 * edge: a daemon that serves its own machine's segment -- the registry, process
 * control, the events -- to the machines it trusts, over one persistent,
 * authenticated, encrypted connection each, and the client verbs that use it.
 *
 *   sub-net keygen                         this machine's key pair
 *   sub-net trust pc pc.pub --at 10.0.0.2:7341
 *   sub-net serve --bind 10.0.0.1:7341     serve this machine's substrate
 *   sub-net ps pc                          its registry, read from here
 *   sub-net run pc --name w -- cmd ...     start a process there
 *   sub-net stop pc w                      and stop it
 *   sub-net events pc                      its events, pushed as they happen
 *   sub-net listen warp -- cmd ...         a named channel, served by cmd
 *   sub-net connect pc warp                talk to the channel on pc
 *
 * The transport is the seam in substrate.h, and the one here is Noise IK:
 * every machine is known by an X25519 key, the caller proves its key and the
 * server proves its own in the same two messages, and each direction then gets
 * its own ChaCha20-Poly1305 key. No certificate authority, no passwords, no
 * third-party code -- the primitives are below, each checked against its RFC
 * by `sub-net selftest`, which the suite runs.
 *
 * What it will not do is listen on everything. --bind names one address, and
 * that address must be loopback or private (RFC 1918, the 100.64/10 range a
 * private overlay hands out, link-local, IPv6 ULA). 0.0.0.0 is refused: this
 * serves process control, and process control does not belong on the
 * internet.
 */
#include "substrate.h"
#include <pthread.h>
#include <poll.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <limits.h>
#include <stdarg.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

typedef uint8_t u8;

/* ================================================================ bytes == */

static uint32_t ld32(const u8 *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void st32(u8 *p, uint32_t v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static void st64(u8 *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (8 * i)); }

/* A key that is dead must not linger in memory the compiler decided was dead too. */
static void wipe(void *p, size_t n) { volatile u8 *v = p; while (n--) *v++ = 0; }

static int ct_eq(const u8 *a, const u8 *b, size_t n) {
    u8 d = 0;
    for (size_t i = 0; i < n; i++) d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

/* Randomness or nothing: a key from a weak source is worse than no key. */
static void random_bytes(u8 *out, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        ssize_t k = read(fd, out + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    if (fd >= 0) close(fd);
    if (got != n) { fprintf(stderr, "net: no randomness from /dev/urandom -- refusing\n"); abort(); }
}

static void hexenc(const u8 *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[in[i] >> 4]; out[2 * i + 1] = H[in[i] & 15]; }
    out[2 * n] = 0;
}
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hexdec(const char *s, u8 *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (u8)(a << 4 | b);
    }
    return 0;
}

/* =============================================================== SHA-256 == */

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

typedef struct { uint32_t s[8]; uint64_t n; u8 buf[64]; size_t k; } sha256_ctx;

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t st[8], const u8 *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

static void sha256_init(sha256_ctx *x) {
    static const uint32_t IV[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(x->s, IV, sizeof IV); x->n = 0; x->k = 0;
}
static void sha256_update(sha256_ctx *x, const void *data, size_t len) {
    const u8 *p = data;
    x->n += len;
    while (len) {
        size_t take = 64 - x->k < len ? 64 - x->k : len;
        memcpy(x->buf + x->k, p, take);
        x->k += take; p += take; len -= take;
        if (x->k == 64) { sha256_block(x->s, x->buf); x->k = 0; }
    }
}
static void sha256_final(sha256_ctx *x, u8 out[32]) {
    uint64_t bits = x->n * 8;
    u8 one = 0x80, zero = 0;
    sha256_update(x, &one, 1);
    while (x->k != 56) sha256_update(x, &zero, 1);
    u8 len[8];
    for (int i = 0; i < 8; i++) len[i] = (u8)(bits >> (56 - 8 * i));
    sha256_update(x, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4*i] = (u8)(x->s[i] >> 24); out[4*i+1] = (u8)(x->s[i] >> 16);
        out[4*i+2] = (u8)(x->s[i] >> 8); out[4*i+3] = (u8)x->s[i];
    }
    wipe(x, sizeof *x);
}

/* HMAC over the concatenation of two parts, so HKDF needs no scratch buffer. */
static void hmac256(const u8 *key, size_t klen, const u8 *m1, size_t l1,
                    const u8 *m2, size_t l2, u8 out[32]) {
    u8 k[64] = {0}, pad[64], inner[32];
    if (klen > 64) { sha256_ctx t; sha256_init(&t); sha256_update(&t, key, klen); sha256_final(&t, k); }
    else memcpy(k, key, klen);
    sha256_ctx x;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&x); sha256_update(&x, pad, 64);
    sha256_update(&x, m1, l1); if (l2) sha256_update(&x, m2, l2);
    sha256_final(&x, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&x); sha256_update(&x, pad, 64); sha256_update(&x, inner, 32);
    sha256_final(&x, out);
    wipe(k, sizeof k); wipe(pad, sizeof pad); wipe(inner, sizeof inner);
}

/* Noise's HKDF: two 32-byte outputs from a chaining key and input material. */
static void hkdf2(const u8 ck[32], const u8 *ikm, size_t ilen, u8 o1[32], u8 o2[32]) {
    u8 prk[32], one = 1, two = 2, t[32];
    hmac256(ck, 32, ikm, ilen, NULL, 0, prk);
    hmac256(prk, 32, &one, 1, NULL, 0, t);
    hmac256(prk, 32, t, 32, &two, 1, o2);
    memcpy(o1, t, 32);
    wipe(prk, sizeof prk); wipe(t, sizeof t);
}

/* ======================================================= ChaCha20 (8439) == */

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define QR(a, b, c, d) \
    a += b; d ^= a; d = ROL32(d, 16); c += d; b ^= c; b = ROL32(b, 12); \
    a += b; d ^= a; d = ROL32(d, 8);  c += d; b ^= c; b = ROL32(b, 7);

static void chacha_block(const u8 key[32], uint32_t ctr, const u8 nonce[12], u8 out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = ctr; s[13] = ld32(nonce); s[14] = ld32(nonce + 4); s[15] = ld32(nonce + 8);
    memcpy(x, s, sizeof s);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]) QR(x[1], x[5], x[9],  x[13])
        QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12])
        QR(x[2], x[7], x[8],  x[13]) QR(x[3], x[4], x[9],  x[14])
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
    wipe(s, sizeof s); wipe(x, sizeof x);
}

static void chacha_xor(const u8 key[32], uint32_t ctr, const u8 nonce[12],
                       const u8 *in, u8 *out, size_t n) {
    u8 ks[64];
    while (n) {
        chacha_block(key, ctr++, nonce, ks);
        size_t take = n < 64 ? n : 64;
        for (size_t i = 0; i < take; i++) out[i] = in[i] ^ ks[i];
        in += take; out += take; n -= take;
    }
    wipe(ks, sizeof ks);
}

/* ======================================================= Poly1305 (8439) == */
/* 26-bit limbs, after poly1305-donna's 32-bit form: no 128-bit integers, so
 * the same code is right on every compiler this repo builds with. */

typedef struct { uint32_t r[5], h[5], pad[4]; size_t left; u8 buf[16]; } poly_ctx;

static void poly_init(poly_ctx *p, const u8 key[32]) {
    p->r[0] = (ld32(key +  0)     ) & 0x3ffffff;
    p->r[1] = (ld32(key +  3) >> 2) & 0x3ffff03;
    p->r[2] = (ld32(key +  6) >> 4) & 0x3ffc0ff;
    p->r[3] = (ld32(key +  9) >> 6) & 0x3f03fff;
    p->r[4] = (ld32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = ld32(key + 16 + 4 * i);
    p->left = 0;
}

static void poly_blocks(poly_ctx *p, const u8 *m, size_t n, uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    while (n >= 16) {
        h0 += (ld32(m +  0)     ) & 0x3ffffff;
        h1 += (ld32(m +  3) >> 2) & 0x3ffffff;
        h2 += (ld32(m +  6) >> 4) & 0x3ffffff;
        h3 += (ld32(m +  9) >> 6) & 0x3ffffff;
        h4 += (ld32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;
        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += 16; n -= 16;
    }
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(poly_ctx *p, const u8 *m, size_t n) {
    if (p->left) {
        size_t take = 16 - p->left < n ? 16 - p->left : n;
        memcpy(p->buf + p->left, m, take);
        p->left += take; m += take; n -= take;
        if (p->left < 16) return;
        poly_blocks(p, p->buf, 16, 1u << 24);
        p->left = 0;
    }
    size_t whole = n & ~(size_t)15;
    if (whole) { poly_blocks(p, m, whole, 1u << 24); m += whole; n -= whole; }
    if (n) { memcpy(p->buf, m, n); p->left = n; }
}

static void poly_final(poly_ctx *p, u8 tag[16]) {
    if (p->left) {
        p->buf[p->left] = 1;
        for (size_t i = p->left + 1; i < 16; i++) p->buf[i] = 0;
        poly_blocks(p, p->buf, 16, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    /* h - p, kept only if it did not go negative: constant time, no branch */
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    h0 = (h0      ) | (h1 << 26);
    h1 = (h1 >>  6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 <<  8);
    uint64_t f;
    f = (uint64_t)h0 + p->pad[0];             h0 = (uint32_t)f;
    f = (uint64_t)h1 + p->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + p->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + p->pad[3] + (f >> 32); h3 = (uint32_t)f;
    st32(tag, h0); st32(tag + 4, h1); st32(tag + 8, h2); st32(tag + 12, h3);
    wipe(p, sizeof *p);
}

/* ======================================= ChaCha20-Poly1305 AEAD (8439) == */

static void aead_tag(const u8 key[32], const u8 nonce[12], const u8 *ad, size_t adl,
                     const u8 *ct, size_t ctl, u8 tag[16]) {
    u8 block[64], zeros[16] = {0}, lens[16];
    chacha_block(key, 0, nonce, block);
    poly_ctx p; poly_init(&p, block);
    poly_update(&p, ad, adl);  if (adl % 16) poly_update(&p, zeros, 16 - adl % 16);
    poly_update(&p, ct, ctl);  if (ctl % 16) poly_update(&p, zeros, 16 - ctl % 16);
    st64(lens, adl); st64(lens + 8, ctl);
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
    wipe(block, sizeof block);
}

/* out = ciphertext || 16-byte tag. Noise's nonce: 4 zero bytes, then n little-endian. */
static void aead_seal(const u8 key[32], uint64_t n, const u8 *ad, size_t adl,
                      const u8 *pt, size_t ptl, u8 *out) {
    u8 nonce[12] = {0};
    st64(nonce + 4, n);
    chacha_xor(key, 1, nonce, pt, out, ptl);
    aead_tag(key, nonce, ad, adl, out, ptl, out + ptl);
}

static int aead_open(const u8 key[32], uint64_t n, const u8 *ad, size_t adl,
                     const u8 *in, size_t inl, u8 *out) {
    if (inl < 16) return -1;
    u8 nonce[12] = {0}, tag[16];
    st64(nonce + 4, n);
    aead_tag(key, nonce, ad, adl, in, inl - 16, tag);
    if (!ct_eq(tag, in + inl - 16, 16)) return -1;      /* nothing decrypted on a bad tag */
    chacha_xor(key, 1, nonce, in, out, inl - 16);
    return 0;
}

/* ================================================== X25519 (RFC 7748) == */
/* TweetNaCl's field arithmetic: sixteen 16-bit limbs in int64s, a ladder that
 * swaps by mask rather than by branch. Small, slow enough not to matter at two
 * handshakes a connection, and checked against RFC 7748's own vectors. */

typedef int64_t gf[16];

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void sel25519(gf p, gf q, int64_t b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) { int64_t t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(u8 *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (u8)(t[i] & 0xff); o[2 * i + 1] = (u8)(t[i] >> 8); }
}
static void unpack25519(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fM(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fS(gf o, const gf a) { fM(o, a, a); }
static void inv25519(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { fS(c, c); if (a != 2 && a != 4) fM(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void x25519(u8 q[32], const u8 n[32], const u8 p[32]) {
    static const gf k121665 = {0xDB41, 1};
    u8 z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (u8)((n[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fA(e, a, c); fZ(a, a, c); fA(c, b, d); fZ(b, b, d);
        fS(d, e); fS(f, a); fM(a, c, a); fM(c, b, e);
        fA(e, a, c); fZ(a, a, c); fS(b, a); fZ(c, d, f);
        fM(a, c, k121665); fA(a, a, d); fM(c, c, a); fM(a, d, f);
        fM(d, b, x); fS(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    inv25519(c, c);
    fM(a, a, c);
    pack25519(q, a);
    wipe(z, sizeof z);
}

static void x25519_base(u8 pub[32], const u8 sec[32]) {
    static const u8 nine[32] = {9};
    x25519(pub, sec, nine);
}

/* A peer that sends a low-order point makes every shared secret zero. Refuse it. */
static int dh(u8 out[32], const u8 sec[32], const u8 pub[32]) {
    static const u8 zero[32] = {0};
    x25519(out, sec, pub);
    return ct_eq(out, zero, 32) ? -1 : 0;
}

/* ============================================================== keys == */

/* Where this machine's keys and trust list live. SUB_NET_DIR wins (the suite
 * uses throwaway ones); otherwise beside the change lane on macOS, and under
 * XDG_CONFIG_HOME elsewhere. Never inside the repo. */
static void net_dir(char *out, size_t cap) {
    const char *e = getenv("SUB_NET_DIR");
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
#if defined(__APPLE__)
    snprintf(out, cap, "%s/Library/Application Support/substrate/net", home);
#else
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(out, cap, "%s/substrate/net", x);
    else         snprintf(out, cap, "%s/.config/substrate/net", home);
#endif
}

static void net_file(const char *leaf, char *out, size_t cap) {
    char d[PATH_MAX]; net_dir(d, sizeof d);
    snprintf(out, cap, "%s/%s", d, leaf);
}

static int mkdir_p(const char *path, mode_t mode) {
    char b[PATH_MAX]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(b, mode); *p = '/'; }
    return (mkdir(b, mode) == 0 || errno == EEXIST) ? 0 : -1;
}

/* Read 64 hex characters from the start of a file. */
static int read_hex32(const char *path, u8 out[32]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128] = {0};
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    int rc = ok && strlen(line) >= 64 ? hexdec(line, out, 32) : -1;
    wipe(line, sizeof line);
    return rc;
}

static int load_key(u8 sk[32], u8 pk[32], char *err, size_t cap) {
    char path[PATH_MAX]; net_file("key", path, sizeof path);
    if (read_hex32(path, sk) < 0) {
        snprintf(err, cap, "no key pair in %s -- run 'sub-net keygen'", path);
        return -1;
    }
    x25519_base(pk, sk);
    return 0;
}

/* The trust list: one line per machine, "name pubkey-hex [address:port]".
 * Being on it is what lets a caller in; the address is where dial() goes. */
typedef struct { char name[SUB_NET_NAMELEN]; u8 pk[32]; char addr[96]; } trusted;

static int trust_scan(const char *name, const u8 *pk, trusted *out) {
    char path[PATH_MAX]; net_file("peers", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    while (found < 0 && fgets(line, sizeof line, f)) {
        trusted t; memset(&t, 0, sizeof t);
        char nm[64] = {0}, hex[80] = {0}, ad[96] = {0};
        if (line[0] == '#') continue;
        int k = sscanf(line, "%63s %79s %95s", nm, hex, ad);
        if (k < 2 || strlen(hex) != 64 || strlen(nm) >= SUB_NET_NAMELEN) continue;
        if (hexdec(hex, t.pk, 32) < 0) continue;
        snprintf(t.name, sizeof t.name, "%s", nm);
        if (k == 3) snprintf(t.addr, sizeof t.addr, "%s", ad);
        if ((name && !strcmp(name, t.name)) || (pk && ct_eq(pk, t.pk, 32))) { *out = t; found = 0; }
    }
    fclose(f);
    return found;
}

/* ================================================== addresses == */

static int parse_addr(const char *s, struct sockaddr_storage *ss, socklen_t *len,
                      char *err, size_t cap) {
    char host[96], port[16];
    const char *colon;
    if (s[0] == '[') {
        const char *close = strchr(s, ']');
        if (!close || close[1] != ':') { snprintf(err, cap, "address '%s': want [v6]:port", s); return -1; }
        snprintf(host, sizeof host, "%.*s", (int)(close - s - 1), s + 1);
        colon = close + 1;
    } else {
        colon = strrchr(s, ':');
        if (!colon) { snprintf(err, cap, "address '%s': want host:port", s); return -1; }
        snprintf(host, sizeof host, "%.*s", (int)(colon - s), s);
    }
    snprintf(port, sizeof port, "%s", colon + 1);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(err, cap, "address '%s': not a numeric address and port", s);
        return -1;
    }
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *len = (socklen_t)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

/* Loopback, or a range no router forwards from the internet. */
static int addr_private(const struct sockaddr_storage *ss) {
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *v = (const struct sockaddr_in *)(const void *)ss;
        uint32_t a = ntohl(v->sin_addr.s_addr);
        return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == 0xAC1 ||
               (a >> 16) == 0xC0A8 || (a >> 22) == (0x6440 >> 6) || (a >> 16) == 0xA9FE;
    }
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v = (const struct sockaddr_in6 *)(const void *)ss;
        const u8 *b = v->sin6_addr.s6_addr;
        static const u8 lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        return !memcmp(b, lo, 16) || (b[0] & 0xfe) == 0xfc || (b[0] == 0xfe && (b[1] & 0xc0) == 0x80);
    }
    return 0;
}

/* ================================================ the wire, raw == */

static int write_all(int fd, const void *buf, size_t n) {
    const u8 *p = buf;
    while (n) {
        ssize_t k = send(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int fd_write_all(int fd, const void *buf, size_t n) {
    const u8 *p = buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int read_all(int fd, void *buf, size_t n) {
    u8 *p = buf;
    while (n) {
        ssize_t k = recv(fd, p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}

/* A frame on the wire: two bytes of length, big-endian, then that many bytes. */
static int raw_send(int fd, const u8 *m, size_t n) {
    u8 hdr[2] = { (u8)(n >> 8), (u8)n };
    if (n > 65535) return -1;
    return (write_all(fd, hdr, 2) || write_all(fd, m, n)) ? -1 : 0;
}
static long raw_recv(int fd, u8 *buf, size_t cap) {
    u8 hdr[2];
    if (read_all(fd, hdr, 2)) return -1;
    size_t n = (size_t)hdr[0] << 8 | hdr[1];
    if (n > cap || read_all(fd, buf, n)) return -1;
    return (long)n;
}

static void sock_timeouts(int fd, int secs) {
    struct timeval tv = { secs, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* ================================== Noise IK, the one transport here == */
/*
 *   <- s                          the caller already knows the server's key
 *   -> e, es, s, ss               the caller's key, sealed under it
 *   <- e, ee, se                  the server's answer, which only its key opens
 *
 * After the first message the server knows who is calling and can check its
 * trust list before saying anything; after the second the caller knows it
 * reached the machine it meant to. Both ephemeral keys die with the handshake,
 * so a key stolen later does not open traffic recorded earlier.
 */

#define NZ_NAME     "Noise_IK_25519_ChaChaPoly_SHA256"     /* exactly 32 bytes */
#define NZ_PROLOGUE "substrate-net/1"

typedef struct { u8 ck[32], h[32], k[32]; uint64_t n; int has_k; } nz_sym;

static void nz_mixhash(nz_sym *s, const u8 *d, size_t n) {
    sha256_ctx x; sha256_init(&x);
    sha256_update(&x, s->h, 32); sha256_update(&x, d, n);
    sha256_final(&x, s->h);
}
static void nz_init(nz_sym *s) {
    memset(s, 0, sizeof *s);
    memcpy(s->h, NZ_NAME, 32);
    memcpy(s->ck, s->h, 32);
    nz_mixhash(s, (const u8 *)NZ_PROLOGUE, strlen(NZ_PROLOGUE));
}
static void nz_mixkey(nz_sym *s, const u8 ikm[32]) {
    hkdf2(s->ck, ikm, 32, s->ck, s->k);
    s->n = 0; s->has_k = 1;
}
static size_t nz_seal(nz_sym *s, const u8 *pt, size_t n, u8 *out) {
    aead_seal(s->k, s->n++, s->h, 32, pt, n, out);
    nz_mixhash(s, out, n + 16);
    return n + 16;
}
static int nz_open(nz_sym *s, const u8 *in, size_t n, u8 *out) {
    if (aead_open(s->k, s->n++, s->h, 32, in, n, out)) return -1;
    nz_mixhash(s, in, n);
    return 0;
}
static void nz_split(nz_sym *s, u8 k1[32], u8 k2[32]) { hkdf2(s->ck, NULL, 0, k1, k2); }

struct sub_net_conn {
    int      fd;
    u8       tx[32], rx[32];
    uint64_t txn, rxn;
    u8      *wire;                       /* one ciphertext frame, reused */
    char     peer[SUB_NET_NAMELEN];
};
struct sub_net_listener { int fd; };

static sub_net_conn *conn_new(int fd) {
    sub_net_conn *c = calloc(1, sizeof *c);
    if (c) c->wire = malloc(65535);
    if (!c || !c->wire) { free(c); return NULL; }
    c->fd = fd;
    return c;
}

static sub_net_listener *nt_listen(const char *addr, char *err, size_t cap) {
    struct sockaddr_storage ss; socklen_t len = 0;
    if (parse_addr(addr, &ss, &len, err, cap) < 0) return NULL;
    if (!addr_private(&ss)) {
        snprintf(err, cap, "refusing to listen on %s: not loopback or a private address "
                           "(bind the private link's own address)", addr);
        return NULL;
    }
    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(err, cap, "socket: %s", strerror(errno)); return NULL; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&ss, len) < 0 || listen(fd, 16) < 0) {
        snprintf(err, cap, "bind %s: %s", addr, strerror(errno));
        close(fd); return NULL;
    }
    sub_net_listener *l = calloc(1, sizeof *l);
    if (!l) { close(fd); return NULL; }
    l->fd = fd;
    return l;
}

static int nt_listen_fd(sub_net_listener *l) { return l->fd; }

static sub_net_conn *nt_accept(sub_net_listener *l, char *peer, size_t pcap, char *err, size_t cap) {
    u8 sk[32], pk[32], e_sk[32], e_pk[32], re[32], rs[32], shared[32], msg[128], tmp[64];
    int fd = accept(l->fd, NULL, NULL);
    if (fd < 0) { snprintf(err, cap, "accept: %s", strerror(errno)); return NULL; }
    sub_net_conn *c = NULL;
    trusted who;
    nz_sym s;
    long n;
    sock_timeouts(fd, 5);                 /* a handshake is two messages, not a session */
    if (load_key(sk, pk, err, cap) < 0) goto fail;

    nz_init(&s);
    nz_mixhash(&s, pk, 32);
    n = raw_recv(fd, msg, sizeof msg);
    if (n != 32 + 48 + 16) { snprintf(err, cap, "handshake: malformed first message"); goto fail; }
    memcpy(re, msg, 32);                       nz_mixhash(&s, re, 32);
    if (dh(shared, sk, re)) { snprintf(err, cap, "handshake: bad ephemeral key"); goto fail; }
    nz_mixkey(&s, shared);                                                  /* es */
    if (nz_open(&s, msg + 32, 48, rs)) { snprintf(err, cap, "handshake: caller not speaking to this key"); goto fail; }
    if (dh(shared, sk, rs)) { snprintf(err, cap, "handshake: bad static key"); goto fail; }
    nz_mixkey(&s, shared);                                                  /* ss */
    if (nz_open(&s, msg + 80, 16, tmp)) { snprintf(err, cap, "handshake: first message failed to open"); goto fail; }
    if (trust_scan(NULL, rs, &who) < 0) { snprintf(err, cap, "handshake: caller's key is not on the trust list"); goto fail; }

    random_bytes(e_sk, 32); x25519_base(e_pk, e_sk);
    memcpy(msg, e_pk, 32);                     nz_mixhash(&s, e_pk, 32);
    if (dh(shared, e_sk, re)) goto fail;       nz_mixkey(&s, shared);       /* ee */
    if (dh(shared, e_sk, rs)) goto fail;       nz_mixkey(&s, shared);       /* se */
    nz_seal(&s, NULL, 0, msg + 32);
    if (raw_send(fd, msg, 48)) { snprintf(err, cap, "handshake: caller went away"); goto fail; }

    if (!(c = conn_new(fd))) goto fail;
    nz_split(&s, c->rx, c->tx);
    snprintf(c->peer, sizeof c->peer, "%s", who.name);
    snprintf(peer, pcap, "%s", who.name);
    sock_timeouts(fd, 0);
    { struct timeval tv = { 10, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
    wipe(&s, sizeof s); wipe(sk, 32); wipe(e_sk, 32); wipe(shared, 32);
    return c;
fail:
    wipe(&s, sizeof s); wipe(sk, 32); wipe(e_sk, 32); wipe(shared, 32);
    close(fd);
    return NULL;
}

static sub_net_conn *nt_dial(const char *peer, char *err, size_t cap) {
    trusted t;
    if (trust_scan(peer, NULL, &t) < 0) { snprintf(err, cap, "no machine named '%s' on the trust list (sub-net trust)", peer); return NULL; }
    if (!t.addr[0]) { snprintf(err, cap, "'%s' is trusted but has no address (sub-net trust %s FILE --at host:port)", peer, peer); return NULL; }
    struct sockaddr_storage ss; socklen_t len = 0;
    if (parse_addr(t.addr, &ss, &len, err, cap) < 0) return NULL;

    u8 sk[32], pk[32], e_sk[32], e_pk[32], re[32], shared[32], msg[128], tmp[16];
    if (load_key(sk, pk, err, cap) < 0) return NULL;
    int fd = socket(ss.ss_family, SOCK_STREAM, 0);
    sub_net_conn *c = NULL;
    nz_sym s;
    memset(&s, 0, sizeof s);
    if (fd < 0) { snprintf(err, cap, "socket: %s", strerror(errno)); goto fail; }
    sock_timeouts(fd, 5);
    if (connect(fd, (struct sockaddr *)&ss, len) < 0) {
        snprintf(err, cap, "connect %s (%s): %s", peer, t.addr, strerror(errno)); goto fail;
    }

    nz_init(&s);
    nz_mixhash(&s, t.pk, 32);
    random_bytes(e_sk, 32); x25519_base(e_pk, e_sk);
    memcpy(msg, e_pk, 32);                     nz_mixhash(&s, e_pk, 32);
    if (dh(shared, e_sk, t.pk)) goto fail;     nz_mixkey(&s, shared);       /* es */
    nz_seal(&s, pk, 32, msg + 32);                                          /* s  */
    if (dh(shared, sk, t.pk)) goto fail;       nz_mixkey(&s, shared);       /* ss */
    nz_seal(&s, NULL, 0, msg + 80);
    if (raw_send(fd, msg, 96)) { snprintf(err, cap, "handshake: %s went away", peer); goto fail; }

    long n = raw_recv(fd, msg, sizeof msg);
    if (n != 48) {
        snprintf(err, cap, "handshake: %s (%s) hung up: it does not trust this machine's key, "
                           "or it is not the machine whose key is trusted under that name", peer, t.addr);
        goto fail;
    }
    memcpy(re, msg, 32);                       nz_mixhash(&s, re, 32);
    if (dh(shared, e_sk, re)) goto fail;       nz_mixkey(&s, shared);       /* ee */
    if (dh(shared, sk, re)) goto fail;         nz_mixkey(&s, shared);       /* se */
    if (nz_open(&s, msg + 32, 16, tmp)) {
        snprintf(err, cap, "handshake: %s did not prove its key -- not the machine trusted under that name", peer);
        goto fail;
    }
    if (!(c = conn_new(fd))) goto fail;
    nz_split(&s, c->tx, c->rx);
    snprintf(c->peer, sizeof c->peer, "%s", peer);
    sock_timeouts(fd, 0);
    { struct timeval tv = { 10, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
    wipe(&s, sizeof s); wipe(sk, 32); wipe(e_sk, 32); wipe(shared, 32);
    return c;
fail:
    if (!err[0]) snprintf(err, cap, "handshake with %s failed", peer);
    wipe(&s, sizeof s); wipe(sk, 32); wipe(e_sk, 32); wipe(shared, 32);
    if (fd >= 0) close(fd);
    return NULL;
}

static int nt_send(sub_net_conn *c, const void *m, size_t n) {
    if (n > SUB_NET_FRAME_MAX || c->txn == UINT64_MAX) return -1;
    aead_seal(c->tx, c->txn++, NULL, 0, m, n, c->wire);
    return raw_send(c->fd, c->wire, n + 16);
}
static long nt_recv(sub_net_conn *c, void *buf, size_t cap) {
    long n = raw_recv(c->fd, c->wire, 65535);
    if (n < 0) return 0;                              /* closed, or torn: either way, gone */
    if (n < 16 || (size_t)n - 16 > cap) return -1;
    if (aead_open(c->rx, c->rxn++, NULL, 0, c->wire, (size_t)n, buf)) return -1;
    return n - 16;
}
static int  nt_fd(sub_net_conn *c)       { return c->fd; }
static void nt_shutdown(sub_net_conn *c) { shutdown(c->fd, SHUT_RDWR); }
static void nt_close(sub_net_conn *c) {
    if (!c) return;
    close(c->fd);
    wipe(c->tx, 32); wipe(c->rx, 32);
    free(c->wire); free(c);
}
static void nt_close_listener(sub_net_listener *l) { if (l) { close(l->fd); free(l); } }

static const sub_net_transport NOISE_TCP = {
    "noise-ik-tcp", nt_listen, nt_listen_fd, nt_accept, nt_dial,
    nt_send, nt_recv, nt_fd, nt_shutdown, nt_close, nt_close_listener,
};
static const sub_net_transport *T = &NOISE_TCP;

/* ============================================================== JSON == */
/* Just enough to read the protocol's own objects: find a top-level key, take
 * a string, a number, a bool, walk an array. Values it does not need it skips
 * whole, so a newer peer's extra fields cost nothing. */

static const char *j_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }

static const char *j_skip(const char *p) {
    p = j_ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p == '"' ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = j_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (obj) {
                if (*p != '"' || !(p = j_skip(p))) return NULL;
                p = j_ws(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = j_skip(p))) return NULL;
            p = j_ws(p);
            if (*p == ',') { p = j_ws(p + 1); continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    const char *s = p;
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p > s ? p : NULL;
}

static const char *j_find(const char *obj, const char *key) {
    const char *p = j_ws(obj);
    if (*p++ != '{') return NULL;
    size_t kl = strlen(key);
    for (;;) {
        p = j_ws(p);
        if (*p != '"') return NULL;
        const char *k = p + 1, *ke = j_skip(p);
        if (!ke) return NULL;
        p = j_ws(ke);
        if (*p++ != ':') return NULL;
        p = j_ws(p);
        if ((size_t)(ke - 1 - k) == kl && !memcmp(k, key, kl)) return p;
        if (!(p = j_skip(p))) return NULL;
        p = j_ws(p);
        if (*p++ != ',') return NULL;
    }
}

/* Decode a JSON string value into out. \u escapes become UTF-8. */
static int j_strval(const char *v, char *out, size_t cap) {
    if (!v || *v != '"' || !cap) return -1;
    size_t o = 0;
    for (v++; *v && *v != '"'; v++) {
        unsigned c = (u8)*v;
        if (c == '\\') {
            v++;
            switch (*v) {
                case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
                case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                case 'u': {
                    unsigned u = 0;
                    for (int i = 1; i <= 4; i++) { int h = hexval(v[i]); if (h < 0) return -1; u = u << 4 | (unsigned)h; }
                    v += 4;
                    if (u < 0x80) c = u;
                    else {
                        char enc[3]; size_t k;
                        if (u < 0x800) { enc[0] = (char)(0xC0 | u >> 6); enc[1] = (char)(0x80 | (u & 63)); k = 2; }
                        else { enc[0] = (char)(0xE0 | u >> 12); enc[1] = (char)(0x80 | ((u >> 6) & 63)); enc[2] = (char)(0x80 | (u & 63)); k = 3; }
                        for (size_t i = 0; i < k; i++) { if (o + 1 >= cap) return -1; out[o++] = enc[i]; }
                        continue;
                    }
                    break;
                }
                case 0: return -1;
                default: c = (u8)*v;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    out[o] = 0;
    return *v == '"' ? 0 : -1;
}

static int j_str(const char *obj, const char *key, char *out, size_t cap) {
    return j_strval(j_find(obj, key), out, cap);
}
static long long j_num(const char *obj, const char *key, long long dflt) {
    const char *v = j_find(obj, key);
    return (v && (*v == '-' || (*v >= '0' && *v <= '9'))) ? strtoll(v, NULL, 10) : dflt;
}
static int j_bool(const char *obj, const char *key) {
    const char *v = j_find(obj, key);
    return v && !strncmp(v, "true", 4);
}

/* Walk an array: *it starts at the '[', each call yields the next element. */
static const char *j_next(const char **it) {
    const char *p = j_ws(*it);
    if (*p == '[' || *p == ',') p = j_ws(p + 1);
    if (*p == ']' || !*p) return NULL;
    const char *end = j_skip(p);
    if (!end) return NULL;
    *it = j_ws(end);
    return p;
}

/* Is the whole line exactly one JSON value? */
static int j_is_value(const char *s) {
    const char *e = j_skip(s);
    return e && !*j_ws(e);
}

typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t k) {
    if (b->n + k + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + k + 1) nc *= 2;
        char *q = realloc(b->p, nc);
        if (!q) abort();
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, s, k);
    b->n += k; b->p[b->n] = 0;
}
static void sb_s(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_f(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_f(sbuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0) sb_put(b, tmp, (size_t)k < sizeof tmp ? (size_t)k : sizeof tmp - 1);
}
static void sb_json(sbuf *b, const char *s, size_t max) {
    sb_s(b, "\"");
    for (size_t i = 0; i < max && s[i]; i++) {
        u8 c = (u8)s[i];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sb_put(b, e, 2); }
        else if (c == '\n') sb_s(b, "\\n");
        else if (c == '\t') sb_s(b, "\\t");
        else if (c == '\r') sb_s(b, "\\r");
        else if (c < 0x20) sb_f(b, "\\u%04x", c);
        else sb_put(b, (const char *)&c, 1);
    }
    sb_s(b, "\"");
}

static int chan_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= SUB_NET_NAMELEN) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.')) return 0;
    return 1;
}

/* ============================================================ the server == */

typedef struct rconn {
    sub_net_conn   *c;
    pthread_mutex_t mu;              /* one sender at a time: the nonce is a counter */
    char            peer[SUB_NET_NAMELEN];
    char            chan[SUB_NET_NAMELEN];
    int             events, dead;
    struct rconn   *next;
} rconn;

typedef struct lsn {
    char            chan[SUB_NET_NAMELEN];
    int             fd;
    pthread_mutex_t mu;
    struct lsn     *next;
} lsn;

/* g_mu guards both lists, and every broadcast holds it, so a connection is
 * only freed once nothing can be mid-send on it. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static rconn   *g_conns = NULL;
static lsn     *g_lsns  = NULL;
static uint64_t g_seq   = 0;
static char     g_sub[PATH_MAX];     /* the `sub` beside this binary */
static volatile sig_atomic_t g_quit = 0;

static void on_quit(int s) { (void)s; g_quit = 1; }

static int rc_send(rconn *r, const char *s, size_t n) {
    pthread_mutex_lock(&r->mu);
    int rc = r->dead ? -1 : T->send(r->c, s, n);
    if (rc < 0 && !r->dead) { r->dead = 1; T->shutdown(r->c); }
    pthread_mutex_unlock(&r->mu);
    return rc;
}

static void reply(rconn *r, long long id, sbuf *b) {
    sbuf o = {0};
    sb_f(&o, "{\"id\":%lld,", id);
    sb_put(&o, b->p ? b->p : "", b->n);
    sb_s(&o, "}");
    rc_send(r, o.p, o.n);
    free(o.p);
}
static void reply_err(rconn *r, long long id, const char *msg) {
    sbuf b = {0};
    sb_s(&b, "\"ok\":false,\"error\":"); sb_json(&b, msg, 4096);
    reply(r, id, &b); free(b.p);
}

/* Push one event to every subscriber, numbered. The caller holds g_mu. */
static void emit_locked(sub_seg *seg, const char *ev, const char *body) {
    uint64_t seq = ++g_seq;
    int any = 0;
    for (rconn *r = g_conns; r; r = r->next) if (r->events) { any = 1; break; }
    if (!any) return;
    struct timeval tv; gettimeofday(&tv, NULL);
    sbuf b = {0};
    sb_f(&b, "{\"seq\":%llu,\"ev\":\"%s\",\"at\":%lld", (unsigned long long)seq, ev,
         (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
    if (seg) sb_f(&b, ",\"count\":%llu,\"next_id\":%llu",
                  (unsigned long long)atomic_load(&seg->count),
                  (unsigned long long)atomic_load(&seg->next_id));
    if (body && *body) { sb_s(&b, ","); sb_s(&b, body); }
    sb_s(&b, "}");
    for (rconn *r = g_conns; r; r = r->next) if (r->events) rc_send(r, b.p, b.n);
    free(b.p);
}

static void emit(sub_seg *seg, const char *ev, const char *body) {
    pthread_mutex_lock(&g_mu);
    emit_locked(seg, ev, body);
    pthread_mutex_unlock(&g_mu);
}

/* ---- events: the segment, watched from inside the machine ----
 *
 * The network side is pushed: a subscriber holds one connection open and is
 * told. Finding out is a read of shared memory every 10 ms -- 256 rows of a
 * page this process already has mapped, no system call but kill(pid, 0) for a
 * live row. A row is reported once it has held still for one scan, because a
 * claim writes `state` before `name` and a report in between would name the
 * row's previous tenant. */

typedef struct { int live; uint64_t pid, ops; char name[SUB_NAMELEN], role[SUB_NAMELEN]; } rowsnap;

static void row_take(sub_peer *p, rowsnap *o) {
    memset(o, 0, sizeof *o);
    o->live = sub_peer_alive(p);
    if (!o->live) return;
    o->pid = atomic_load(&p->pid);
    o->ops = atomic_load(&p->ops);
    memcpy(o->name, p->name, SUB_NAMELEN); o->name[SUB_NAMELEN - 1] = 0;
    memcpy(o->role, p->role, SUB_NAMELEN); o->role[SUB_NAMELEN - 1] = 0;
}
static int row_same(const rowsnap *a, const rowsnap *b) {
    return a->live == b->live && a->pid == b->pid &&
           !strcmp(a->name, b->name) && !strcmp(a->role, b->role);
}
static void row_body(sbuf *b, int slot, const rowsnap *r) {
    sb_f(b, "\"slot\":%d,\"pid\":%llu,\"name\":", slot, (unsigned long long)r->pid);
    sb_json(b, r->name, SUB_NAMELEN);
    sb_s(b, ",\"role\":"); sb_json(b, r->role, SUB_NAMELEN);
    sb_f(b, ",\"ops\":%llu", (unsigned long long)r->ops);
}

/* argv words for the `sub` this daemon runs. Arrays, not literals: execv
 * takes char *const[], and a literal is const under -Wwrite-strings. */
static char A_RUN[] = "run", A_STOP[] = "stop", A_ENABLE[] = "enable", A_DISABLE[] = "disable",
            A_NAME[] = "--name", A_ROLE[] = "--role", A_FORCE[] = "--force", A_DASH[] = "--";

static void *events_main(void *arg) {
    (void)arg;
    static rowsnap seen[SUB_MAX_PEERS], told[SUB_MAX_PEERS];
    sub_seg *seg = NULL;
    int up = -1;
    uint64_t told_count = 0;
    uint64_t told_ops[SUB_MAX_PEERS];
    int64_t next_slow = 0;
    memset(told_ops, 0, sizeof told_ops);
    while (!g_quit) {
        if (!seg || !sub_owner_alive(seg)) {
            if (seg) { munmap(seg, sizeof *seg); seg = NULL; }
            int err = 0;
            seg = sub_attach(&err);
            if (seg && !sub_owner_alive(seg)) { munmap(seg, sizeof *seg); seg = NULL; }
            int now_up = seg != NULL;
            if (now_up != up) {
                pthread_mutex_lock(&g_mu);
                for (int i = 0; i < SUB_MAX_PEERS; i++) if (told[i].live) {
                    sbuf b = {0}; row_body(&b, i, &told[i]);
                    emit_locked(NULL, "leave", b.p); free(b.p);
                }
                memset(told, 0, sizeof told); memset(seen, 0, sizeof seen);
                sbuf b = {0};
                sb_f(&b, "\"up\":%s,\"segment\":", now_up ? "true" : "false");
                sb_json(&b, sub_shm_name(), 64);
                emit_locked(seg, "registry", b.p); free(b.p);
                pthread_mutex_unlock(&g_mu);
                up = now_up;
                if (seg) told_count = atomic_load(&seg->count);
            }
            if (!seg) { usleep(100000); continue; }
        }

        for (int i = 0; i < SUB_MAX_PEERS; i++) {
            rowsnap cur; row_take(&seg->peers[i], &cur);
            int stable = row_same(&cur, &seen[i]);
            seen[i] = cur;
            if (!stable || row_same(&cur, &told[i])) continue;
            const char *ev = !cur.live ? "leave" : !told[i].live ? "join" : "update";
            sbuf b = {0}; row_body(&b, i, cur.live ? &cur : &told[i]);
            emit(seg, ev, b.p); free(b.p);
            told[i] = cur;
            told_ops[i] = cur.ops;
        }

        /* Counters move fast; they are coalesced to ten reports a second. */
        int64_t now = sub_now_ns();
        if (now >= next_slow) {
            next_slow = now + 100000000;
            for (int i = 0; i < SUB_MAX_PEERS; i++) {
                if (!told[i].live) continue;
                uint64_t ops = atomic_load(&seg->peers[i].ops);
                if (ops == told_ops[i]) continue;
                told_ops[i] = ops; told[i].ops = ops;
                sbuf b = {0}; row_body(&b, i, &told[i]);
                emit(seg, "ops", b.p); free(b.p);
            }
            uint64_t c = atomic_load(&seg->count);
            if (c != told_count) { told_count = c; emit(seg, "count", NULL); }
        }
        usleep(10000);
    }
    return NULL;
}

/* ---- process control: done by this machine's own `sub` ---- */

static void ps_body(sbuf *b, sub_seg *s) {
    sb_f(b, "\"ok\":true,\"segment\":");
    sb_json(b, sub_shm_name(), 64);
    sb_f(b, ",\"count\":%llu,\"next_id\":%llu,\"owner_pid\":%llu,\"now\":%lld,\"peers\":[",
         (unsigned long long)atomic_load(&s->count), (unsigned long long)atomic_load(&s->next_id),
         (unsigned long long)atomic_load(&s->owner_pid), (long long)time(NULL));
    int first = 1;
    for (int i = 0; i < SUB_MAX_PEERS; i++) {
        sub_peer *p = &s->peers[i];
        if (atomic_load(&p->state) != 1) continue;
        rowsnap r; row_take(p, &r);
        if (!r.live) {               /* registered, process gone, not yet reaped */
            r.pid = atomic_load(&p->pid); r.ops = atomic_load(&p->ops);
            memcpy(r.name, p->name, SUB_NAMELEN); r.name[SUB_NAMELEN - 1] = 0;
            memcpy(r.role, p->role, SUB_NAMELEN); r.role[SUB_NAMELEN - 1] = 0;
        }
        uint64_t since = sub_proc_start((pid_t)r.pid);
        if (!since) since = atomic_load(&p->since);
        sb_s(b, first ? "{" : ",{"); first = 0;
        row_body(b, i, &r);
        sb_f(b, ",\"since\":%llu,\"alive\":%s}", (unsigned long long)since, r.live ? "true" : "false");
    }
    sb_s(b, "]");
}

/* Run a command to completion, its stdout and stderr captured together. */
static int run_capture(char *const av[], sbuf *out) {
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t k = fork();
    if (k < 0) { close(p[0]); close(p[1]); return -1; }
    if (k == 0) {
        dup2(p[1], 1); dup2(p[1], 2); close(p[0]); close(p[1]);
        int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); close(nul); }
        execv(av[0], av);
        _exit(127);
    }
    close(p[1]);
    char buf[1024]; ssize_t n;
    while ((n = read(p[0], buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (out->n < 16384) sb_put(out, buf, (size_t)n);
    }
    close(p[0]);
    int st = 0;
    while (waitpid(k, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* Start `sub run` detached from this daemon: its own session, so stopping the
 * daemon does not stop what it started. Returns the wrapper's pid. */
static pid_t spawn_detached(char *const av[], const char *log) {
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t k = fork();
    if (k < 0) { close(p[0]); close(p[1]); return -1; }
    if (k == 0) {
        close(p[0]);
        pid_t g = fork();
        if (g == 0) {
            close(p[1]);
            setsid();
            int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); close(nul); }
            int lf = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); close(lf); }
            execv(av[0], av);
            _exit(127);
        }
        ssize_t w = write(p[1], &g, sizeof g); (void)w;
        _exit(0);
    }
    close(p[1]);
    pid_t g = -1;
    if (read(p[0], &g, sizeof g) != (ssize_t)sizeof g) g = -1;
    close(p[0]);
    while (waitpid(k, NULL, 0) < 0 && errno == EINTR) {}
    return g;
}

/* The pid on the live row named `name`, or 0. */
static pid_t row_named(const char *name) {
    int err = 0;
    sub_seg *s = sub_attach(&err);
    if (!s) return 0;
    pid_t hit = 0;
    for (int i = 0; i < SUB_MAX_PEERS && !hit; i++)
        if (sub_peer_alive(&s->peers[i]) && !strncmp(s->peers[i].name, name, SUB_NAMELEN - 1))
            hit = (pid_t)atomic_load(&s->peers[i].pid);
    munmap(s, sizeof *s);
    return hit;
}

static void op_run(rconn *r, long long id, const char *req) {
    char name[SUB_NAMELEN], role[SUB_NAMELEN] = "service";
    if (j_str(req, "name", name, sizeof name) < 0 || !chan_ok(name)) { reply_err(r, id, "run: want a name of letters, digits, - _ . (15 at most)"); return; }
    j_str(req, "role", role, sizeof role);
    const char *it = j_find(req, "argv");
    if (!it || *it != '[') { reply_err(r, id, "run: want argv, a list of strings"); return; }
    char *av[72]; int n = 0;
    av[n++] = g_sub; av[n++] = A_RUN;
    av[n++] = A_NAME; av[n++] = name;
    av[n++] = A_ROLE; av[n++] = role;
    av[n++] = A_DASH;
    int base = n;
    for (const char *e; (e = j_next(&it)) && n < 70; ) {
        char tmp[4096];
        if (j_strval(e, tmp, sizeof tmp) < 0) break;
        av[n++] = strdup(tmp);
    }
    av[n] = NULL;
    if (n == base) { reply_err(r, id, "run: argv is empty"); return; }
    char logd[PATH_MAX], log[PATH_MAX + 64];
    net_file("logs", logd, sizeof logd); mkdir_p(logd, 0700);
    snprintf(log, sizeof log, "%s/%s.log", logd, name);
    pid_t g = spawn_detached(av, log);
    for (int i = base; i < n; i++) free(av[i]);
    if (g < 0) { reply_err(r, id, "run: could not start it"); return; }
    /* Answer once the row is there, so "started" means "registered". */
    /* The row's pid is the service's own; the wrapper's is only how it got there. */
    pid_t reg = 0;
    for (int i = 0; i < 100 && !(reg = row_named(name)); i++) {
        if (kill(g, 0) != 0) break;                    /* it exited: sub run refused */
        usleep(20000);
    }
    sbuf b = {0};
    sb_f(&b, "\"ok\":%s,\"pid\":%d,\"wrapper\":%d,\"registered\":%s", reg ? "true" : "false",
         (int)reg, (int)g, reg ? "true" : "false");
    if (!reg) { sb_s(&b, ",\"error\":\"not registered -- see the log\",\"log\":"); sb_json(&b, log, sizeof log); }
    reply(r, id, &b); free(b.p);
}

static void op_capture(rconn *r, long long id, char *const av[]) {
    sbuf out = {0};
    int rc = run_capture(av, &out);
    sbuf b = {0};
    sb_f(&b, "\"ok\":%s,\"exit\":%d,\"out\":", rc == 0 ? "true" : "false", rc);
    sb_json(&b, out.p ? out.p : "", 16384);
    reply(r, id, &b); free(b.p); free(out.p);
}

static void handle(rconn *r, const char *req) {
    /* channel traffic: forward the value, as it came, to this machine's listener */
    if (!j_find(req, "op") && j_find(req, "chan")) {
        char ch[SUB_NET_NAMELEN];
        const char *m = j_find(req, "msg"), *me = m ? j_skip(m) : NULL;
        if (j_str(req, "chan", ch, sizeof ch) < 0 || strcmp(ch, r->chan) || !me) return;
        pthread_mutex_lock(&g_mu);
        for (lsn *l = g_lsns; l; l = l->next) if (!strcmp(l->chan, ch)) {
            pthread_mutex_lock(&l->mu);
            fd_write_all(l->fd, m, (size_t)(me - m));
            fd_write_all(l->fd, "\n", 1);
            pthread_mutex_unlock(&l->mu);
        }
        pthread_mutex_unlock(&g_mu);
        return;
    }
    long long id = j_num(req, "id", 0);
    char op[24];
    if (j_str(req, "op", op, sizeof op) < 0) { reply_err(r, id, "want an op"); return; }

    if (!strcmp(op, "ps")) {
        int err = 0;
        sub_seg *s = sub_attach(&err);
        if (!s || !sub_owner_alive(s)) { if (s) munmap(s, sizeof *s); reply_err(r, id, "no registry running on this machine"); return; }
        sbuf b = {0}; ps_body(&b, s); munmap(s, sizeof *s);
        reply(r, id, &b); free(b.p);
    } else if (!strcmp(op, "run")) {
        op_run(r, id, req);
    } else if (!strcmp(op, "stop")) {
        char name[64];
        if (j_str(req, "name", name, sizeof name) < 0 || !name[0]) { reply_err(r, id, "stop: want a name"); return; }
        char *av[] = { g_sub, A_STOP, A_FORCE, name, NULL };
        if (!j_bool(req, "force")) { av[2] = name; av[3] = NULL; }
        op_capture(r, id, av);
    } else if (!strcmp(op, "disable")) {
        char name[64];
        if (j_str(req, "name", name, sizeof name) < 0 || !name[0]) { reply_err(r, id, "disable: want a name"); return; }
        char *av[] = { g_sub, A_DISABLE, name, NULL };
        op_capture(r, id, av);
    } else if (!strcmp(op, "enable")) {
        const char *it = j_find(req, "args");
        if (!it || *it != '[') { reply_err(r, id, "enable: want args, a list of strings"); return; }
        char *av[72]; int n = 0;
        av[n++] = g_sub; av[n++] = A_ENABLE;
        for (const char *e; (e = j_next(&it)) && n < 70; ) {
            char tmp[4096];
            if (j_strval(e, tmp, sizeof tmp) < 0) break;
            av[n++] = strdup(tmp);
        }
        av[n] = NULL;
        op_capture(r, id, av);
        for (int i = 2; i < n; i++) free(av[i]);
    } else if (!strcmp(op, "events")) {
        pthread_mutex_lock(&g_mu);           /* the answer goes out before any event */
        r->events = 1;
        sbuf b = {0}; sb_f(&b, "\"ok\":true,\"seq\":%llu,\"segment\":", (unsigned long long)g_seq);
        sb_json(&b, sub_shm_name(), 64);
        reply(r, id, &b); free(b.p);
        pthread_mutex_unlock(&g_mu);
    } else if (!strcmp(op, "connect")) {
        char ch[SUB_NET_NAMELEN];
        if (j_str(req, "chan", ch, sizeof ch) < 0 || !chan_ok(ch)) { reply_err(r, id, "connect: want a channel name"); return; }
        pthread_mutex_lock(&g_mu);
        int have = 0;
        for (lsn *l = g_lsns; l; l = l->next) if (!strcmp(l->chan, ch)) have = 1;
        if (have) snprintf(r->chan, sizeof r->chan, "%s", ch);
        pthread_mutex_unlock(&g_mu);
        if (!have) { reply_err(r, id, "nothing is listening on that channel here"); return; }
        sbuf b = {0}; sb_s(&b, "\"ok\":true,\"chan\":"); sb_json(&b, ch, sizeof ch);
        reply(r, id, &b); free(b.p);
    } else {
        reply_err(r, id, "unknown op");
    }
}

static void *conn_main(void *arg) {
    rconn *r = arg;
    char *buf = malloc(SUB_NET_FRAME_MAX + 1);
    for (long n; buf && (n = T->recv(r->c, buf, SUB_NET_FRAME_MAX)) > 0; ) {
        buf[n] = 0;
        handle(r, buf);
    }
    free(buf);
    pthread_mutex_lock(&g_mu);
    for (rconn **pp = &g_conns; *pp; pp = &(*pp)->next) if (*pp == r) { *pp = r->next; break; }
    pthread_mutex_unlock(&g_mu);
    printf("net: %s disconnected\n", r->peer); fflush(stdout);
    T->close(r->c);
    pthread_mutex_destroy(&r->mu);
    free(r);
    return NULL;
}

/* A local listener: a process on THIS machine serving a named channel. It
 * speaks plain lines over a unix socket in this machine's net folder -- the
 * same trust boundary as the segment, which is the file mode. */
static void *lsn_main(void *arg) {
    int fd = (int)(intptr_t)arg;
    char hello[80]; size_t h = 0;
    while (h + 1 < sizeof hello) {
        ssize_t k = read(fd, hello + h, 1);
        if (k <= 0) { close(fd); return NULL; }
        if (hello[h] == '\n') break;
        h++;
    }
    hello[h] = 0;
    const char *ch = !strncmp(hello, "listen ", 7) ? hello + 7 : "";
    if (!chan_ok(ch)) { fd_write_all(fd, "error bad channel\n", 18); close(fd); return NULL; }

    lsn *me = calloc(1, sizeof *me);
    if (!me) { close(fd); return NULL; }
    snprintf(me->chan, sizeof me->chan, "%s", ch);
    me->fd = fd;
    pthread_mutex_init(&me->mu, NULL);
    pthread_mutex_lock(&g_mu);
    int busy = 0;
    for (lsn *l = g_lsns; l; l = l->next) if (!strcmp(l->chan, ch)) busy = 1;
    if (!busy) { me->next = g_lsns; g_lsns = me; }
    pthread_mutex_unlock(&g_mu);
    if (busy) { fd_write_all(fd, "error busy\n", 11); close(fd); free(me); return NULL; }
    fd_write_all(fd, "ok\n", 3);
    printf("net: listening on channel %s\n", me->chan); fflush(stdout);

    char *line = malloc(SUB_NET_FRAME_MAX);
    size_t have = 0;
    int toolong = 0;
    for (;;) {
        char buf[4096];
        ssize_t k = read(fd, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0 || !line) break;
        for (ssize_t i = 0; i < k; i++) {
            if (buf[i] != '\n') {
                if (have + 1 < SUB_NET_FRAME_MAX - 64) line[have++] = buf[i]; else toolong = 1;
                continue;
            }
            line[have] = 0;
            if (toolong) fprintf(stderr, "net: channel %s: a line over %d bytes was dropped\n", me->chan, SUB_NET_FRAME_MAX - 64);
            else if (have) {
                sbuf b = {0};
                sb_s(&b, "{\"chan\":"); sb_json(&b, me->chan, sizeof me->chan); sb_s(&b, ",\"msg\":");
                if (j_is_value(line)) sb_put(&b, line, have); else sb_json(&b, line, have);
                sb_s(&b, "}");
                pthread_mutex_lock(&g_mu);
                for (rconn *r = g_conns; r; r = r->next) if (!strcmp(r->chan, me->chan)) rc_send(r, b.p, b.n);
                pthread_mutex_unlock(&g_mu);
                free(b.p);
            }
            have = 0; toolong = 0;
        }
    }
    free(line);
    pthread_mutex_lock(&g_mu);
    for (lsn **pp = &g_lsns; *pp; pp = &(*pp)->next) if (*pp == me) { *pp = me->next; break; }
    sbuf b = {0};
    sb_s(&b, "{\"chan\":"); sb_json(&b, me->chan, sizeof me->chan); sb_s(&b, ",\"closed\":true}");
    for (rconn *r = g_conns; r; r = r->next) if (!strcmp(r->chan, me->chan)) { rc_send(r, b.p, b.n); r->chan[0] = 0; }
    pthread_mutex_unlock(&g_mu);
    free(b.p);
    printf("net: channel %s closed\n", me->chan); fflush(stdout);
    close(fd);
    pthread_mutex_destroy(&me->mu);
    free(me);
    return NULL;
}

static int self_dir(char *out, size_t cap) {
    char p[PATH_MAX] = {0};
#if defined(__APPLE__)
    uint32_t n = sizeof p;
    if (_NSGetExecutablePath(p, &n) != 0) return -1;
#else
    ssize_t k = readlink("/proc/self/exe", p, sizeof p - 1);
    if (k <= 0) return -1;
    p[k] = 0;
#endif
    char real[PATH_MAX];
    if (realpath(p, real)) snprintf(p, sizeof p, "%s", real);
    char *slash = strrchr(p, '/');
    if (!slash) return -1;
    *slash = 0;
    snprintf(out, cap, "%s", p);
    return 0;
}

static void spawn_thread(void *(*fn)(void *), void *arg) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, fn, arg) != 0) perror("net: pthread_create");
    pthread_attr_destroy(&a);
}

static int unix_sock_path(char *out, size_t cap) {
    net_file("netd.sock", out, cap);
    return strlen(out) < sizeof(((struct sockaddr_un *)0)->sun_path) ? 0 : -1;
}

static int do_serve(int argc, char **argv) {
    const char *bind_to = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_to = argv[++i];
        else { fprintf(stderr, "sub-net serve: unknown option %s\n", argv[i]); return 2; }
    }
    if (!bind_to) {
        fprintf(stderr, "usage: sub-net serve --bind ADDRESS:PORT   (the private link's own address, e.g. 10.0.0.1:%d)\n", SUB_NET_PORT);
        return 2;
    }
    char err[256] = {0};
    u8 sk[32], pk[32];
    if (load_key(sk, pk, err, sizeof err) < 0) { fprintf(stderr, "sub-net: %s\n", err); return 3; }
    wipe(sk, 32);

    char dir[PATH_MAX]; self_dir(dir, sizeof dir);
    snprintf(g_sub, sizeof g_sub, "%s/sub", dir);
    if (access(g_sub, X_OK) != 0) { fprintf(stderr, "sub-net: no 'sub' beside this binary (%s)\n", g_sub); return 3; }

    sub_net_listener *L = T->listen(bind_to, err, sizeof err);
    if (!L) { fprintf(stderr, "sub-net: %s\n", err); return 3; }

    /* the local side: listeners for named channels */
    char sp[PATH_MAX];
    if (unix_sock_path(sp, sizeof sp) < 0) { fprintf(stderr, "sub-net: net folder path too long for a socket\n"); return 3; }
    struct sockaddr_un ua; memset(&ua, 0, sizeof ua);
    ua.sun_family = AF_UNIX;
    snprintf(ua.sun_path, sizeof ua.sun_path, "%s", sp);
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0 && connect(probe, (struct sockaddr *)&ua, sizeof ua) == 0) {
        fprintf(stderr, "sub-net: another sub-net serve already uses %s\n", sp);
        close(probe); return 3;
    }
    if (probe >= 0) close(probe);
    unlink(sp);
    int U = socket(AF_UNIX, SOCK_STREAM, 0);
    if (U < 0 || bind(U, (struct sockaddr *)&ua, sizeof ua) < 0 || listen(U, 16) < 0) {
        fprintf(stderr, "sub-net: local socket %s: %s\n", sp, strerror(errno)); return 3;
    }
    chmod(sp, 0600);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_quit;                       /* no SA_RESTART: poll() must wake */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    spawn_thread(events_main, NULL);

    char nd[PATH_MAX]; net_dir(nd, sizeof nd);
    printf("net: serving %s on %s (%s); trust list %s/peers\n",
           sub_shm_name(), bind_to, T->name, nd);
    fflush(stdout);
    while (!g_quit) {
        struct pollfd pf[2] = { { T->listen_fd(L), POLLIN, 0 }, { U, POLLIN, 0 } };
        int k = poll(pf, 2, 250);
        if (k <= 0) continue;
        if (pf[0].revents & POLLIN) {
            char peer[SUB_NET_NAMELEN] = {0}; err[0] = 0;
            sub_net_conn *c = T->accept(L, peer, sizeof peer, err, sizeof err);
            if (!c) { printf("net: refused a caller: %s\n", err); fflush(stdout); }
            else {
                rconn *r = calloc(1, sizeof *r);
                if (!r) { T->close(c); continue; }
                r->c = c;
                snprintf(r->peer, sizeof r->peer, "%s", peer);
                pthread_mutex_init(&r->mu, NULL);
                pthread_mutex_lock(&g_mu); r->next = g_conns; g_conns = r; pthread_mutex_unlock(&g_mu);
                printf("net: %s connected\n", peer); fflush(stdout);
                spawn_thread(conn_main, r);
            }
        }
        if (pf[1].revents & POLLIN) {
            int fd = accept(U, NULL, NULL);
            if (fd >= 0) spawn_thread(lsn_main, (void *)(intptr_t)fd);
        }
    }
    close(U); unlink(sp);
    T->close_listener(L);
    printf("net: stopped\n");
    return 0;
}

/* ============================================================ the clients == */

static sub_net_conn *must_dial(const char *peer) {
    char err[320] = {0};
    sub_net_conn *c = T->dial(peer, err, sizeof err);
    if (!c) fprintf(stderr, "sub-net: %s\n", err);
    return c;
}

/* Send one request and wait for its answer, passing over anything pushed meanwhile. */
static char *ask(sub_net_conn *c, sbuf *req) {
    if (T->send(c, req->p, req->n) < 0) { fprintf(stderr, "sub-net: connection lost\n"); return NULL; }
    long long want = j_num(req->p, "id", 0);
    char *buf = malloc(SUB_NET_FRAME_MAX + 1);
    for (long n; buf && (n = T->recv(c, buf, SUB_NET_FRAME_MAX)) > 0; ) {
        buf[n] = 0;
        if (j_find(buf, "ok") && j_num(buf, "id", -1) == want) return buf;
    }
    free(buf);
    fprintf(stderr, "sub-net: connection lost before the answer\n");
    return NULL;
}

static int answer_failed(const char *ans) {
    if (j_bool(ans, "ok")) return 0;
    char e[1024] = "refused";
    j_str(ans, "error", e, sizeof e);
    fprintf(stderr, "sub-net: %s\n", e);
    return 1;
}

static void fmt_age(long long s, char *out, size_t cap) {
    if (s < 0) s = 0;
    if      (s < 60)    snprintf(out, cap, "%llds", s);
    else if (s < 3600)  snprintf(out, cap, "%lldm%02llds", s / 60, s % 60);
    else if (s < 86400) snprintf(out, cap, "%lldh%02lldm", s / 3600, (s % 3600) / 60);
    else                snprintf(out, cap, "%lldd%02lldh", s / 86400, (s % 86400) / 3600);
}

static int do_ps(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: sub-net ps MACHINE [--json]\n"); return 2; }
    int json = argc > 1 && !strcmp(argv[1], "--json");
    sub_net_conn *c = must_dial(argv[0]);
    if (!c) return 3;
    sbuf req = {0}; sb_s(&req, "{\"id\":1,\"op\":\"ps\"}");
    char *ans = ask(c, &req);
    free(req.p); T->close(c);
    if (!ans) return 3;
    if (answer_failed(ans)) { free(ans); return 1; }
    if (json) { printf("%s\n", ans); free(ans); return 0; }
    long long now = j_num(ans, "now", (long long)time(NULL));
    printf("%-8s  %-15s  %-15s  %-9s  %s\n", "PID", "NAME", "ROLE", "UP", "OPS");
    const char *it = j_find(ans, "peers");
    int live = 0, stale = 0;
    for (const char *e; it && (e = j_next(&it)); ) {
        char nm[SUB_NAMELEN * 2] = "", rl[SUB_NAMELEN * 2] = "", age[16];
        j_str(e, "name", nm, sizeof nm); j_str(e, "role", rl, sizeof rl);
        fmt_age(now - j_num(e, "since", now), age, sizeof age);
        int alive = j_bool(e, "alive");
        printf("%-8lld  %-15s  %-15s  %-9s  %lld%s\n", j_num(e, "pid", 0), nm, rl, age,
               j_num(e, "ops", 0), alive ? "" : "   (gone, not yet reaped)");
        alive ? live++ : stale++;
    }
    if (!live && !stale) printf("(nothing registered)\n");
    else printf("\n%d registered on %s", live + stale, argv[0]);
    if (stale) printf(", %d awaiting reap", stale);
    if (live || stale) printf("\n");
    free(ans);
    return 0;
}

static int print_capture(const char *ans) {
    char out[16384] = "";
    j_str(ans, "out", out, sizeof out);
    fputs(out, stdout);
    if (!j_find(ans, "exit")) return answer_failed(ans) ? 1 : 0;
    return (int)j_num(ans, "exit", 1);
}

static int do_run(int argc, char **argv) {
    if (argc < 1) goto usage;
    const char *peer = argv[0], *name = NULL, *role = "service";
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role = argv[++i];
        else goto usage;
    }
    if (i >= argc) goto usage;
    if (!name) { const char *s = strrchr(argv[i], '/'); name = s ? s + 1 : argv[i]; }
    sbuf req = {0};
    sb_s(&req, "{\"id\":1,\"op\":\"run\",\"name\":"); sb_json(&req, name, 256);
    sb_s(&req, ",\"role\":"); sb_json(&req, role, 256);
    sb_s(&req, ",\"argv\":[");
    for (int k = i; k < argc; k++) { if (k > i) sb_s(&req, ","); sb_json(&req, argv[k], 4096); }
    sb_s(&req, "]}");
    sub_net_conn *c = must_dial(peer);
    if (!c) { free(req.p); return 3; }
    char *ans = ask(c, &req);
    free(req.p); T->close(c);
    if (!ans) return 3;
    int bad = answer_failed(ans);
    if (!bad) printf("sub-net: %s running on %s (pid %lld)\n", name, peer, j_num(ans, "pid", 0));
    free(ans);
    return bad;
usage:
    fprintf(stderr, "usage: sub-net run MACHINE [--name N] [--role R] -- cmd [args...]\n");
    return 2;
}

static int simple_op(const char *peer, sbuf *req) {
    sub_net_conn *c = must_dial(peer);
    if (!c) { free(req->p); return 3; }
    char *ans = ask(c, req);
    free(req->p); T->close(c);
    if (!ans) return 3;
    int rc = print_capture(ans);
    free(ans);
    return rc;
}

static int do_stop(int argc, char **argv) {
    int force = argc > 1 && !strcmp(argv[1], "--force");
    if (argc < 2 + force) { fprintf(stderr, "usage: sub-net stop MACHINE [--force] NAME\n"); return 2; }
    sbuf req = {0};
    sb_f(&req, "{\"id\":1,\"op\":\"stop\",\"force\":%s,\"name\":", force ? "true" : "false");
    sb_json(&req, argv[1 + force], 256); sb_s(&req, "}");
    return simple_op(argv[0], &req);
}

static int do_disable(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sub-net disable MACHINE NAME\n"); return 2; }
    sbuf req = {0};
    sb_s(&req, "{\"id\":1,\"op\":\"disable\",\"name\":"); sb_json(&req, argv[1], 256); sb_s(&req, "}");
    return simple_op(argv[0], &req);
}

static int do_enable(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sub-net enable MACHINE [sub enable's options] -- cmd [args...]\n"); return 2; }
    sbuf req = {0};
    sb_s(&req, "{\"id\":1,\"op\":\"enable\",\"args\":[");
    for (int k = 1; k < argc; k++) { if (k > 1) sb_s(&req, ","); sb_json(&req, argv[k], 4096); }
    sb_s(&req, "]}");
    return simple_op(argv[0], &req);
}

/* Stream the machine's events, one JSON object a line, as they are pushed. */
static int do_events(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: sub-net events MACHINE [--count N]\n"); return 2; }
    long long count = -1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoll(argv[++i]);
    sub_net_conn *c = must_dial(argv[0]);
    if (!c) return 3;
    sbuf req = {0}; sb_s(&req, "{\"id\":1,\"op\":\"events\"}");
    char *ans = ask(c, &req);
    free(req.p);
    if (!ans || answer_failed(ans)) { free(ans); T->close(c); return 3; }
    /* On stderr, so stdout stays pure events: from here on, nothing is missed. */
    fprintf(stderr, "sub-net: subscribed to %s's events after seq %lld\n", argv[0], j_num(ans, "seq", 0));
    free(ans);
    setvbuf(stdout, NULL, _IOLBF, 0);
    char *buf = malloc(SUB_NET_FRAME_MAX + 1);
    long n;
    while (count != 0 && buf && (n = T->recv(c, buf, SUB_NET_FRAME_MAX)) > 0) {
        buf[n] = 0;
        if (!j_find(buf, "ev")) continue;
        printf("%s\n", buf);
        if (count > 0) count--;
    }
    free(buf);
    T->close(c);
    return count == 0 ? 0 : 1;
}

/* A line channel to a machine's listener: stdin lines out, its lines to stdout. */
static int do_connect(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sub-net connect MACHINE CHANNEL [--lines N]\n"); return 2; }
    long long lines = -1;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--lines") && i + 1 < argc) lines = atoll(argv[++i]);
    const char *ch = argv[1];
    if (!chan_ok(ch)) { fprintf(stderr, "sub-net: bad channel name '%s'\n", ch); return 2; }
    sub_net_conn *c = must_dial(argv[0]);
    if (!c) return 3;
    sbuf req = {0}; sb_s(&req, "{\"id\":1,\"op\":\"connect\",\"chan\":"); sb_json(&req, ch, 64); sb_s(&req, "}");
    char *ans = ask(c, &req);
    free(req.p);
    if (!ans || answer_failed(ans)) { free(ans); T->close(c); return 3; }
    free(ans);
    setvbuf(stdout, NULL, _IOLBF, 0);

    char *buf = malloc(SUB_NET_FRAME_MAX + 1), *line = malloc(SUB_NET_FRAME_MAX);
    size_t have = 0;
    int in_open = 1, rc = 0;
    while (lines != 0 && buf && line) {
        struct pollfd pf[2] = { { T->fd(c), POLLIN, 0 }, { 0, POLLIN, 0 } };
        int k = poll(pf, in_open ? 2 : 1, -1);
        if (k < 0) { if (errno == EINTR) continue; break; }
        if (pf[0].revents) {
            long n = T->recv(c, buf, SUB_NET_FRAME_MAX);
            if (n <= 0) { rc = lines > 0 ? 1 : 0; break; }
            buf[n] = 0;
            if (j_bool(buf, "closed")) break;
            const char *m = j_find(buf, "msg"), *me = m ? j_skip(m) : NULL;
            if (!me) continue;
            fwrite(m, 1, (size_t)(me - m), stdout); fputc('\n', stdout);
            if (lines > 0) lines--;
        }
        if (in_open && pf[1].revents) {
            char tmp[4096];
            ssize_t n = read(0, tmp, sizeof tmp);
            if (n <= 0) { in_open = 0; continue; }     /* stdin done; keep listening */
            for (ssize_t i = 0; i < n; i++) {
                if (tmp[i] != '\n') { if (have + 1 < SUB_NET_FRAME_MAX - 128) line[have++] = tmp[i]; continue; }
                line[have] = 0;
                if (have) {
                    sbuf f = {0};
                    sb_s(&f, "{\"chan\":"); sb_json(&f, ch, 64); sb_s(&f, ",\"msg\":");
                    if (j_is_value(line)) sb_put(&f, line, have); else sb_json(&f, line, have);
                    sb_s(&f, "}");
                    int bad = T->send(c, f.p, f.n) < 0;
                    free(f.p);
                    if (bad) { rc = 1; lines = 0; break; }
                }
                have = 0;
            }
        }
    }
    free(buf); free(line);
    T->close(c);
    return rc;
}

/* Serve a named channel on this machine: with a command, the command's stdin
 * and stdout ARE the channel; without one, this process's are. */
static int do_listen(int argc, char **argv) {
    if (argc < 1 || !chan_ok(argv[0])) { fprintf(stderr, "usage: sub-net listen CHANNEL [-- cmd [args...]]\n"); return 2; }
    char sp[PATH_MAX];
    if (unix_sock_path(sp, sizeof sp) < 0) { fprintf(stderr, "sub-net: net folder path too long\n"); return 3; }
    struct sockaddr_un ua; memset(&ua, 0, sizeof ua);
    ua.sun_family = AF_UNIX;
    snprintf(ua.sun_path, sizeof ua.sun_path, "%s", sp);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&ua, sizeof ua) < 0) {
        fprintf(stderr, "sub-net: no 'sub-net serve' running here (%s)\n", sp); return 3;
    }
    char hello[80];
    int hl = snprintf(hello, sizeof hello, "listen %s\n", argv[0]);
    fd_write_all(fd, hello, (size_t)hl);
    char ack[32]; size_t a = 0;
    while (a + 1 < sizeof ack && read(fd, ack + a, 1) == 1 && ack[a] != '\n') a++;
    ack[a] = 0;
    if (strcmp(ack, "ok")) { fprintf(stderr, "sub-net: channel %s: %s\n", argv[0], ack[0] ? ack : "refused"); return 3; }

    int cmd = argc > 2 && !strcmp(argv[1], "--");
    if (cmd) {
        dup2(fd, 0); dup2(fd, 1); close(fd);
        execvp(argv[2], &argv[2]);
        fprintf(stderr, "sub-net: cannot exec %s: %s\n", argv[2], strerror(errno));
        return 127;
    }
    /* stdio bridge */
    int in_open = 1;
    for (;;) {
        struct pollfd pf[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
        if (poll(pf, in_open ? 2 : 1, -1) < 0) { if (errno == EINTR) continue; break; }
        char buf[4096]; ssize_t n;
        if (pf[0].revents) {
            if ((n = read(fd, buf, sizeof buf)) <= 0) break;
            if (fd_write_all(1, buf, (size_t)n)) break;
        }
        if (in_open && pf[1].revents) {
            if ((n = read(0, buf, sizeof buf)) <= 0) { in_open = 0; shutdown(fd, SHUT_WR); continue; }
            if (fd_write_all(fd, buf, (size_t)n)) break;
        }
    }
    close(fd);
    return 0;
}

/* ---- keys and trust, on this machine ---- */

static int do_keygen(int argc, char **argv) {
    int force = argc > 0 && !strcmp(argv[0], "--force");
    char dir[PATH_MAX], kp[PATH_MAX + 16], pp[PATH_MAX + 16];
    net_dir(dir, sizeof dir);
    snprintf(kp, sizeof kp, "%s/key", dir);
    snprintf(pp, sizeof pp, "%s/key.pub", dir);
    if (!force && access(kp, F_OK) == 0) {
        fprintf(stderr, "sub-net: %s already has a key pair; --force replaces it "
                        "(every machine that trusts this one must then be given the new key.pub)\n", dir);
        return 2;
    }
    if (mkdir_p(dir, 0700) < 0) { fprintf(stderr, "sub-net: cannot make %s: %s\n", dir, strerror(errno)); return 1; }
    u8 sk[32], pk[32];
    random_bytes(sk, 32);
    x25519_base(pk, sk);
    char hex[65];
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof tmp, "%s.tmp", kp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    hexenc(sk, 32, hex);
    int bad = fd < 0 || fd_write_all(fd, hex, 64) || fd_write_all(fd, "\n", 1);
    if (fd >= 0) { fsync(fd); close(fd); }
    wipe(hex, sizeof hex); wipe(sk, 32);
    if (bad || rename(tmp, kp) < 0) { fprintf(stderr, "sub-net: cannot write %s\n", kp); unlink(tmp); return 1; }
    FILE *f = fopen(pp, "w");
    hexenc(pk, 32, hex);
    if (!f || fprintf(f, "%s\n", hex) < 0 || fclose(f) != 0) { fprintf(stderr, "sub-net: cannot write %s\n", pp); return 1; }
    printf("sub-net: key pair made in %s\n", dir);
    printf("  key      this machine's secret half (mode 0600; never copy it anywhere)\n");
    printf("  key.pub  the half to give each machine that should trust this one:\n");
    printf("           there, sub-net trust NAME key.pub [--at ADDRESS:PORT]\n");
    return 0;
}

/* Add (or replace) a machine on the trust list, from its key.pub file. */
static int do_trust(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sub-net trust NAME KEY.PUB [--at ADDRESS:PORT]\n"); return 2; }
    const char *name = argv[0], *pub = argv[1], *at = NULL;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--at") && i + 1 < argc) at = argv[++i];
    if (!chan_ok(name)) { fprintf(stderr, "sub-net: name: letters, digits, - _ . only\n"); return 2; }
    if (at) {
        char err[256]; struct sockaddr_storage ss; socklen_t len;
        if (parse_addr(at, &ss, &len, err, sizeof err) < 0) { fprintf(stderr, "sub-net: %s\n", err); return 2; }
    }
    u8 pk[32];
    if (read_hex32(pub, pk) < 0) { fprintf(stderr, "sub-net: %s is not a key.pub\n", pub); return 2; }
    char dir[PATH_MAX], path[PATH_MAX + 16], tmp[PATH_MAX + 32];
    net_dir(dir, sizeof dir);
    if (mkdir_p(dir, 0700) < 0) { fprintf(stderr, "sub-net: cannot make %s\n", dir); return 1; }
    snprintf(path, sizeof path, "%s/peers", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) { fprintf(stderr, "sub-net: cannot write %s\n", tmp); if (in) fclose(in); return 1; }
    chmod(tmp, 0600);
    char hex[65], want[256];
    hexenc(pk, 32, hex);
    snprintf(want, sizeof want, "%s %s%s%s\n", name, hex, at ? " " : "", at ? at : "");
    char line[512];
    int same = 0;
    while (in && fgets(line, sizeof line, in)) {
        char nm[64] = "";
        if (sscanf(line, "%63s", nm) == 1 && !strcmp(nm, name)) { same |= !strcmp(line, want); continue; }   /* replaced below */
        fputs(line, out);
    }
    if (in) fclose(in);
    /* Trusting what is already trusted, the same way, changes nothing: a setup
     * script can run this every time and the file is left as it was. */
    if (same) {
        fclose(out); unlink(tmp);
        printf("sub-net: already trusting %s%s%s\n", name, at ? " at " : "", at ? at : "");
        return 0;
    }
    fputs(want, out);
    if (fclose(out) != 0 || rename(tmp, path) < 0) { fprintf(stderr, "sub-net: cannot write %s\n", path); return 1; }
    printf("sub-net: trusting %s%s%s\n", name, at ? " at " : "", at ? at : "");
    return 0;
}

/* Take a machine off the trust list: it can no longer call in, nor be dialled. */
static int do_untrust(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "usage: sub-net untrust NAME\n"); return 2; }
    const char *name = argv[0];
    char path[PATH_MAX], tmp[PATH_MAX + 16];
    net_file("peers", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *in = fopen(path, "r");
    if (!in) { printf("sub-net: %s was not trusted\n", name); return 0; }
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); fprintf(stderr, "sub-net: cannot write %s\n", tmp); return 1; }
    chmod(tmp, 0600);
    char line[512];
    int hit = 0;
    while (fgets(line, sizeof line, in)) {
        char nm[64] = "";
        if (sscanf(line, "%63s", nm) == 1 && !strcmp(nm, name)) { hit = 1; continue; }
        fputs(line, out);
    }
    fclose(in);
    if (!hit) { fclose(out); unlink(tmp); printf("sub-net: %s was not trusted\n", name); return 0; }
    if (fclose(out) != 0 || rename(tmp, path) < 0) { fprintf(stderr, "sub-net: cannot write %s\n", path); return 1; }
    printf("sub-net: no longer trusting %s\n", name);
    return 0;
}

static int do_peers(void) {
    char path[PATH_MAX]; net_file("peers", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) { printf("(no machines trusted yet: sub-net trust NAME KEY.PUB --at ADDRESS:PORT)\n"); return 0; }
    char line[512];
    printf("%-20s  %s\n", "NAME", "ADDRESS");
    while (fgets(line, sizeof line, f)) {
        char nm[64] = "", hex[80] = "", ad[96] = "";
        if (line[0] == '#' || sscanf(line, "%63s %79s %95s", nm, hex, ad) < 2) continue;
        printf("%-20s  %s\n", nm, ad[0] ? ad : "(calls in only)");
    }
    fclose(f);
    return 0;
}

/* ---- every primitive, against the vectors its RFC publishes ---- */

static int vec(const char *what, const u8 *got, const char *want_hex) {
    size_t n = strlen(want_hex) / 2;
    u8 want[256];
    hexdec(want_hex, want, n);
    int ok = !memcmp(got, want, n);
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    return ok ? 0 : 1;
}

static int do_selftest(void) {
    int bad = 0;
    u8 out[256];
    { sha256_ctx x; sha256_init(&x); sha256_update(&x, "abc", 3); sha256_final(&x, out);
      bad |= vec("SHA-256 'abc' (FIPS 180-2)", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); }
    { const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
      sha256_ctx x; sha256_init(&x); sha256_update(&x, m, strlen(m)); sha256_final(&x, out);
      bad |= vec("SHA-256 two-block message (FIPS 180-2)", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"); }
    { const char *d = "what do ya want for nothing?";
      hmac256((const u8 *)"Jefe", 4, (const u8 *)d, strlen(d), NULL, 0, out);
      bad |= vec("HMAC-SHA-256 (RFC 4231 case 2)", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"); }
    { u8 key[32], nonce[12] = {0,0,0,9,0,0,0,0x4a,0,0,0,0};
      for (int i = 0; i < 32; i++) key[i] = (u8)i;
      chacha_block(key, 1, nonce, out);
      bad |= vec("ChaCha20 block (RFC 8439 2.3.2)", out,
                 "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                 "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e"); }
    { u8 key[32]; const char *kh = "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b";
      hexdec(kh, key, 32);
      const char *m = "Cryptographic Forum Research Group";
      poly_ctx p; poly_init(&p, key); poly_update(&p, (const u8 *)m, strlen(m)); poly_final(&p, out);
      bad |= vec("Poly1305 (RFC 8439 2.5.2)", out, "a8061dc1305136c6c22b8baf0c0127a9"); }
    { u8 key[32], nonce[12] = {7,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
      u8 ad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
      for (int i = 0; i < 32; i++) key[i] = (u8)(0x80 + i);
      const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
                       "for the future, sunscreen would be it.";
      size_t n = strlen(pt);
      u8 ct[160], tag[16], back[160];
      chacha_xor(key, 1, nonce, (const u8 *)pt, ct, n);
      aead_tag(key, nonce, ad, sizeof ad, ct, n, tag);
      bad |= vec("ChaCha20-Poly1305 ciphertext (RFC 8439 2.8.2)", ct, "d31a8d34648e60db7b86afbc53ef7ec2");
      bad |= vec("ChaCha20-Poly1305 tag (RFC 8439 2.8.2)", tag, "1ae10b594f09e26a7e902ecbd0600691");
      /* and the Noise-nonce form round-trips, and refuses a flipped bit */
      u8 sealed[160 + 16];
      aead_seal(key, 42, ad, sizeof ad, (const u8 *)pt, n, sealed);
      int rt = aead_open(key, 42, ad, sizeof ad, sealed, n + 16, back) == 0 && !memcmp(back, pt, n);
      sealed[3] ^= 1;
      int tamper = aead_open(key, 42, ad, sizeof ad, sealed, n + 16, back) != 0;
      printf("  %s  AEAD round trip, and a flipped bit refused\n", rt && tamper ? "ok  " : "FAIL");
      bad |= !(rt && tamper); }
    { u8 k[32], u[32];
      hexdec("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
      hexdec("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
      x25519(out, k, u);
      bad |= vec("X25519 (RFC 7748 5.2)", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"); }
    { u8 a[32], b[32], apub[32], bpub[32], s1[32], s2[32];
      hexdec("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
      hexdec("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
      x25519_base(apub, a); x25519_base(bpub, b);
      bad |= vec("X25519 Alice's public key (RFC 7748 6.1)", apub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
      bad |= vec("X25519 Bob's public key (RFC 7748 6.1)", bpub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
      x25519(s1, a, bpub); x25519(s2, b, apub);
      bad |= vec("X25519 shared secret, Alice's side (RFC 7748 6.1)", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
      bad |= vec("X25519 shared secret, Bob's side (RFC 7748 6.1)", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"); }
    { static const u8 low[32] = {0};             /* a low-order point: refused */
      u8 k[32] = {1}, s[32];
      int refused = dh(s, k, low) != 0;
      printf("  %s  a low-order public key is refused\n", refused ? "ok  " : "FAIL");
      bad |= !refused; }
    printf(bad ? "selftest: FAILED\n" : "selftest: all primitives match their RFC vectors\n");
    return bad;
}

static int usage(void) {
    printf(
      "sub-net %s -- this machine's substrate, reachable from the machines it trusts\n"
      "\n"
      "  sub-net keygen [--force]       make this machine's key pair (in the net folder)\n"
      "  sub-net trust NAME KEY.PUB [--at ADDRESS:PORT]\n"
      "                                 trust a machine; --at is where to reach it\n"
      "  sub-net untrust NAME           stop trusting it\n"
      "  sub-net peers                  the machines trusted here\n"
      "  sub-net dir                    where this machine's net folder is\n"
      "  sub-net serve --bind ADDRESS:PORT\n"
      "                                 serve this machine's registry, process control\n"
      "                                 and events on one private address (never 0.0.0.0)\n"
      "\n"
      "  sub-net ps MACHINE [--json]    its registry\n"
      "  sub-net run MACHINE [--name N] [--role R] -- cmd [args...]\n"
      "  sub-net stop MACHINE [--force] NAME\n"
      "  sub-net enable MACHINE [sub enable's options] -- cmd [args...]\n"
      "  sub-net disable MACHINE NAME\n"
      "  sub-net events MACHINE [--count N]\n"
      "                                 its events, one JSON object a line, pushed live\n"
      "  sub-net listen CHANNEL [-- cmd [args...]]\n"
      "                                 serve a named line channel here (cmd's stdio)\n"
      "  sub-net connect MACHINE CHANNEL [--lines N]\n"
      "                                 a channel there: stdin lines out, its lines in\n"
      "  sub-net selftest               every primitive against its RFC vectors\n"
      "\n"
      "Transport: %s. Net folder: $SUB_NET_DIR, else ", SUB_RELEASE, T->name);
    char d[PATH_MAX]; net_dir(d, sizeof d);
    printf("%s.\nThe segment served is $SUB_SHM, else %s.\n", d, SUB_SHM_NAME);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const char *v = argv[1];
    signal(SIGPIPE, SIG_IGN);
    if (!strcmp(v, "serve"))    return do_serve(argc - 2, argv + 2);
    if (!strcmp(v, "keygen"))   return do_keygen(argc - 2, argv + 2);
    if (!strcmp(v, "trust"))    return do_trust(argc - 2, argv + 2);
    if (!strcmp(v, "untrust"))  return do_untrust(argc - 2, argv + 2);
    if (!strcmp(v, "peers"))    return do_peers();
    if (!strcmp(v, "dir"))      { char d[PATH_MAX]; net_dir(d, sizeof d); printf("%s\n", d); return 0; }
    if (!strcmp(v, "ps"))       return do_ps(argc - 2, argv + 2);
    if (!strcmp(v, "run"))      return do_run(argc - 2, argv + 2);
    if (!strcmp(v, "stop"))     return do_stop(argc - 2, argv + 2);
    if (!strcmp(v, "enable"))   return do_enable(argc - 2, argv + 2);
    if (!strcmp(v, "disable"))  return do_disable(argc - 2, argv + 2);
    if (!strcmp(v, "events"))   return do_events(argc - 2, argv + 2);
    if (!strcmp(v, "connect"))  return do_connect(argc - 2, argv + 2);
    if (!strcmp(v, "listen"))   return do_listen(argc - 2, argv + 2);
    if (!strcmp(v, "selftest")) return do_selftest();
    if (!strcmp(v, "--version")) { printf("sub-net %s (format v%u, %s)\n", SUB_RELEASE, SUB_VERSION, T->name); return 0; }
    if (!strcmp(v, "--help") || !strcmp(v, "-h") || !strcmp(v, "help")) return usage();
    fprintf(stderr, "sub-net: unknown verb '%s' (try: sub-net --help)\n", v);
    return 2;
}
