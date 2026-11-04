#ifndef AFK_MANAGED_INSTALL_H
#define AFK_MANAGED_INSTALL_H
#include "core/environment/afk_env_catalog.h"
#include "core/distro/afk_distro_catalog.h"
#include "tools/fakefs.h"

/* Isolated provisioning, NOT a runtime lifecycle backend. Caller serializes
 * installs and supplies an immutable app-private archive (the bundled rootfs).
 * Never selects/activates/remounts an environment. Guest symlinks are encoded by
 * fakefs_import as regular host files; host symlink store components are refused.
 * Returns core result codes; output is empty on failure, including rollback. */
int afk_managed_install(struct afk_catalog *catalog, const struct afk_distro *distro,
                        const char *archive, const char *name, struct progress progress,
                        struct afk_env_record *out, char *err, size_t errlen);
#endif
