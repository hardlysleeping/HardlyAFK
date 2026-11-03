#ifndef AFK_SHA256_H
#define AFK_SHA256_H

/*
 * Portable SHA-256 (FIPS 180-4) used to verify downloaded root filesystems.
 *
 * This is an integrity check, not a secret: it exists so the core can verify a
 * pinned digest identically on Apple, Linux and Android hosts without depending
 * on CommonCrypto or OpenSSL being reachable from every call site. It is
 * validated against published test vectors and against the operating system's
 * own sha256 for the pinned archive.
 */

#include <stddef.h>
#include <stdint.h>

struct afk_sha256 {
    uint32_t state[8];
    uint64_t length;
    unsigned char buffer[64];
    size_t buffered;
};

void afk_sha256_init(struct afk_sha256 *ctx);
void afk_sha256_update(struct afk_sha256 *ctx, const void *data, size_t length);
void afk_sha256_final(struct afk_sha256 *ctx, unsigned char digest[32]);

/* Lowercase hex, NUL terminated: out must hold 65 bytes. */
void afk_sha256_hex(const unsigned char digest[32], char out[65]);

/* Streams a file; returns 0 on success and -1 with a message on failure. */
int afk_sha256_file(const char *path, unsigned char digest[32], char *err, size_t errlen);
int afk_sha256_file_hex(const char *path, char out[65], char *err, size_t errlen);

#endif
