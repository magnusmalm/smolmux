/*
 * Minimal SHA-1 implementation for WebSocket handshake.
 * RFC 3174 compliant. Not for security-critical use.
 */
#include "util/sha1.h"
#include <string.h>

static uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1_block(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2];
    uint32_t d = state[3], e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;           k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;           k = 0xCA62C1D6; }

        uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
    }

    state[0] += a; state[1] += b; state[2] += c;
    state[3] += d; state[4] += e;
}

void sm_sha1(const uint8_t *data, size_t len, uint8_t out[SM_SHA1_DIGEST_SIZE])
{
    uint32_t state[5] = {
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
    };

    size_t i;
    for (i = 0; i + 64 <= len; i += 64)
        sha1_block(state, data + i);

    /* Padding */
    uint8_t pad[128];
    size_t rem = len - i;
    memcpy(pad, data + i, rem);
    pad[rem++] = 0x80;

    /* Need room for 8-byte length */
    size_t pad_len = (rem <= 56) ? 64 : 128;
    memset(pad + rem, 0, pad_len - rem);

    /* Append bit length as big-endian 64-bit */
    uint64_t bits = (uint64_t)len * 8;
    pad[pad_len - 8] = (uint8_t)(bits >> 56);
    pad[pad_len - 7] = (uint8_t)(bits >> 48);
    pad[pad_len - 6] = (uint8_t)(bits >> 40);
    pad[pad_len - 5] = (uint8_t)(bits >> 32);
    pad[pad_len - 4] = (uint8_t)(bits >> 24);
    pad[pad_len - 3] = (uint8_t)(bits >> 16);
    pad[pad_len - 2] = (uint8_t)(bits >> 8);
    pad[pad_len - 1] = (uint8_t)(bits);

    for (size_t j = 0; j < pad_len; j += 64)
        sha1_block(state, pad + j);

    /* Output big-endian */
    for (int j = 0; j < 5; j++) {
        out[j * 4]     = (uint8_t)(state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(state[j]);
    }
}
