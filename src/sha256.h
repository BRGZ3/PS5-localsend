#ifndef PS5LOCALSEND_SHA256_H
#define PS5LOCALSEND_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_BYTES 32U
#define SHA256_HEX_BYTES 64U

typedef struct sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    unsigned char block[64];
    size_t block_size;
} sha256_context_t;

void sha256_init(sha256_context_t *context);
void sha256_update(sha256_context_t *context, const void *data, size_t size);
void sha256_final(sha256_context_t *context,
                  unsigned char digest[SHA256_DIGEST_BYTES]);
void sha256_hex(const unsigned char digest[SHA256_DIGEST_BYTES],
                char output[SHA256_HEX_BYTES + 1U]);

#endif
