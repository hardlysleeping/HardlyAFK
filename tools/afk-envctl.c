/*
 * afk-envctl: command-line front end for the environment catalog.
 *
 * This is the host-side proof that environments are real, independent objects:
 * the same calls the Apple UI will make, driven from a shell so they can be
 * exercised without a device. It does not boot Linux.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "core/environment/afk_env_catalog.h"
#include "core/environment/afk_env_store.h"

static char err[512];

static void usage(void) {
    fputs("usage: afk-envctl [--root DIR] <command>\n"
          "\n"
          "  create --name NAME --distro ID [--version V] [--session terminal|desktop]\n"
          "         [--user U]\n"
          "  list\n"
          "  show ID\n"
          "  rename ID NEW_NAME\n"
          "  activate ID | activate --none\n"
          "  delete ID\n"
          "  usage ID\n"
          "  path ID\n"
          "\n"
          "--root defaults to $AFK_CATALOG_DIR, then $HOME/.hardlyafk.\n", stderr);
}

static void die(const char *message) {
    fprintf(stderr, "afk-envctl: %s\n", err[0] ? err : message);
    exit(1);
}

static const char *arg_value(int argc, char **argv, const char *flag) {
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], flag) == 0 && i + 1 < argc) return argv[i + 1];
    return NULL;
}

static void print_record(const struct afk_env_record *env) {
    printf("%-36s  %-20s  %-8s %-6s  %-8s  %s\n", env->id, env->name, env->distro_id,
           env->distro_version, env->default_session, env->relative_path);
}

static void visit(const struct afk_env_record *env, void *ctx) {
    (void) ctx;
    print_record(env);
}

static int ensure_root(char *out, size_t outlen, int argc, char **argv) {
    const char *dir = arg_value(argc, argv, "--root");
    if (!dir || !*dir) dir = getenv("AFK_CATALOG_DIR");
    if (!dir || !*dir) {
        const char *home = getenv("HOME");
        if (!home) return -1;
        snprintf(out, outlen, "%s/.hardlyafk", home);
    } else {
        snprintf(out, outlen, "%s", dir);
    }
    if (mkdir(out, 0700) != 0) {
        struct stat st;
        if (stat(out, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    char root[AFK_ENV_PATH_MAX];
    if (ensure_root(root, sizeof root, argc, argv) != 0) die("cannot prepare the catalog directory");

    /* Skip the program name and any --root pair to find the command. */
    const char *command = NULL;
    int first = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0) {
            i++;
            continue;
        }
        command = argv[i];
        first = i;
        break;
    }
    if (!command) {
        usage();
        return 2;
    }

    struct afk_catalog *catalog = NULL;
    if (afk_catalog_open(root, &catalog, err, sizeof err) != AFK_ENV_OK) die("cannot open the catalog");

    if (strcmp(command, "list") == 0) {
        char active[AFK_ENV_ID_LEN + 1];
        int has_active = afk_catalog_active(catalog, active) == AFK_ENV_OK;
        printf("%-36s  %-20s  %-8s %-6s  %-8s  %s\n", "ID", "NAME", "DISTRO", "VER", "SESSION", "PATH");
        int count = afk_catalog_list(catalog, visit, NULL);
        if (count < 0) die("cannot list environments");
        if (has_active) printf("\nactive: %s\n", active);
        else printf("\nactive: (none)\n");
    } else if (strcmp(command, "create") == 0) {
        const char *name = arg_value(argc, argv, "--name");
        const char *distro = arg_value(argc, argv, "--distro");
        if (!name || !distro) {
            fprintf(stderr, "afk-envctl: create requires --name and --distro\n");
            return 2;
        }
        struct afk_env_spec spec = {
            .name = name,
            .distro_id = distro,
            .distro_version = arg_value(argc, argv, "--version"),
            .architecture = "aarch64",
            .default_user = arg_value(argc, argv, "--user"),
            .default_session = arg_value(argc, argv, "--session"),
            .created_at = 0,
        };
        struct afk_env_record created;
        int rc = afk_store_create(catalog, &spec, &created, err, sizeof err);
        if (rc != AFK_ENV_OK) die("cannot create the environment");
        char path[AFK_ENV_PATH_MAX];
        if (afk_catalog_resolve_root(catalog, &created, path, sizeof path) != AFK_ENV_OK)
            die("cannot resolve the environment path");
        printf("%s\n", created.id);
        fprintf(stderr, "created \"%s\" at %s\n", created.name, path);
    } else if (strcmp(command, "show") == 0 || strcmp(command, "usage") == 0) {
        if (first + 1 >= argc) {
            fprintf(stderr, "afk-envctl: %s requires an environment id\n", command);
            return 2;
        }
        struct afk_env_record env;
        if (afk_catalog_get(catalog, argv[first + 1], &env) != AFK_ENV_OK) die("environment not found");
        print_record(&env);
        if (strcmp(command, "usage") == 0) {
            struct afk_env_usage used;
            if (afk_store_usage(catalog, env.id, &used, err, sizeof err) != AFK_ENV_OK)
                die("cannot measure the environment");
            printf("files: %llu  directories: %llu  bytes: %llu\n",
                   (unsigned long long) used.files, (unsigned long long) used.directories,
                   (unsigned long long) used.bytes);
        }
    } else if (strcmp(command, "path") == 0) {
        if (first + 1 >= argc) {
            fprintf(stderr, "afk-envctl: path requires an environment id\n");
            return 2;
        }
        struct afk_env_record env;
        char path[AFK_ENV_PATH_MAX];
        if (afk_catalog_get(catalog, argv[first + 1], &env) != AFK_ENV_OK) die("environment not found");
        if (afk_catalog_resolve_root(catalog, &env, path, sizeof path) != AFK_ENV_OK)
            die("cannot resolve the environment path");
        printf("%s\n", path);
    } else if (strcmp(command, "rename") == 0) {
        if (first + 2 >= argc) {
            fprintf(stderr, "afk-envctl: rename requires an environment id and a new name\n");
            return 2;
        }
        if (afk_catalog_rename(catalog, argv[first + 1], argv[first + 2], err, sizeof err) != AFK_ENV_OK)
            die("cannot rename the environment");
        printf("renamed to \"%s\"\n", argv[first + 2]);
    } else if (strcmp(command, "activate") == 0) {
        if (first + 1 >= argc) {
            fprintf(stderr, "afk-envctl: activate requires an environment id or --none\n");
            return 2;
        }
        const char *target = strcmp(argv[first + 1], "--none") == 0 ? "" : argv[first + 1];
        if (afk_catalog_set_active(catalog, target, err, sizeof err) != AFK_ENV_OK)
            die("cannot activate the environment");
        printf("%s\n", *target ? target : "(none)");
    } else if (strcmp(command, "delete") == 0) {
        if (first + 1 >= argc) {
            fprintf(stderr, "afk-envctl: delete requires an environment id\n");
            return 2;
        }
        if (afk_store_delete(catalog, argv[first + 1], err, sizeof err) != AFK_ENV_OK)
            die("cannot delete the environment");
        printf("deleted %s\n", argv[first + 1]);
    } else {
        usage();
        afk_catalog_close(catalog);
        return 2;
    }

    afk_catalog_close(catalog);
    return 0;
}
