#ifndef AFK_IOS_BOOT_H
#define AFK_IOS_BOOT_H
#include "afk_env_catalog.h"
/* Resolve a terminal/root arm64 fakefs without modifying it. No runtime lifecycle. */
int afk_ios_boot_root(struct afk_catalog *catalog, const char *id,
                      char out[AFK_ENV_PATH_MAX], char *err, size_t errlen);
#endif
