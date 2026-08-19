#include "util/base64.h"

#include <stdlib.h>
#include <string.h>

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t b64_decode_table[256] = {
    ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,
    ['F'] = 5,  ['G'] = 6,  ['H'] = 7,  ['I'] = 8,  ['J'] = 9,
    ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14,
    ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
    ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24,
    ['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29,
    ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34,
    ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
    ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44,
    ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
    ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54,
    ['3'] = 55, ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
    ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63,
};

char *sm_base64_encode(const uint8_t *data, size_t len)
{
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i + 1] << 8) |
                     (uint32_t)data[i + 2];
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = b64_table[(v >> 6) & 0x3F];
        out[j++] = b64_table[v & 0x3F];
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

static int is_b64_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

uint8_t *sm_base64_decode(const char *b64, size_t b64_len, size_t *out_len)
{
    if (b64_len == 0) {
        *out_len = 0;
        uint8_t *out = malloc(1);
        if (out) out[0] = 0;
        return out;
    }

    /* Validate: length must be multiple of 4 and all chars valid */
    if (b64_len % 4 != 0)
        return NULL;
    for (size_t i = 0; i < b64_len; i++) {
        if (!is_b64_char((unsigned char)b64[i]))
            return NULL;
        /* '=' only allowed in last two positions */
        if (b64[i] == '=' && i < b64_len - 2)
            return NULL;
    }

    size_t pad = 0;
    if (b64[b64_len - 1] == '=') pad++;
    if (b64[b64_len - 2] == '=') pad++;

    size_t decoded_len = (b64_len / 4) * 3 - pad;
    uint8_t *out = malloc(decoded_len + 1);
    if (!out) return NULL;

    size_t i = 0, j = 0;
    for (; i + 3 < b64_len; i += 4) {
        uint32_t v = ((uint32_t)b64_decode_table[(unsigned char)b64[i]] << 18) |
                     ((uint32_t)b64_decode_table[(unsigned char)b64[i + 1]] << 12) |
                     ((uint32_t)b64_decode_table[(unsigned char)b64[i + 2]] << 6) |
                     (uint32_t)b64_decode_table[(unsigned char)b64[i + 3]];
        if (j < decoded_len) out[j++] = (uint8_t)(v >> 16);
        if (j < decoded_len) out[j++] = (uint8_t)(v >> 8);
        if (j < decoded_len) out[j++] = (uint8_t)v;
    }

    *out_len = decoded_len;
    return out;
}
