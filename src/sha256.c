/*
 * sha256.c - a small, self-contained SHA-256 implementation (FIPS 180-4).
 *
 * Clean-room: written from the published FIPS 180-4 description, with no
 * code taken from OpenSSL or any other library, so Masa gains a hashing
 * primitive for `masa brew fetch` without a new build dependency.
 *
 * Copyright (C) 2026 Masa contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "masa.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* First 32 bits of the fractional parts of the cube roots of the first
 * 64 primes (FIPS 180-4 section 4.2.2). */
static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

void masa_sha256_init(struct masa_sha256_ctx *ctx)
{
    /* First 32 bits of the fractional parts of the square roots of the
     * first 8 primes (FIPS 180-4 section 5.3.3). */
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0;
    ctx->blocklen = 0;
}

static void sha256_block(struct masa_sha256_ctx *ctx, const unsigned char *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void masa_sha256_update(struct masa_sha256_ctx *ctx, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;

    ctx->bitlen += (uint64_t)len * 8u;

    while (len > 0) {
        size_t space = 64 - ctx->blocklen;
        size_t take = (len < space) ? len : space;

        memcpy(ctx->block + ctx->blocklen, p, take);
        ctx->blocklen += take;
        p += take;
        len -= take;

        if (ctx->blocklen == 64) {
            sha256_block(ctx, ctx->block);
            ctx->blocklen = 0;
        }
    }
}

void masa_sha256_final(struct masa_sha256_ctx *ctx, unsigned char out[32])
{
    unsigned char pad[72];
    unsigned char lenbuf[8];
    uint64_t total = ctx->bitlen;   /* length before padding is appended */
    size_t padlen;
    int i;

    /* Append a single 1 bit, then zeros, then the 64-bit big-endian length
     * so the padded message is a whole number of 64-byte blocks. */
    pad[0] = 0x80;
    memset(pad + 1, 0, sizeof(pad) - 1);
    padlen = (ctx->blocklen < 56) ? (56 - ctx->blocklen)
                                  : (120 - ctx->blocklen);
    masa_sha256_update(ctx, pad, padlen);

    for (i = 0; i < 8; i++) {
        lenbuf[i] = (unsigned char)(total >> (56 - i * 8));
    }
    masa_sha256_update(ctx, lenbuf, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
    }
}

static void sha256_to_hex(const unsigned char digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hex[64] = '\0';
}

int masa_sha256_buffer(const void *data, size_t len, char hex[65])
{
    struct masa_sha256_ctx ctx;
    unsigned char digest[32];

    masa_sha256_init(&ctx);
    masa_sha256_update(&ctx, data, len);
    masa_sha256_final(&ctx, digest);
    sha256_to_hex(digest, hex);
    return 0;
}

int masa_sha256_file(const char *path, char hex[65])
{
    struct masa_sha256_ctx ctx;
    unsigned char digest[32];
    unsigned char buf[65536];
    FILE *fp;
    size_t got;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    masa_sha256_init(&ctx);
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
        masa_sha256_update(&ctx, buf, got);
    }
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    masa_sha256_final(&ctx, digest);
    sha256_to_hex(digest, hex);
    return 0;
}
