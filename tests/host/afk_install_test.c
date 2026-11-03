/*
 * Host regression for installing a distribution into a new environment.
 * usage: afk_install_test /path/to/alpine.tar.gz /path/to/debian-layer.tar.gz
 */
#define _DEFAULT_SOURCE
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/api/afk_environment_api.h"

struct mock {
    int starts;
    int unpack_calls;
    int fail_unpack;
    char last_archive[512];
    char last_root[512];
    char order[512];
};

static int backend_start(void *ctx, const struct afk_env_record *env, const char *root_path,
                         char *err, size_t errlen) {
    (void) root_path;
    (void) err;
    (void) errlen;
    struct mock *m = ctx;
    m->starts++;
    snprintf(m->order + strlen(m->order), sizeof m->order - strlen(m->order), "start:%s;", env->name);
    return AFK_ENV_OK;
}

static int backend_stop(void *ctx, char *err, size_t errlen) {
    (void) err;
    (void) errlen;
    struct mock *m = ctx;
    snprintf(m->order + strlen(m->order), sizeof m->order - strlen(m->order), "stop;");
    return AFK_ENV_OK;
}

static int backend_run(void *ctx, const char *const *argv, int argc, afk_output_sink sink,
                       void *sink_ctx, char *err, size_t errlen) {
    (void) ctx;
    (void) argv;
    (void) argc;
    (void) sink;
    (void) sink_ctx;
    (void) err;
    (void) errlen;
    return AFK_ENV_OK;
}

static const struct afk_host_backend backend = {
    .name = "mock",
    .start = backend_start,
    .stop = backend_stop,
    .run_command = backend_run,
};

static int unpack(void *ctx, const char *archive_path, const char *root_path,
                  char *err, size_t errlen) {
    struct mock *m = ctx;
    m->unpack_calls++;
    snprintf(m->last_archive, sizeof m->last_archive, "%s", archive_path);
    snprintf(m->last_root, sizeof m->last_root, "%s", root_path);
    if (m->fail_unpack) {
        snprintf(err, errlen, "mock unpacker refused");
        return -1;
    }
    /* The destination must already exist and be empty: the core creates it. */
    DIR *dir = opendir(root_path);
    assert(dir != NULL);
    int entries = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) entries++;
    closedir(dir);
    assert(entries == 0);

    char marker[600];
    snprintf(marker, sizeof marker, "%s/.afk-unpacked", root_path);
    FILE *file = fopen(marker, "wb");
    assert(file != NULL);
    fputs("unpacked\n", file);
    fclose(file);
    return 0;
}

static int count_entries(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int entries = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) entries++;
    closedir(dir);
    return entries;
}

/* Number of environment directories; zero when the store has not been created. */
static int count_environments(const char *root) {
    char path[600];
    snprintf(path, sizeof path, "%s/environments", root);
    int entries = count_entries(path);
    return entries < 0 ? 0 : entries;
}

static int count_listed;

static void summarise(const struct afk_env_record *record, void *ctx) {
    (void) ctx;
    (void) record;
    count_listed++;
}

static int listed(struct afk_api *api) {
    count_listed = 0;
    /* afk_api_list returns the number of records visited, or a negative code. */
    int rc = afk_api_list(api, summarise, NULL);
    assert(rc >= 0);
    assert(rc == count_listed);
    return count_listed;
}

static void temp_dir(char *out, size_t len) {
    const char *base = getenv("TMPDIR");
    snprintf(out, len, "%s/afk-install-XXXXXX", base ? base : "/tmp");
    assert(mkdtemp(out) != NULL);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s /path/to/alpine.tar.gz /path/to/debian-layer.tar.gz\n", argv[0]);
        return 2;
    }
    const char *alpine_archive = argv[1];
    const char *debian_archive = argv[2];

    char root[512];
    char err[512] = {0};
    temp_dir(root, sizeof root);

    struct mock mock;
    memset(&mock, 0, sizeof mock);
    struct afk_host_backend bound = backend;
    bound.context = &mock;
    struct afk_api *api = NULL;
    assert(afk_api_open(root, &bound, &api, err, sizeof err) == AFK_ENV_OK);

    assert(count_environments(root) == 0);

    /* An environment that already exists, with data that must survive failures. */
    struct afk_env_spec existing = {
        .name = "Existing", .distro_id = "alpine", .distro_version = "3.24",
        .architecture = "aarch64", .default_user = "root",
        .default_session = AFK_ENV_SESSION_TERMINAL, .created_at = 1700000000,
    };
    struct afk_env_record existing_record;
    assert(afk_api_create_environment(api, &existing, &existing_record, err, sizeof err) ==
           AFK_ENV_OK);
    char existing_root[AFK_ENV_PATH_MAX];
    assert(count_environments(root) == 1);
    assert(snprintf(existing_root, sizeof existing_root, "%s/environments/%s/rootfs", root,
                    existing_record.id) > 0);
    char sentinel[1300];
    snprintf(sentinel, sizeof sentinel, "%s/keep-me", existing_root);
    FILE *keep = fopen(sentinel, "wb");
    assert(keep != NULL);
    fputs("precious\n", keep);
    fclose(keep);

    struct afk_unpacker unpacker = {.context = &mock, .unpack = unpack};

    /* A corrupt archive changes nothing at all. */
    char corrupt[600];
    snprintf(corrupt, sizeof corrupt, "%s.corrupt", debian_archive);
    FILE *in = fopen(debian_archive, "rb");
    assert(in != NULL);
    FILE *out = fopen(corrupt, "wb");
    assert(out != NULL);
    unsigned char buffer[65536];
    size_t read;
    int flipped = 0;
    while ((read = fread(buffer, 1, sizeof buffer, in)) > 0) {
        if (!flipped) {
            buffer[read / 3] ^= 0x40;
            flipped = 1;
        }
        assert(fwrite(buffer, 1, read, out) == read);
    }
    fclose(in);
    fclose(out);

    struct afk_env_spec debian = {
        .name = "Debian Work", .distro_id = "debian", .distro_version = "13",
        .architecture = "aarch64", .default_user = "hardly",
        .default_session = AFK_ENV_SESSION_DESKTOP, .created_at = 1700000100,
    };
    struct afk_env_record record;
    memset(&record, 0xff, sizeof record);
    const struct afk_distro *debian_distro = afk_distro_find("debian", "13");
    assert(debian_distro != NULL);
    memset(err, 0, sizeof err);
    assert(afk_api_install_environment(api, debian_distro, corrupt, &unpacker, &debian, &record,
                                       err, sizeof err) == AFK_DISTRO_ERR_DIGEST_MISMATCH);
    assert(strstr(err, "checksum mismatch") != NULL);
    assert(strstr(err, "Debian") != NULL);
    assert(mock.unpack_calls == 0);            /* the unpacker never saw it */
    assert(listed(api) == 1);                  /* no environment appeared */
    assert(count_environments(root) == 1);          /* no directory was created */
    assert(access(sentinel, F_OK) == 0);       /* existing data untouched */
    assert(record.name[0] == '\0');            /* out is cleared, not half-filled */
    remove(corrupt);

    /* An unpinned distribution is refused for the same reason. */
    const struct afk_distro *arch = afk_distro_find("arch", "latest");
    assert(arch != NULL);
    assert(afk_api_install_environment(api, arch, alpine_archive, &unpacker, &debian, NULL, err,
                                       sizeof err) == AFK_DISTRO_ERR_UNPINNED);
    assert(mock.unpack_calls == 0);
    assert(listed(api) == 1);

    /* A failed unpack rolls the environment back out of the list and disk. */
    mock.fail_unpack = 1;
    assert(afk_api_install_environment(api, debian_distro, debian_archive, &unpacker, &debian,
                                       NULL, err, sizeof err) == AFK_API_ERR_BACKEND);
    assert(strstr(err, "mock unpacker refused") != NULL);
    assert(mock.unpack_calls == 1);
    assert(listed(api) == 1);
    assert(count_environments(root) == 1);
    assert(access(sentinel, F_OK) == 0);
    mock.fail_unpack = 0;

    /* A real Debian rootfs installs, and the unpacker is handed the pinned bytes. */
    assert(afk_api_install_environment(api, debian_distro, debian_archive, &unpacker, &debian,
                                       &record, err, sizeof err) == AFK_ENV_OK);
    assert(strcmp(record.name, "Debian Work") == 0);
    assert(strcmp(record.distro_id, "debian") == 0);
    assert(strcmp(mock.last_archive, debian_archive) == 0);
    assert(strstr(mock.last_root, record.id) != NULL);
    assert(listed(api) == 2);
    assert(count_environments(root) == 2);
    char marker[1300];
    snprintf(marker, sizeof marker, "%s/.afk-unpacked", mock.last_root);
    assert(access(marker, F_OK) == 0);
    char debian_root[AFK_ENV_PATH_MAX];
    snprintf(debian_root, sizeof debian_root, "%s/environments/%s/rootfs", root, record.id);
    assert(strcmp(debian_root, mock.last_root) == 0);

    /* The installed environment is ordinary: it boots and stops like any other. */
    assert(afk_api_start(api, record.id, err, sizeof err) == AFK_ENV_OK);
    assert(mock.starts == 1);

    /* Installing while a guest runs is refused before any verification work. */
    struct afk_env_spec alpine = {
        .name = "Alpine Test", .distro_id = "alpine", .distro_version = "3.24",
        .architecture = "aarch64", .default_user = "root",
        .default_session = AFK_ENV_SESSION_TERMINAL, .created_at = 1700000200,
    };
    mock.unpack_calls = 0;
    assert(afk_api_install_environment(api, afk_distro_find("alpine", "3.24"), alpine_archive,
                                       &unpacker, &alpine, NULL, err, sizeof err) ==
           AFK_SESSION_BUSY);
    assert(mock.unpack_calls == 0);
    assert(strstr(err, "stop the running environment") != NULL);

    assert(afk_api_stop(api, err, sizeof err) == AFK_ENV_OK);

    /* A guest-architecture mismatch is refused rather than installed. */
    struct afk_env_spec wrong_arch = alpine;
    wrong_arch.name = "Wrong Arch";
    wrong_arch.architecture = "x86_64";
    assert(afk_api_install_environment(api, afk_distro_find("alpine", "3.24"), alpine_archive,
                                       &unpacker, &wrong_arch, NULL, err, sizeof err) ==
           AFK_ENV_ERR_INVALID);
    assert(strstr(err, "built for aarch64") != NULL);

    /* The Alpine tarball installs too, from its own pinned digest. */
    assert(afk_api_install_environment(api, afk_distro_find("alpine", "3.24"), alpine_archive,
                                       &unpacker, &alpine, &record, err, sizeof err) == AFK_ENV_OK);
    assert(strcmp(record.distro_id, "alpine") == 0);
    assert(listed(api) == 3);

    int environments = listed(api);
    afk_api_close(api);
    printf("install-test: PASS (unpack_calls=%d environments=%d)\n", mock.unpack_calls,
           environments);
    return 0;
}
