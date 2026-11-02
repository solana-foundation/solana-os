#pragma once

// The part of PSA Crypto (ESP-IDF 6's mbedTLS 4) Solana OS uses: SHA-256
// hashing, over the emulator's own SHA-256 (platform/esp_emu.c).

#include <stddef.h>
#include <stdint.h>
#include "mbedtls/sha256.h"

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS                 ((psa_status_t)0)
#define PSA_ERROR_NOT_SUPPORTED     ((psa_status_t)-134)
#define PSA_ERROR_BUFFER_TOO_SMALL  ((psa_status_t)-138)
#define PSA_ALG_SHA_256             ((psa_algorithm_t)0x02000009)

typedef struct {
    mbedtls_sha256_context ctx;
} psa_hash_operation_t;

#define PSA_HASH_OPERATION_INIT {0}

static inline psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    if (alg != PSA_ALG_SHA_256) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    mbedtls_sha256_init(&op->ctx);
    mbedtls_sha256_starts(&op->ctx, 0);
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t n)
{
    mbedtls_sha256_update(&op->ctx, in, n);
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t cap, size_t *len)
{
    if (cap < 32) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    mbedtls_sha256_finish(&op->ctx, out);
    *len = 32;
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    mbedtls_sha256_free(&op->ctx);
    return PSA_SUCCESS;
}

static inline psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t *in, size_t n, uint8_t *out,
                                            size_t cap, size_t *len)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t st = psa_hash_setup(&op, alg);
    if (st == PSA_SUCCESS) {
        psa_hash_update(&op, in, n);
        st = psa_hash_finish(&op, out, cap, len);
    }
    psa_hash_abort(&op);
    return st;
}
