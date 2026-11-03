# HardlyAFK

HardlyAFK aims to manage multiple independent ARM64 Linux systems on iPhone and iPad, with both terminal and X11 desktop sessions. It is an early development fork of [rcarmo/ios-linuxkit](https://github.com/rcarmo/ios-linuxkit), itself derived from [iSH](https://ish.app/). It is not yet an MVP, an App Store app, or a proven Linux desktop on iOS.

Current development milestone: preserve and rebrand the upstream Alpine ARM64 terminal, verify an unsigned iOS device build, then test boot/network/persistence on a physical iPhone. The upstream already supports multiple importable/exportable Linux roots; HardlyAFK's environment model and desktop viewer are not implemented yet.

Planned dependency chain: physical iPhone shell → environment API and multiple isolated environments → Debian installer → X11 proof → interactive viewer → Openbox/LXQt → Plasma X11 → application launcher and recovery. Android, GNOME, Wayland and Metal are not MVP work.

## Build and test

The iOS build needs macOS/Xcode with Meson and Ninja. Linux runtime tests need an AArch64 host; x86-64 Linux cannot execute the upstream AArch64 gadget engine. A locally unsigned `.app` compiled by CI cannot be installed on a normal iPhone without compatible signing and provisioning.

```sh
git clone --recurse-submodules https://github.com/hardlysleeping/HardlyAFK.git
cd HardlyAFK
python3 -m unittest discover -s tests/host -p 'test_product_identity.py' -v
make test-rootfs-download
```

On macOS, build the existing `iSH-ARM64` scheme with Xcode; see [upstream's iOS build guide](docs/IOS_APPLICATION.md) for its dependency and signing constraints. The fork's `.github/workflows/ios-build.yml` checks an unsigned device compile, not an iPhone installation. The bundled Alpine archive is pinned by SHA-256 in `app/GuestARM64.xcconfig`. No publishing or upstream Fastlane credentials are configured.

## Security, licensing, provenance

Linux guest users are not security principals: the emulator does not confine hostile guest code inside the host app. See [upstream limitations](docs/LIMITATIONS.md) and [security notes](SECURITY.md). Distribution with arbitrary downloaded packages and guest software has App Review uncertainty; approval is not assumed.

The fork preserves Git history and the inherited [GPL and contributor terms](LICENSE.md) and [iOS additional terms](LICENSE.IOS). Third-party sources, submodules and vendored terminal fonts/libraries carry their own notices. Source release and dependency-license review are required before any binary distribution. See [upstream provenance](docs/legacy/) and [fork provenance](docs/UPSTREAM.md).
