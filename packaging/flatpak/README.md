# Experimental Flatpak

Desktop-mode packaging for immutable systems such as Bazzite and SteamOS.
The package contains the Qt Quick GUI, a separate background runtime, and `tryx`.
Hardware acceptance on these systems is still required before declaring support.

## Requirements and boundaries

- Flatpak 1.15.11 or newer, a working USB portal and compatible desktop backend.
- Media conversion uses KDE Platform's `org.freedesktop.Platform.codecs-extra`
  extension. Keep the runtime's related extensions enabled when installing.
- Printer-class product profiles `391a:1011`, `391a:1021`, `391a:2011` only.
  Legacy serial/ADB identities, including `18d1:2d04`, are not exposed in this build.
- USB enumeration is narrowly scoped. The portal asks for device access once per
  device identity; denial does not trigger a loop of permission prompts.
  Portal permission does not bypass the host's existing USB access controls.
- Files and export folders are chosen through the system FileChooser portal.
  No whole-home/host access, raw USB grant, full session/system bus, host systemd
  unit, udev rule, or host command execution is included.
- Firmware validation/flashing and desktop autostart are explicitly unavailable
  in this first experimental build. Use the native application for firmware work.
  Metrics requiring host-only tools such as `nvidia-smi` may be unavailable.
- Native and Flatpak runtimes cannot own the device concurrently. Finish active
  operations and stop the other runtime yourself; the Flatpak never stops it.
  Data stays app-private and is not automatically migrated from native installs.

## Local build

Install `flatpak-builder` and configure the official Flathub remote first.
The commands below use fish. KDE SDK/Platform 6.11 are the chosen compatibility
baseline; their branch receives updates. Dependency archives and CI actions are
pinned, but this does not claim bit-for-bit reproducibility across SDK updates.

```fish
flatpak install --user flathub org.kde.Sdk//6.11 org.kde.Platform//6.11
mkdir -p build
set flatpak_work (mktemp -d (pwd)/build/flatpak-XXXXXX)
flatpak-builder --user --force-clean --jobs=2 \
    --state-dir="$flatpak_work/state" --repo="$flatpak_work/repo" \
    "$flatpak_work/app" packaging/flatpak/io.github.dxvsi.tryx_panorama_manager.yml
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$flatpak_work/repo" "$flatpak_work/tryx-panorama-manager-x86_64.flatpak" \
    io.github.dxvsi.tryx_panorama_manager experimental
flatpak install --user "$flatpak_work/tryx-panorama-manager-x86_64.flatpak"
flatpak run io.github.dxvsi.tryx_panorama_manager//experimental
```

After the GUI has started its runtime, the CLI uses the same sandbox namespace:

```fish
flatpak run --command=tryx io.github.dxvsi.tryx_panorama_manager//experimental status
```

The manifest runs `check-in-sdk.sh` before export: portal lifecycle/FD and startup
tests, a server-side firmware prohibition test, private FFmpeg artifact checks,
native protocol and Qt Quick regressions, strict translation compilation, CLI checks,
and an offscreen GUI smoke test. Tests use private D-Bus sessions and require USB
nodes to be absent. Run the builder as a regular user. CI changes to a dedicated
unprivileged builder before entering the SDK; checks reject root or effective
filesystem-permission bypass capabilities. The CLI's root-session-bus rejection
and write-denial fixtures remain unchanged. The test-only D-Bus tools are removed
from the final package.

Full C++/QML translation-source completeness and its negative tests run in the
native RPM/DEB/Arch CI for the same commit. The KDE SDK's `lupdate` is built
without QML parsing, so it cannot perform that source-extraction check.

`tests/flatpak/documentportal_probe.cpp` additionally provides an explicit
`--portal-fixture` integration probe. It grants only a newly-created synthetic
file and output folder, exercises the real file helpers, and revokes its own
document IDs on exit. It must not be confused with physical-device qualification.

CI produces a downloadable experimental artifact. Tagged releases call the same
workflow and include the versioned `.flatpak` alongside native packages in the
draft GitHub Release, with checksums and build-provenance attestations. The
Flatpak remains experimental; this does not publish it to Flathub or establish
hardware acceptance on additional systems.

The Flatpak CI container uses an Ubuntu 22.04 runner host because Ubuntu 24.04's
default AppArmor user-namespace restrictions prevent its non-root bubblewrap
network setup. The SDK remains KDE 6.11; no host security settings are disabled.
Native DEB packaging continues to use Ubuntu 24.04.
