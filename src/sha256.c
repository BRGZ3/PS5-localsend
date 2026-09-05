#include "sha256.h"

#include <string.h>

static uint32_t rotate_right(uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

static void transform(sha256_context_t *context, const unsigned char block[64]) {
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf), UINT32_C(0xe9b5dba5),
        UINT32_C(0x3956c25b), UINT32_C(0x59f111f1), UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5),
        UINT32_C(0xd807aa98), UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7), UINT32_C(0xc19bf174),
        UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786), UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc),
        UINT32_C(0x2de92c6f), UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8), UINT32_C(0xbf597fc7),
        UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147), UINT32_C(0x06ca6351), UINT32_C(0x14292967),
        UINT32_C(0x27b70a85), UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e), UINT32_C(0x92722c85),
        UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b), UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3),
        UINT32_C(0xd192e819), UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c), UINT32_C(0x34b0bcb5),
        UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a), UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3),
        UINT32_C(0x748f82ee), UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7), UINT32_C(0xc67178f2)};
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        words[index] = ((uint32_t)block[index * 4U] << 24U) |
                       ((uint32_t)block[index * 4U + 1U] << 16U) |
                       ((uint32_t)block[index * 4U + 2U] << 8U) |
                       (uint32_t)block[index * 4U + 3U];
    }
    for (; index < 64U; ++index) {
        uint32_t x = words[index - 15U];
        uint32_t y = words[index - 2U];
        uint32_t s0 = rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
        uint32_t s1 = rotate_right(y, 17U) ^ rotate_right(y, 19U) ^ (y >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }
    a = context->state[0]; b = context->state[1]; c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5]; g = context->state[6]; h = context->state[7];
    for (index = 0U; index < 64U; ++index) {
        uint32_t s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + choice + constants[index] + words[index];
        uint32_t s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    context->state[0] += a; context->state[1] += b; context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f; context->state[6] += g; context->state[7] += h;
}

void sha256_init(sha256_context_t *context) {
    static const uint32_t initial[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85), UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c), UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)};
    memset(context, 0, sizeof(*context));
    memcpy(context->state, initial, sizeof(initial));
}

void sha256_update(sha256_context_t *context, const void *data, size_t size) {
    const unsigned char *input = data;
    context->bit_count += (uint64_t)size * UINT64_C(8);
    while (size > 0U) {
        /* Once the pending partial block is empty, transform complete input
         * blocks in place.  Upload callbacks are much larger than 64 bytes;
         * avoiding a memcpy for every SHA-256 block leaves more CPU for the
         * network and filesystem on the PS5. */
        if (context->block_size == 0U && size >= sizeof(context->block)) {
            transform(context, input);
            input += sizeof(context->block);
            size -= sizeof(context->block);
            continue;
        }
        size_t take = sizeof(context->block) - context->block_size;
        if (take > size) take = size;
        memcpy(context->block + context->block_size, input, take);
        context->block_size += take; input += take; size -= take;
        if (context->block_size == sizeof(context->block)) {
            transform(context, context->block);
            context->block_size = 0U;
        }
    }
}

void sha256_final(sha256_context_t *context, unsigned char digest[SHA256_DIGEST_BYTES]) {
    uint64_t bits = context->bit_count;
    size_t index;
    context->block[context->block_size++] = 0x80U;
    if (context->block_size > 56U) {
        memset(context->block + context->block_size, 0, 64U - context->block_size);
        transform(context, context->block);
        context->block_size = 0U;
    }
    memset(context->block + context->block_size, 0, 56U - context->block_size);
    for (index = 0U; index < 8U; ++index) context->block[63U - index] = (unsigned char)(bits >> (index * 8U));
    transform(context, context->block);
    for (index = 0U; index < 8U; ++index) {
        digest[index * 4U] = (unsigned char)(context->state[index] >> 24U);
        digest[index * 4U + 1U] = (unsigned char)(context->state[index] >> 16U);
        digest[index * 4U + 2U] = (unsigned char)(context->state[index] >> 8U);
        digest[index * 4U + 3U] = (unsigned char)context->state[index];
    }
    memset(context, 0, sizeof(*context));
}

void sha256_hex(const unsigned char digest[SHA256_DIGEST_BYTES], char output[SHA256_HEX_BYTES + 1U]) {
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0U; index < SHA256_DIGEST_BYTES; ++index) {
        output[index * 2U] = digits[digest[index] >> 4U];
        output[index * 2U + 1U] = digits[digest[index] & 15U];
    }
    output[SHA256_HEX_BYTES] = '\0';
}
