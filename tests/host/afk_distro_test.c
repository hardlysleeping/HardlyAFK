/*
 * Host regression for SHA-256 and the distribution pins.
 * usage: afk_distro_test /path/to/pinned-alpine-archive
 */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/distro/afk_distro_catalog.h"
#include "core/distro/afk_sha256.h"

static void digest_of(const char *input, char out[65]) {
    struct afk_sha256 ctx;
    afk_sha256_init(&ctx);
    afk_sha256_update(&ctx, input, strlen(input));
    unsigned char digest[32];
    afk_sha256_final(&ctx, digest);
    afk_sha256_hex(digest, out);
}

static void test_vectors(void) {
    char hex[65];

    digest_of("", hex);
    assert(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    digest_of("abc", hex);
    assert(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    assert(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    /* Exactly one block, and one byte either side of it: padding boundaries. */
    digest_of("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", hex);
    assert(strcmp(hex, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb") == 0);

    /* Streaming in uneven chunks must agree with a single update. */
    struct afk_sha256 ctx;
    afk_sha256_init(&ctx);
    const char *message = "the quick brown fox jumps over the lazy dog";
    afk_sha256_update(&ctx, message, 4);
    afk_sha256_update(&ctx, message + 4, 1);
    afk_sha256_update(&ctx, message + 5, strlen(message) - 5);
    unsigned char digest[32];
    afk_sha256_final(&ctx, digest);
    afk_sha256_hex(digest, hex);
    /* Ground truth from coreutils' sha256sum for this exact lower-case string. */
    assert(strcmp(hex, "05c6e08f1d9fdafa03147fcb8f82f124c76d2f70e3d989dc8aadb5e7d7450bec") == 0);

    /* The published million-'a' vector, streamed. */
    afk_sha256_init(&ctx);
    char block[1000];
    memset(block, 'a', sizeof block);
    for (int i = 0; i < 1000; i++) afk_sha256_update(&ctx, block, sizeof block);
    afk_sha256_final(&ctx, digest);
    afk_sha256_hex(digest, hex);
    assert(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);

    printf("sha256: 5 published vectors ok\n");
}

static void test_lookup(void) {
    size_t count = 0;
    const struct afk_distro *all = afk_distro_all(&count);
    assert(all != NULL && count >= 2);

    const struct afk_distro *alpine = afk_distro_find("alpine", "3.24");
    assert(alpine != NULL);
    assert(alpine->status == AFK_DISTRO_SUPPORTED);
    assert(alpine->sha256 && strlen(alpine->sha256) == 64);
    assert(strcmp(alpine->architecture, "aarch64") == 0);
    assert(afk_distro_find("alpine", NULL) == alpine);

    const struct afk_distro *debian = afk_distro_find("debian", "13");
    assert(debian != NULL);
    assert(debian->status == AFK_DISTRO_PIN_REQUIRED);
    assert(debian->sha256 == NULL);

    assert(afk_distro_find("alpine", "9.99") == NULL);
    assert(afk_distro_find("ubuntu", NULL) == NULL);
    assert(afk_distro_find("", NULL) == NULL);
    assert(strcmp(afk_distro_status_name(AFK_DISTRO_PIN_REQUIRED), "pin-required") == 0);

    /* An unpinned distribution is refused before any file is touched. */
    char err[512] = {0};
    char actual[65] = {0};
    assert(afk_distro_verify_archive(debian, "/nonexistent/archive", actual, sizeof actual,
                                     err, sizeof err) == AFK_DISTRO_ERR_UNPINNED);
    assert(strstr(err, "no pinned checksum") != NULL);

    printf("distro-catalog: alpine pinned, debian pin-required, lookups strict\n");
}

static void test_archive(const char *path) {
    char err[512] = {0};
    char actual[65] = {0};
    const struct afk_distro *alpine = afk_distro_find("alpine", "3.24");
    assert(alpine != NULL);

    assert(afk_distro_verify_archive(alpine, path, actual, sizeof actual, err, sizeof err) ==
           AFK_DISTRO_OK);
    assert(strcmp(actual, alpine->sha256) == 0);
    printf("distro-archive: %s verified %s\n", path, actual);

    /* A single flipped byte must fail, and the computed digest is reported. */
    char copy[600];
    snprintf(copy, sizeof copy, "%s.corrupt", path);
    FILE *in = fopen(path, "rb");
    assert(in != NULL);
    FILE *out = fopen(copy, "wb");
    assert(out != NULL);
    unsigned char buffer[8192];
    size_t read;
    int flipped = 0;
    while ((read = fread(buffer, 1, sizeof buffer, in)) > 0) {
        if (!flipped && read > 0) {
            buffer[read / 2] ^= 0x01;
            flipped = 1;
        }
        assert(fwrite(buffer, 1, read, out) == read);
    }
    fclose(in);
    fclose(out);

    memset(err, 0, sizeof err);
    memset(actual, 0, sizeof actual);
    assert(afk_distro_verify_archive(alpine, copy, actual, sizeof actual, err, sizeof err) ==
           AFK_DISTRO_ERR_DIGEST_MISMATCH);
    assert(strstr(err, "checksum mismatch") != NULL);
    assert(strcmp(actual, alpine->sha256) != 0);
    remove(copy);

    memset(err, 0, sizeof err);
    assert(afk_distro_verify_archive(alpine, "/nonexistent/archive", NULL, 0, err, sizeof err) ==
           AFK_DISTRO_ERR_IO);
    printf("distro-archive: corrupted copy rejected, missing file reported\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/pinned-alpine-archive\n", argv[0]);
        return 2;
    }
    test_vectors();
    test_lookup();
    test_archive(argv[1]);
    printf("distro-test: PASS\n");
    return 0;
}
