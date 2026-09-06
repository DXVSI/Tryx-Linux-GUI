# TRYX Panorama Linux GUI

Linux-only Qt 6 Quick application for managing compatible TRYX cooler
displays, including Panorama models and the Turris 620.

The project ships one desktop GUI backed by a separate headless runtime. The
GUI owns presentation and user interaction; the runtime owns device discovery,
USB and serial/ADB communication, media operations, metrics delivery, and
firmware work. They communicate over the user D-Bus session.

Project home: [github.com/DXVSI/Tryx-Linux-GUI](https://github.com/DXVSI/Tryx-Linux-GUI)

## Support the Project

If TRYX Panorama Manager is useful to you, you can support continued development, protocol compatibility work, and testing on real hardware.

### USDT on TON

| Detail | Value |
|--------|-------|
| TON DNS | `fedora.ton` |
| Network | `TON Mainnet` |
| Token | `USD₮ (USDT Jetton)` |

**Wallet address**

`UQBO74LeYwNViA9MfdWPqfj4A5SkJ8vTcVG2uZzYzu9LFU-j`

> [!IMPORTANT]
> Send only USD₮ via TON Mainnet. Before confirming the transaction, verify that your wallet displays USD₮, not native TON. Do not use TRON, Ethereum, BNB Chain, or any other network.

## Supported and Planned Models

Only models marked **Tested on real hardware** are verified by the maintainer.
Community-tested entries may be supported, but have not been reproduced by
the maintainer; roadmap entries do not imply current compatibility.

| Product | Type | Display | Project status |
|---------|------|---------|----------------|
| [PANORAMA 240 / 280 / 360](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | Hardware needed; protocol unverified |
| [PANORAMA ARGB 240 / 280 / 360](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-argb/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | 360 ARGB community-tested; not maintainer-tested |
| [PANORAMA SE ARGB 360](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-se/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | Tested on real hardware |
| [PANORAMA SE ARGB 240](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-se/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | Hardware needed; protocol unverified |
| [PANORAMA WB](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-wb/black) | Custom-loop CPU water block | 6.5-inch curved AMOLED | Planned; hardware and protocol research required |
| [STAGE ARGB 360](https://www.tryx.com/en/products/liquid-cooling/stage/stage/white) | AIO liquid cooler | Dual 4.0-inch IPS, 720 × 720 each | Planned; hardware and protocol research required |
| [TURRIS 620](https://www.tryx.com/en/products/liquid-cooling/turris/turris-620/black) | Dual-tower air cooler | 5.0-inch IPS, 1280 × 720 | Community-tested upstream; not maintainer-tested |
| [HOLO ARGB 360](https://www.tryx.com/en/products/liquid-cooling/holo/holo/white-360) | AIO liquid cooler | Holographic display, 640 × 480 | Planned; hardware and protocol research required |
| [PANORAMA V2](https://www.tryx.com/en/about/news/tryx-computex-2026) | AIO liquid cooler | 2K curved AMOLED | Announced for Q3 2026; planned |
| [PANORAMA SE V2](https://www.tryx.com/en/about/news/tryx-computex-2026) | AIO liquid cooler | 2K curved AMOLED | Announced for Q3 2026; planned |

**Status definitions:**

- **Tested on real hardware** - verified by the maintainer using a physical device.
- **Community-tested** - reported working by an external user, but not reproduced by the maintainer.
- **Hardware needed** - a physical device is required before compatibility can be claimed.
- **Planned** - support is on the roadmap, but no current compatibility or implementation is implied.

Printer-class support is selected from the USB product ID rather than assuming
that every TRYX display has the same geometry or capabilities:

| USB identity | Product profile | Prepared media | Enabled capability set |
|--------------|-----------------|----------------|------------------------|
| `391a:1021` | Panorama SE / PASE | 2240 × 1080 | Current PASE printer-class features |
| `391a:1011` | Panorama | 2240 × 1080 | PASE media, display, and overlay features; firmware flashing disabled; community-tested |
| `391a:2011` | Turris 620 | 1280 × 720 | Acknowledged user-media upload only; no PASE bootstrap, Ping, catalog, display configuration, or firmware |

Turris support is based on the independently reported hardware results from
[MrEssentials/tryx-linux-display-manager](https://github.com/MrEssentials/tryx-linux-display-manager).
It has not been reproduced on maintainer-owned hardware. Metrics and overlay
layouts, split or waterfall modes, factory presets, and firmware operations
remain disabled for Turris through model capability gates.

<div align="center">

https://github.com/user-attachments/assets/f9baac04-fe28-4aeb-a8ea-eb2af37ff6cb

</div>

## Community Guides

- [Tryx Panorama 360 on Linux: What Actually Works (Hands-On, 2026)](https://pimpmycooler.com/en/guides/tryx-panorama-360-linux) - PimpMyCooler hands-on guide for using the DXVSI project on a retail Panorama 360 ARGB.

## What was done

- Analysis of KANALI resources to identify the device-side preset catalog without redistributing its extracted media
- Full protocol analysis to discover device commands for system metrics display
- Implemented working real-time CPU/GPU/Disk temperature monitoring on the cooler screen
- Built a complete Qt 6 Quick GUI from scratch (Dashboard, Display, and Settings pages)
- Auto-detection of CPU/GPU hardware names for badge display
- Auto-conversion of non-MP4 media formats (WebM, MKV, AVI, GIF) before upload to device
- Fixed serial communication issues (timeouts, wrong command formats, broken ADB quoting)
- Separated hardware ownership into a headless runtime behind the desktop interface

## Architecture

There is one supported desktop frontend: `tryx-panorama-manager`, implemented
with Qt Quick. It talks to `tryx-panorama-runtime` through the Manager1 and
Manager2 D-Bus interfaces. Closing or restarting the GUI does not transfer
hardware ownership away from the runtime.

Manager2 remains API generation 8 and negotiates additive runtime and
device-profile capabilities after the version probe. A current GUI keeps the
API 8 baseline when an older runtime does not implement capability methods;
unknown, malformed, stale-owner, or stale-device replies never enable optional
features.

When the runtime advertises presentation preferences, Settings can switch
between Celsius/Fahrenheit and 24-hour/12-hour time. The runtime persists the
confirmed pair and applies it consistently to the Dashboard and supported PASE
overlays even after the GUI closes. Older API 8 runtimes keep the compatible
Celsius and 24-hour defaults without exposing a writable setting.

On `391a:1021`, a capable runtime also enables **Automatic / Custom text** for
each enabled CPU/GPU badge in Panorama. Full uses one pair; Split keeps left and
right choices independent. Custom is trimmed plain text, 1-32 Unicode scalars
and at most 128 UTF-8 bytes, with a neutral background. Editing changes only the
draft; Apply sends the text together with the layout. Saved layouts and manual
Retry preserve the same choices. The accepted snapshot is host configuration,
not proof that the device font renders every glyph; device qualification is
still separate. Custom is not enabled on `1011`, TURRIS or legacy devices.
Recovered-media Replace is blocked while Custom is active because its old
contract cannot preserve text. Use Save as new, select the copy, then Apply.
New overlay/Saved/Retry formats keep private pre-upgrade configuration backups
and block incompatible downgrade; backups do not restore pruned media artifacts.

Settings can also save a local redacted support report as a JSON file selected
through a folder-only picker. The report contains an allowlisted host summary
and, when supported by the active runtime, a bounded cached runtime snapshot;
it does not query the device, read the journal or environment, or upload data.
The application generates a new private filename and never overwrites an
existing destination. Older compatible runtimes still produce an explicitly
marked host-only report.

Development builds are written to:

- `build/quick/tryx-panorama-manager` - desktop GUI
- `build/runtime/tryx-panorama-runtime` - headless runtime
- `build/cli/tryx` - one-shot terminal client

A system installation uses:

- `/usr/bin/tryx-panorama-manager` - public desktop launcher
- `/usr/bin/tryx` - public one-shot terminal client
- `/usr/lib/tryx-panorama-manager/tryx-panorama-runtime` - private runtime
  started by the user service or the GUI bootstrap

The `tryx` command is supported from a local terminal for the same Unix user as
the D-Bus session. Remote terminal qualification, including SSH and VS Code
Remote SSH, is deferred to the backlog. The command does not start the runtime,
forward the GUI, open a network listener, or connect to a remote D-Bus
endpoint. The inspection commands are read-only with respect to the runtime and
device; `support-report` only writes the explicitly requested local diagnostic
file. The two explicitly named downgrade commands below are the only commands
that change the runtime's local downgrade state:

```fish
tryx status
tryx capabilities
tryx operations
tryx support-report --output-dir /absolute/path
tryx --json status
tryx prepare-downgrade-v10
tryx abort-downgrade-v10
```

If the background runtime is intentionally stopped, start it separately with
`systemctl --user start tryx-panorama.service`. The runtime is shipped in the
same native package as the GUI and CLI. The `support-report` command can still
create a redacted host-only report with status `unavailable` when the user bus
or runtime is unavailable. An incompatible runtime, a legacy API 8 runtime, or
a runtime without the B3 support-snapshot capability instead produces a
host-only report with status `unsupported`; both host-only outcomes exit with
status 0. See `man tryx` for the JSON and exit-code contracts.

### Controlled runtime downgrade to v10

Do not replace a current runtime with the released v10 runtime unless
`tryx prepare-downgrade-v10` has completed successfully for the same Unix user
that owns the runtime. Preparation atomically blocks new runtime mutations and
accepts only an empty retry store or one exact terminal Full-frame retry. Split
media, an in-flight dispatch, pending cleanup or recovery, and an unverified
store all block the downgrade. Success is reported only after the exact D-Bus
owner has stopped.

This is not a general rollback of newer configuration formats. An overlay
store at version 3, a saved-layout store at version 2, or a retry carrying the
versioned badge Apply continuation blocks preparation, even if all badges are
Automatic. Switching Custom back to Automatic does not downgrade those files.
Do not delete newer state to bypass the guard. Restoring a pre-upgrade backup
requires a separately verified offline recovery plan; the private JSON backups
do not restore media artifacts that have since been removed.

The retry directory must also match an exact allowlist. Additional files,
including pre-upgrade configuration backups there, prevent preparation even
when no retry candidate remains. The marker protects the supported helper
workflow; it cannot prevent an arbitrary package-manager downgrade. The v10,
v11 and v12 labels in this section describe retry formats/implementation, not
application release numbers.

```fish
tryx prepare-downgrade-v10
```

After that command succeeds, replace the native package using the supported
package manager for the distribution, then start the user service or launch the
GUI:

```fish
systemctl --user start tryx-panorama.service
```

Preparation writes an owner-only marker under `XDG_STATE_HOME`, bound to the
full path, inode, size, and SHA-256 of the current runtime executable. The same
v11 executable cannot restart past that marker; the replaced v10 executable has
a different identity and ignores it. The marker does not install, remove, or
downgrade a package.

If package replacement is cancelled while the prepared v11 executable is still
installed, abort the preparation while the runtime remains offline, then start
it again:

```fish
tryx abort-downgrade-v10
systemctl --user start tryx-panorama.service
```

The abort command refuses an active runtime, a missing or unsafe marker, and a
different installed executable. Runtime sessions owned by different Unix users
have separate state and must be handled independently. An arbitrary package
downgrade without a successful preparation result is unsupported.

The Linux tray integration exports a StatusNotifierItem and DBusMenu over
D-Bus and sends notifications through `org.freedesktop.Notifications`. When a
StatusNotifier watcher and host are available, Settings lets the user choose
whether closing the window hides the GUI to the native desktop tray or quits
the GUI. The hide option is unavailable without a host, so closing the window
then quits only the GUI. In either mode, the separate runtime remains available
to the user service. A separate Settings switch can create an owner-managed
XDG Autostart entry for the GUI. Login start hides the initial window only when
Hide to tray is selected and a tray host is actually available; otherwise the
window is shown. This switch never changes the background runtime service.

## What's new in 2.3.0

- Independent **Automatic / Custom text** choices for each CPU/GPU badge on
  capable `391a:1021` displays, including separate left/right choices in Split.
- Saved layouts with explicit Load and Apply, independent left/right overlay
  styling, and grouped metric search with up to three metrics per area.
- Explicit Full-screen / Split-area media preparation and stricter Replace
  checks that preserve the other display area and reject stale layouts.
- Confirmed device specifications, Celsius/Fahrenheit and 12/24-hour settings,
  optional NVIDIA telemetry, and independent GUI autostart and close behavior.
- Local redacted support reports, safe unused temporary-file cleanup, and the
  one-shot `tryx` command-line client for local session inspection.
- Quiet startup/hourly release notifications with a link to GitHub. There is
  no automatic download, installation, firmware update, or opt-in setting.
- Runtime/session/protocol ownership refactoring with the existing API 8
  contracts and model gates preserved. The GUI now recovers when connection
  revisions advance between cached reads instead of leaving controls disabled.
- Qt 6.4-compatible deferred UI actions and layout hints that wrap within their
  panels even with unusually wide font metrics.

Back up the user profile before upgrading. New overlay, saved-layout, and retry
formats are not a general round-trip to older packages; switching badges back
to Automatic does not downgrade the stored format. See the
[downgrade limitations](#controlled-runtime-downgrade-to-v10).

Custom text remains limited to `1021`. Device font coverage is not guaranteed;
Replace is unavailable while Custom is active, so use Save as new and then Apply.
No additional model or NVIDIA hardware qualification is claimed by this release.

## What's new in 2.2.0

- Added community-tested printer-class support for Panorama devices with USB
  ID `391a:1011`, including 2240 × 1080 media, display configuration, and
  overlay metrics.
- Added community-tested Turris 620 support for USB ID `391a:2011`. Images,
  GIFs, and videos are prepared at the native 1280 × 720 resolution, wrapped
  as MXHD media, uploaded with exact acknowledgements, and activated
  immediately.
- The editor and preview now follow the connected model's native geometry and
  regenerate stale previews when the target profile changes.
- Device capabilities are selected from the exact USB product profile, so
  unsupported catalog, display, overlay, and firmware operations are rejected
  before USB traffic starts.
- Session recovery, prepared-media retries, and upload reconciliation are
  bound to the exact USB product, preventing state from one model from being
  reused on another model at the same path.
- Firmware safety checks keep the identified legacy `cm01` serial/ADB path and
  supported PASE path available while rejecting Panorama `391a:1011`, Turris
  `391a:2011`, and unidentified targets.
- Updated udev rules, package descriptions, AppStream metadata, user-facing
  text, and Russian translations for all supported printer-class IDs.

Turris support in this release is intentionally limited to acknowledged media
upload with immediate activation. Device catalog, export, Save as new,
Replace, delete, Apply, display configuration, brightness, metrics, presets,
keepalive, and firmware operations remain disabled. Panorama `391a:1011` and
Turris `391a:2011` have not been reproduced on maintainer-owned hardware.

## What's new in 2.1.1

- Launching the GUI directly from `build/quick` now selects the sibling API 8
  runtime from the same build instead of starting an older installed runtime.
- Bootstrap checks the systemd unit PID, state, and queued job before starting
  a development runtime, and fails closed if the D-Bus owner changes or its API
  cannot be verified.
- An incompatible installed runtime is never stopped automatically after a
  non-atomic state probe. The GUI provides an explicit safe restart command.
- Isolated D-Bus regression tests cover API 6/API 8 selection, systemd startup
  races, delayed D-Bus readiness, owner replacement, and exact-PID cleanup.

## What's new in 2.1.0

- The desktop application is now one Qt Quick GUI with no Qt Widgets runtime
  dependency. Hardware access remains in the separately packaged private
  runtime.
- The Linux tray uses StatusNotifierItem and DBusMenu, while desktop
  notifications use `org.freedesktop.Notifications`. Its explicit Quit action
  closes the GUI reliably while leaving the separate runtime active.
- Legacy serial/ADB devices retain display, media, metrics, keepalive, and
  device-control support through the same Quick interface.
- PASE user media can be edited with Fit, Fill, Crop, Stretch, zoom, pan, and
  rotation, exported as its exact raw H264 device copy, saved as a new item,
  replaced through a crash-safe verified workflow, or deleted when eligible.
- Quick Settings provides local firmware package selection and validation.
  The runtime obtains an exclusive device-transport gate before handing work
  to the existing updater backend and writes an owner-only recovery interlock
  before dispatch. A daemon restart cannot silently reconnect after an
  interrupted or completed flash. The user must inspect the display and
  explicitly acknowledge recovery before the normal device session resumes.
  The gate, journal, and validation paths are covered by offline tests; this
  release preparation did not physically flash a device and does not claim
  that hardware operation as verified.
- The QML application and package checks remain compatible with Qt 6.4 for
  Ubuntu 24.04 and Linux Mint 22. Atomic file writes explicitly check buffered
  flush errors before commit to preserve the previous file on older Qt builds
  affected by [QTBUG-132332](https://github.com/qt/qtbase/commit/92373d353cf090faa03cbc8aca505d1784b10b54).
- The protocol implementation uses project-owned clean-room schemas. Release
  packages contain no extracted KANALI schemas, vendor firmware, or bundled
  media.

## Features

- Upload images, videos, GIFs (auto-converts non-MP4 formats)
- Modern desktop interface with a preview-first, model-aware media editor
- Explicit Fit, Fill, Crop, Stretch, Zoom, pan, rotation, and Fit background controls before upload
- Exact transformed preview rendered through the same canonical FFmpeg filter used for the final model-specific media: full 2240 × 1080 or split-area 1120 × 1080 for Panorama/PASE, and full 1280 × 720 for Turris
- Immutable private upload snapshot with atomic client-to-runtime ownership transfer before D-Bus acceptance
- Origin-aware PASE media catalog that labels device presets separately from user uploads
- Export of writable PASE user media as an honest raw H264 device copy
- Edit of an existing PASE user-media copy with Save as new or crash-safe Replace
- Real-time system metrics on display (temperature, usage, frequency, power and date/time)
- Optional bounded `/usr/bin/nvidia-smi` telemetry for NVIDIA temperature,
  usage, graphics clock, power, and VRAM. A missing tool, driver, GPU, or field
  is shown as unavailable rather than zero; VRAM remains local to the host
  Dashboard, the four existing device GPU tokens and Manager2 API 8 are
  unchanged, and real NVIDIA hardware qualification is still pending.
- Hardware name badges, with optional custom text on capable `391a:1021` runtimes
- Brightness control (0-100) on capable Panorama/PASE profiles
- Display settings on capable Panorama/PASE profiles: position, alignment, color, filter
- Runtime-owned keepalive for capable Panorama/PASE display sessions
- Auto-detects legacy devices through `/dev/ttyACM*` and supported printer-class devices through direct libusb discovery
- Native Linux StatusNotifierItem tray integration with DBusMenu and desktop notifications when a watcher is available
- Optional, independent XDG Autostart for the desktop GUI with safe tray fallback
- Settings persistence between sessions
- Runtime-owned Celsius/Fahrenheit and 24-hour/12-hour presentation preferences
- About shortcuts to the project home and its published MIT license
- Quiet startup/hourly checks for new stable application releases, with a
  desktop notification and a release link in Settings/About
- Async device communication (non-blocking GUI)
- Quick Settings firmware panel for locally selected packages, with validation and hardware work owned by the headless runtime
- Device information and media list over the new KANALI USB printer-class protocol
- Direct asynchronous libusb transport with one request-scoped IN armed before OUT and bounded response reads after known OUT completion
- Exact operation IDs, progress, cancellation, verified completion, and manual retry through D-Bus Manager2
- Backward-compatible media catalog through D-Bus Manager1 and an enhanced origin-aware catalog through Manager2 API version 8
- Content-aware Save that reuses a verified PASE copy instead of uploading the same local media again
- Verified deletion of one eligible user media file at a time, with crash-safe reconciliation and no automatic FileRemove replay
- One shared Panorama operation banner with progress, cancellation, and one fail-closed manual retry candidate
- Runtime-owned PASE metric configuration and one-second sampling that continue after the GUI closes
- Runtime API compatibility check that prevents the GUI from silently using an outdated background runtime

## Application release notifications

While the primary desktop GUI is running, it checks the public GitHub latest
release endpoint once after startup and then every 60 minutes, including when
hidden in the tray or started at sign-in. A secondary launch does not trigger
another check. Closing the GUI completely stops checking; the background runtime,
CLI, firmware tools, export helpers and offline smoke tests never poll releases.

Only a strictly newer stable `X.Y.Z` or `vX.Y.Z` tag produces a desktop
notification and a version/link row in Settings/About. Each version is announced
at most once per GUI process; a later, higher version can be announced again.
Desktop notification settings, Do Not Disturb or an unavailable notification
service may hide the popup; the Settings link remains available. Network errors,
timeouts, rate limits and invalid responses are silent and preserve the last
confirmed update. A successful equal/older release response clears the row.

The GUI sends an unauthenticated HTTPS request to
`https://api.github.com/repos/DXVSI/Tryx-Linux-GUI/releases/latest`, with a fixed
application User-Agent and no GitHub token, cookies, device identifiers, logs or
media metadata. GitHub receives the normal connection/HTTP information, including
your public IP address. Each attempt has a 10-second deadline and a 1 MiB response
limit; redirects are not followed. Server rate-limit hints can postpone the next
hourly attempt. Validated release data and ETags stay in process memory only.
There is no opt-in setting, manual check button or persistent network state.

Choose **Open release** in Settings/About to open the validated project release
page in your system browser, then update manually using the appropriate package.
Nothing is downloaded, installed or restarted automatically. These notifications
do not check or update device firmware.

## Media-free distribution

The application does not bundle, install, or search for the extracted KANALI video library. Factory media already stored on PASE appears in the unified Media Library as a read-only `DEVICE PRESET`; it can be selected and applied but never deleted by the application. User uploads remain labelled separately. A user file with the same name as a known device file is still treated according to its catalog origin and is never promoted to a preset by filename.

Manual user upload remains available. Thumbnails are generated from the user-selected source and shown only after the runtime has confirmed the uploaded origin in its device-scoped XDG media catalog. Legacy files left by an older installation under `/usr/share/tryx-panorama-manager/media` are ignored by the current runtime and are not deleted automatically.

Writable user media already stored on a PASE device has an action menu in the
Media Library. `Export copy…` saves the exact prepared device stream as
`.h264`; it cannot reconstruct the original MP4, GIF, image, audio, or file
name. `Edit` downloads the same private copy into the background runtime,
opens it in the Fit, Fill, Crop and Stretch editor, and offers two explicit
results:

Before sizing controls, `Prepare for` selects `Full screen` or `Split area`.
The Split option appears only after the runtime and connected device advertise
the additive capability, uses a real 1120 × 1080 canvas, and prepares one copy
that can later be selected for either side. Changing the target resets the
editing transform to neutral Fit and never applies a display change by itself.

- `Save as new` uploads a verified new media file and always keeps the
  original.
- `Replace original` uploads and verifies the new file first, updates only
  a fresh active Full or Split layout matching the selected target, and only
  then removes the original once. The read-only preflight compares the complete
  `screenMode`, `playMode`, ordered media list, and the original's exact
  Full/Left/Right slot. Immediately before FileRemove, the runtime rechecks both
  the original and the verified replacement in the same fresh FileList by exact
  name, size, user source, and writable flag.

Factory presets, read-only entries, and unsupported device media do not expose
Export, Edit, or Delete. An interrupted Replace is reconciled from its
owner-only journal without automatically repeating an uncertain Apply or
Delete command. Reconciliation becomes terminal only when a fresh FileList
also proves that the exact verified replacement copy still exists.

## Native Linux packages

TRYX Panorama Manager supports Linux only. Native packaging targets Fedora 43
and 44, Ubuntu 24.04, Linux Mint 22, and current Arch Linux. A binary package
built for one distribution is not reused on another distribution.

Release assets use these formats:

- RPM `x86_64` for Fedora 43 and Fedora 44
- DEB `amd64` for Ubuntu 24.04 and Linux Mint 22
- `.pkg.tar.zst` `x86_64` for current Arch Linux
- `SHA256SUMS` for artifact verification

The commands below apply after the corresponding files have been published
on the GitHub Releases page. Until then, use the source build instructions
below.

Install a downloaded package with the package manager for your distribution:

```fish
# Fedora. Enable RPM Fusion Free first because media conversion requires the
# full ffmpeg package with the libx264 encoder.
sudo dnf install --allowerasing ./tryx-panorama-manager-2.3.0-1.fc44.x86_64.rpm

# Ubuntu 24.04 or Linux Mint 22
sudo apt install ./tryx-panorama-manager_2.3.0-1_amd64.deb

# Arch Linux
sudo pacman -U ./tryx-panorama-manager-2.3.0-1-x86_64.pkg.tar.zst
```

These commands use the distribution package manager to resolve and download
all mandatory runtime dependencies. Qt 6, Protobuf, libusb, and the other
required shared libraries do not need to be installed manually. Use the
commands above instead of `rpm -i` or `dpkg -i`, because those tools do not
download missing dependencies.

Optional helpers such as ADB, `unzip`, `debugfs`, `glxinfo`, `lspci`, and
`/usr/bin/nvidia-smi` may not be installed automatically. They are not required
for basic printer-class
operation and are only used by the corresponding legacy firmware, archive
inspection, hardware detection, or NVIDIA telemetry features. The application
does not install or recommend a proprietary NVIDIA driver package.

For Fedora, follow the
[RPM Fusion configuration instructions](https://rpmfusion.org/Configuration)
before installing the RPM. Fedora's `ffmpeg-free` can provide an `ffmpeg`
executable without the `libx264` encoder required by device media preparation.
Use `--allowerasing` when installing the RPM so DNF can replace an existing
`ffmpeg-free` package with RPM Fusion's full `ffmpeg` build.

Native packages install the Qt Quick GUI at
`/usr/bin/tryx-panorama-manager`, the one-shot terminal client at
`/usr/bin/tryx`, its `tryx(1)` manual, the private background runtime at
`/usr/lib/tryx-panorama-manager/tryx-panorama-runtime`, the desktop entry, icon,
systemd user unit, and two TRYX printer-class udev rules. The native package
name remains `tryx-panorama-manager`. Packages do not enable the runtime
service or GUI autostart, and do not restart an existing runtime during an
upgrade. Reconnect the supported TRYX display USB cable after installation,
launch the application once, and enable GUI autostart in Settings only if
wanted.

The committed Arch PKGBUILD intentionally accepts only a local release source
archive with an explicit checksum. From a clean release checkout, build it
with:

```fish
mkdir -p dist/source
packaging/scripts/create-source-archive.sh dist/source
set version (string trim < VERSION)
set archive packaging/arch/tryx-panorama-manager-$version.tar.xz
cp dist/source/tryx-panorama-manager-$version.tar.xz $archive
set -lx TRYX_LOCAL_SOURCE_SHA256 (sha256sum $archive | cut -d ' ' -f 1)
pushd packaging/arch
makepkg --cleanbuild --check
popd
```

## Requirements

**Build:**
- Linux
- Qt 6.4 or newer (Concurrent, Core, D-Bus, Gui, QML, Quick, Quick Controls 2)
- C++17 compiler
- qmake6
- Qt6 translation tools with `lrelease`
- protoc and the matching full C++ protobuf development runtime
- libudev development files
- libusb 1.0 development files
- systemd development metadata

Fedora build dependencies:

Enable RPM Fusion Free first, then install the full `ffmpeg` build so package
tests exercise the same `libx264` encoder required at runtime:

```fish
sudo dnf install -y --allowerasing gcc-c++ git make dbus-daemon ffmpeg pkgconf-pkg-config qt6-qtbase-devel qt6-qtdeclarative-devel qt6-linguist protobuf-compiler protobuf-devel systemd-devel libusb1-devel
```

Ubuntu 24.04 and Linux Mint 22 build dependencies:

```fish
sudo apt install build-essential dbus-user-session ffmpeg git libprotobuf-dev libsystemd-dev libudev-dev libusb-1.0-0-dev pkg-config protobuf-compiler qmake6 qt6-base-dev qt6-declarative-dev qt6-declarative-dev-tools qt6-l10n-tools qml6-module-qt-labs-folderlistmodel qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs qml6-module-qtquick-layouts qml6-module-qtquick-shapes qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qttest systemd-dev
```

Arch Linux build dependencies:

```fish
sudo pacman -S --needed base-devel dbus git libusb protobuf qt6-base qt6-declarative qt6-tools systemd
```

The qmake guard requires the protobuf compiler and C++ runtime to be from the
same version. It treats an omitted trailing zero as formatting only, so
`protoc 35.1` matches `libprotobuf 35.1.0`, while a real patch mismatch is
still rejected.

**Runtime:**
- `ffmpeg` with the `libx264` encoder - media conversion
- `adb` (android-tools) - legacy OTA file transfer and optional Rockchip reboot-to-loader
- `unzip` - firmware package validation and extraction
- `debugfs` (e2fsprogs) - Rockchip rootfs inspection
- `upgrade_tool` - optional external Rockchip flashing backend for new KANALI firmware bundles
- `glxinfo` (mesa-utils) - GPU name detection (optional)
- `/usr/bin/nvidia-smi` - NVIDIA telemetry (optional, supplied by the NVIDIA
  driver; intentionally not a package dependency or recommendation)

Fedora runtime dependencies:

```fish
sudo dnf install -y android-tools unzip e2fsprogs ffmpeg mesa-demos
```

**Permissions:**
- User must be in `dialout` group (or `uucp` on Arch) for serial access
- Supported printer-class devices use `391a:1011` for Panorama, `391a:1021` for Panorama SE / PASE, and `391a:2011` for Turris 620; direct libusb access uses `/dev/bus/usb/*/*` and requires the `lp` group or a seat ACL from `TAG+="uaccess"`
- Fedora's generic printer rule must not start CUPS `configure-printer` for this vendor protocol. The qmake install target places an early access rule and a late printer-suppression rule in `/usr/lib/udev/rules.d`; do not create same-named overrides in `/etc/udev/rules.d`, because they would shadow packaged updates.

## Firmware Updates

Firmware updates are initiated from the firmware panel in Quick Settings, but
package validation and hardware access belong to the headless runtime. The
panel accepts a locally selected ZIP; it does not download firmware
automatically. Availability depends on the package type, connected device
state, and required external tools. Validation is not a claim that an
arbitrary package is safe for a different model.

The local validator recognizes two Panorama SE package formats:

- Legacy Android OTA `update.zip` for `cm01_se` devices. The runtime validates `META-INF/com/android/metadata`, copies an approved package to `/sdcard/update.zip` over ADB, verifies the copied size and SHA-256, and requests recovery reboot.
- New KANALI Rockchip loader ZIP bundles for `PASE`. The runtime validates the required Rockchip files, checks `parameter.txt` for `RK3568`, and inspects `rootfs:/usr/bin/panorama` for the product marker. It can invoke an external Rockchip `upgrade_tool` only when the backend and device-state checks pass. If an identified firmware-capable device is present over ADB or the `391a:1021` printer interface, the authorized update may reboot it into Loader and continue there. A cold, unidentified Loader device and Maskrom mode are rejected because their original product profile cannot be proven.

The `upgrade_tool` executable is not bundled in this open source repository because its redistribution rights are not clear. The app looks for it in `TRYX_UPGRADE_TOOL`, `PATH`, next to the app binary, `tools/upgrade_tool`, and `~/.local/bin/upgrade_tool`.

Rockchip RK3568 loader access may require a local udev rule for USB VID/PID `2207:350a` so the flashing backend can reset or inspect the device without root.

Before dispatching an approved package, the runtime atomically writes an
owner-only recovery journal and keeps the device transport under an exclusive
firmware gate. The journal survives daemon crashes, forced termination, and
successful updater completion. While it exists, startup is fail-closed: the
runtime does not automatically open a normal display session. After the
updater finishes, wait for the cooler to boot, inspect the physical display,
then use **I inspected the display; resume connection** in Quick Settings.
That explicit action removes the exact journal entry, releases the gate, and
starts a fresh connection. It is not an automated firmware-version or boot
verification. A new flash cannot be started from an unidentified Loader-only
device; the runtime must first identify a firmware-capable TRYX product before
authorizing the transition into Loader mode.

After updating to the new KANALI firmware, the cooler no longer exposes ADB by default. It appears as `391a:1021 RK PASE` with a bidirectional printer interface. The app generates C++ types from three minimal, project-owned schemas under `protocol/wire-v1`; recovered vendor descriptor sources are not a build or release dependency. The production path does not read or write `/dev/usb/lp*`: it claims the `07/01/02` interface through usbfs, temporarily detaches `usblp`, arms one bulk IN before each request, never re-arms that endpoint while the matching bulk OUT is still active, drains optional periodic responses to a complete frame boundary after OUT, and releases the interface on shutdown.

Turris 620 exposes the supported `391a:2011` printer-class identity. Its
community-tested contract is deliberately narrower than PASE: media is prepared
at `1280x720`, wrapped as MXHD, and sent only through the acknowledged
FileTransmit sequence `400` (begin), `401` (data), and `402` (end), with exact
successful responses `800`, `801`, and `802`. The final `802` acknowledgement is
the success and activation boundary. Turris does not use the PASE bootstrap,
Ping or display keepalive, FileList/media-catalog queries, user/display
configuration, overlay metrics, presets, deletion, or firmware operations.

All printer operations are serialized by one worker-owned session, while cancellable ffmpeg conversion runs outside the USB worker. Passive udev discovery recognizes the `391a:0006 rk3xxx` Rockchip gadget identity but never opens it. Discovery is based on physical USB device events and stable bus/port identity, so the app does not mistake its own `usblp` detach or attach for a physical reconnect. Printer Class `GET_PORT_STATUS` is deliberately not used because PASE does not provide a reliable readiness signal through that request. A physical remove/add creates a new connection generation, interrupts old I/O through its cancellation gate, and discards stale results. On Panorama/PASE profiles, recovery confirms protocol readiness through an exact DeviceInfo response, completes the remaining bootstrap once, sends one post-bootstrap Ping, restores the confirmed overlay at most once, and only then starts metrics. It never retries a complete bootstrap in the same physical generation or automatically replays user configuration, upload, delete, or apply mutations. Turris bypasses this PASE readiness/bootstrap path and opens its bounded transfer path only for an explicit user upload.

The Panorama/PASE readiness phase has a 20-second monotonic deadline. It retries only a DeviceInfo request whose USB OUT is confirmed to have transferred zero bytes, keeping the same claimed handle and using capped `500`, `1000`, then `2000` millisecond backoff. A partial or unknown OUT, cancellation, malformed response, or a complete OUT without the exact DeviceInfo response is terminal for that physical generation. System configuration and authentication queries are each sent at most once after readiness. Panorama/PASE keepalive uses the observed untracked Ping frame and drains an optional asynchronous Pong. Metrics sampling and mutations start only after the post-bootstrap barrier. Manual upload converts media to the active product profile and gives data chunks a dedicated 15-second OUT deadline. Panorama/PASE verifies the exact new name, prepared size, writable flag, and user source through a fresh media catalog before reporting success or applying it. Turris requires an exact successful response to every MXHD-wrapped FileTransmit stage, and the successful `802` response completes and activates the upload without a subsequent FileList or Apply request. Save and origin reuse are catalog-backed Panorama/PASE workflows and are unavailable for Turris. No completed IN transfer is re-armed while any OUT remains active, preventing queued response fragments or `EPROTO` completions from starving the writer. Periodic Panorama/PASE write-only commands perform a bounded post-OUT drain; no response is acceptable, but a partial or malformed frame closes the session fail-closed. A persistent bulk-IN failure latches the current USB endpoint generation as lost. Production does not call `libusb_reset_device`, retry the same generation, or replay its last mutation; recovery requires an observed physical remove/add cycle or a full printer-class device power cycle that creates a new generation. Conversion and preview subprocesses have bounded deadlines; a preview timeout falls back to an honest placeholder without discarding valid H264. The direct USB reader can recover a complete tracked protobuf when faulty PASE firmware drops only the `TRYX` frame header after an IN transport error; recovery still requires the exact transaction ID and expected response body. Manager2 API version 8 adds FilePull-backed trusted device-media artifacts with owner-bound leases and crash-safe Save as new or Replace operations while preserving the API 7 media-transform and upload semantics. It also exposes stable UUIDs, structured operation states, origin-aware catalog entries, typed display mutations, confirmed display state, per-side overlay configuration, and explicit backlight power control. Manager1 retains its original catalog tuple for ABI compatibility. A verified prepared file and its staged JPEG preview are cached atomically after a failed transfer and can only be retried manually after prepared-file hash and exact product and device-generation checks; catalog-capable Panorama/PASE profiles additionally require media-catalog validation. The original source file is not required after conversion. If a data transfer ends partially or with an unknown outcome, its recovery requirement remains sticky across retries and daemon restarts. Mutations supported by the active product profile remain blocked until the runtime observes removal and reconnection of the same printer-class product, because closing libusb or issuing a generic USB reset does not prove that firmware discarded its hidden transfer session. A successful Panorama/PASE verification promotes the preview and content identity into the XDG media catalog. Apply is not atomic: uncertain writes are reported as partial or unknown, the session is closed, and no automatic rollback or replay is attempted.

PASE full-screen mode supports up to three exact protocol metrics selected from CPU temperature, frequency, usage and power; GPU temperature, frequency, usage and power; memory frequency and usage; and date/time. A separate Manager2 operation sends the overlay layout, then the background daemon sends live values through a headerless metric batch every second. The two-second background scheduler supports two measured arms through `pase_overlay_lease_mode` in the existing XDG `config.json`: `ping-and-overlay-lease` alternates Ping with a full overlay lease, while `ping-only` sends only Ping after the initial reconnect overlay restoration. The default preserves the current `ping-and-overlay-lease` behavior until the A/B monitor selects an arm. The lease never writes user configuration or media state. An explicit protocol error from either metric update or layout lease is fail-closed instead of being discarded. Metric sampling pauses during upload or Apply and coalesces to the latest sample, while a delayed tracked response can still receive one bounded liveness command without replaying the mutation. The confirmed layout is stored only for the same non-empty device serial and survives GUI or daemon restarts. Missing sensors remain unavailable instead of being reported as zero. The Memory Frequency protocol label is retained for compatibility, but the current Linux runtime reports it as unavailable because upstream Linux does not expose a portable unprivileged source for the live DRAM clock; static SMBIOS transfer rates are not mislabeled as MHz.

PASE media deletion is limited to one exact user-owned, writable, unreferenced catalog entry per operation. The runtime persists a delete-intent journal before sending a single USB media-removal request and reports success only after a fresh media catalog no longer contains the exact name. A lost or ambiguous response enters read-only reconciliation; media removal is never replayed automatically.

PASE display configuration uses one read-modify-write user-configuration update, one complete overlay-layout update, and a bounded configuration readback. The UI supports brightness, display backlight power, Mirror, Waterfall, Full Screen, and Screen Splitting with two existing media files. Firmware-controlled standby enablement and standby media remain read-only and are never rewritten by the display power control. Rapid brightness input keeps at most one active operation and one latest pending value; the pending value is dispatched only after the prior operation, exact readback, and a subsequent background keepalive all succeed. Each screen area can contain up to three metrics plus CPU and GPU badges with exact `#RRGGBB` text colors. The grouped metric selector keeps Full, Left, and Right counters independent, requires an explicit position when replacing a fourth choice, and keeps a selected unavailable sensor removable without silently rewriting the draft. Mirror uses `media_rotation=180`, Waterfall uses `ui_rotation=90`, and split mode uses the dual-media wire mode with independent left and right media. A display operation succeeds only when the requested fields match the fresh device readback. A matching explicit protocol error from the optional overlay response is treated as a logical rejection and is never discarded as stale. Direct printer-protocol firmware writes and loader reboot remain intentionally disabled.

Automatic firmware download remains disabled. The September 6, 2026
[B6 source review](docs/2026-08-09-runtime-decomposition-and-feature-roadmap.md#b6-remote-firmware-availability-research)
found official download and offline-upgrade pages, but did not establish a
supported firmware manifest, publisher verification, exact hardware
compatibility or power-loss recovery contract for the target models. KANALI
installer and upgrade-tool version labels are not verified firmware revisions.
Earlier research observed SM2-protected metadata/download-URL requests in a
KANALI wrapper; this does not establish the current public distribution
contract or authenticate the firmware itself. Local ZIP checks and SHA-256
pin approved bytes, not vendor provenance; the recovery journal is not rollback.

## Build

```fish
git clone --branch production https://github.com/DXVSI/Tryx-Linux-GUI.git; and cd Tryx-Linux-GUI
qmake6 tryx-panorama-all.pro
make
dbus-run-session -- make package-check
```

For a development run, launch the build-tree GUI. If no compatible runtime
already owns the D-Bus service, the GUI first verifies that the installed
systemd unit has no process or queued job, then starts the sibling runtime from
`build/runtime`. It does not start the installed service first:

```fish
./build/quick/tryx-panorama-manager
```

If an incompatible installed runtime already owns the D-Bus service, first
finish or cancel its active operation, stop that service, and retry the GUI:

```fish
systemctl --user stop tryx-panorama.service
./build/quick/tryx-panorama-manager
```

To keep runtime logs in a dedicated terminal, stop the installed service and
run `./build/runtime/tryx-panorama-runtime` there before launching the GUI.

If the installed service was masked while diagnosing an older runtime, unmask
it before returning to the installed GUI:

```fish
systemctl --user unmask tryx-panorama.service; and systemctl --user daemon-reload
```

System installation includes the public GUI and CLI, private runtime, user
service, TRYX printer-class usbfs rules, desktop entry, icon, manuals, and
translations. It does not install a video library:

```fish
sudo make install; and sudo udevadm control --reload-rules; and sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=391a; and sudo udevadm settle --timeout=10
systemctl --user daemon-reload; and systemctl --user start tryx-panorama.service; and systemctl --user is-active tryx-panorama.service
```

The command above is the first-install path. When updating an existing manual
source installation, first finish or cancel every active media operation, then
install the new files and restart the runtime explicitly:

```fish
sudo make install; and systemctl --user daemon-reload; and systemctl --user restart tryx-panorama.service; and systemctl --user is-active tryx-panorama.service
```

Native package upgrades deliberately do not force this restart because a
package transaction cannot prove that another user's media operation is idle.

The install target supplies a user preset that keeps the background runtime
service disabled by default. Enable that service explicitly with
`systemctl --user enable tryx-panorama.service` only when it must start at
session login without the GUI. An on-demand runtime started by the GUI can stay
active even while the unit remains disabled. The separate Settings switch
controls only GUI login startup through
`$XDG_CONFIG_HOME/autostart/tryx-panorama-manager.desktop` and never changes
the runtime unit.

The GUI, CLI, and runtime version commands are safe to use without a graphical
or D-Bus session:

```fish
./build/quick/tryx-panorama-manager --version
./build/cli/tryx --version
./build/runtime/tryx-panorama-runtime --version
```

Offline printer-protocol tests do not access physical USB hardware:

```fish
cd tests; and qmake6 printerprotocol_tests.pro; and make -j(nproc); and ../build/tests/printerprotocol-tests
```

## Project Structure

```
src/
  cli/               # One-shot local user-session client
  core/              # Legacy serial/ADB protocol and shared configuration
  quick/             # The Qt Quick GUI, D-Bus client, tray, and controllers
  runtime/           # Headless runtime entry point
  devicemanager.*    # Runtime-owned async device and operation coordination
  firmwarebridge.*   # Runtime-side firmware D-Bus boundary
  firmwareupdater.*  # Local firmware validation and external-tool execution
  mediatransform.*   # Canonical media transform validation and FFmpeg filter
  printermediapreparer.* # Async media preparation, cancellation, and retry validation
  runtimecontract.*  # Shared Manager1/Manager2 D-Bus data contract
  runtimebridge.*    # Exported runtime D-Bus adaptors
  printerprotocol.*  # PASE framing, direct libusb transport and udev discovery
  systemmonitor.*    # System metrics reader
qml/
  Main.qml           # Single desktop shell
  pages/             # Dashboard, Display, and Settings
  components/        # Media editor, firmware panel, and shared controls
resources/           # GUI resource collection and application icon
translations/        # Qt Linguist translation sources
include/panorama/    # Protocol headers
protocol/wire-v1/    # Minimal project-owned protobuf wire schema
tests/               # Offline runtime, protocol, transport, and Quick tests
debian/              # Ubuntu 24.04 and Linux Mint 22 package metadata
packaging/
  arch/              # Arch Linux PKGBUILD
  rpm/               # Fedora RPM spec
  metainfo/          # AppStream metadata
  scripts/           # Release and package-content gates
  *.rules            # PASE permissions and printer suppression
tryx-panorama-all.pro   # Aggregate runtime + CLI + GUI build and package-check
tryx-cli.pro            # Headless terminal client qmake project
tryx-panorama.pro       # Headless runtime qmake project
```

## Tested on

| Distro | Kernel | CPU | GPU1 | GPU2 |
|--------|--------|-----|------|------|
| Fedora 44 | 7.1.3-200.fc44.x86_64 | AMD Ryzen 9 9950X3D | AMD Radeon RX 7900 XTX | AMD Radeon RX 7900 XTX |

## License

MIT. See [LICENSE](LICENSE).
