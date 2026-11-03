# Upstream provenance

HardlyAFK was forked from `rcarmo/ios-linuxkit` master at commit `abe413540d4ff8b4ad6b52447ab80f824c58495f`. The fork retains the parent commit history, original license texts (`LICENSE.md`, `LICENSE.IOS`), submodule pointers and the original developer notes under `docs/legacy/`. The project itself derives from iSH. The original upstream Fastlane configuration was removed from the active branch to prevent accidental upload using another publisher's account; its history remains available.

The iOS app currently still uses upstream's `iSH-ARM64` Xcode scheme and internal C `ish` names. Those identifiers are build/runtime implementation details, not a claim of upstream sponsorship. Runtime and application APIs will be separated incrementally after the physical-device shell gate.

Before distributing binaries, inventory third-party licenses (including submodules, libraries, assets and fonts), provide complete corresponding source and build instructions at the shipped commit, and obtain a legal review of Apple distribution terms. `LICENSE.IOS` is the inherited iSH notice; it is not a blanket promise on behalf of every dependency or contributor.
