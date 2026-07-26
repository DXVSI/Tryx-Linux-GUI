# TRYX Panorama Linux GUI

Qt6 GUI application for managing TRYX Panorama AIO cooler displays on Linux.

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

Only models marked **Tested on real hardware** are verified by the maintainer. Other entries are community reports or roadmap targets and must not be treated as currently compatible.

| Product | Type | Display | Project status |
|---------|------|---------|----------------|
| [PANORAMA 240 / 280 / 360](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | Hardware needed; protocol unverified |
| [PANORAMA ARGB 240 / 280 / 360](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-argb/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | 360 ARGB community-tested; not maintainer-tested |
| [PANORAMA SE ARGB 360](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-se/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | Tested on real hardware |
| [PANORAMA SE ARGB 240](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-se/black-360) | AIO liquid cooler | 6.67-inch curved AMOLED, 2240 × 1080 | Hardware needed; protocol unverified |
| [PANORAMA WB](https://www.tryx.com/en/products/liquid-cooling/panorama/panorama-wb/black) | Custom-loop CPU water block | 6.5-inch curved AMOLED | Planned; hardware and protocol research required |
| [STAGE ARGB 360](https://www.tryx.com/en/products/liquid-cooling/stage/stage/white) | AIO liquid cooler | Dual 4.0-inch IPS, 720 × 720 each | Planned; hardware and protocol research required |
| [TURRIS 620](https://www.tryx.com/en/products/liquid-cooling/turris/turris-620/black) | Dual-tower air cooler | 5.0-inch IPS, 1280 × 720 | Planned; hardware and protocol research required |
| [HOLO ARGB 360](https://www.tryx.com/en/products/liquid-cooling/holo/holo/white-360) | AIO liquid cooler | Holographic display, 640 × 480 | Planned; hardware and protocol research required |
| [PANORAMA V2](https://www.tryx.com/en/about/news/tryx-computex-2026) | AIO liquid cooler | 2K curved AMOLED | Announced for Q3 2026; planned |
| [PANORAMA SE V2](https://www.tryx.com/en/about/news/tryx-computex-2026) | AIO liquid cooler | 2K curved AMOLED | Announced for Q3 2026; planned |

**Status definitions:**

- **Tested on real hardware** - verified by the maintainer using a physical device.
- **Community-tested** - reported working by an external user, but not reproduced by the maintainer.
- **Hardware needed** - a physical device is required before compatibility can be claimed.
- **Planned** - support is on the roadmap, but no current compatibility or implementation is implied.

<div align="center">

https://github.com/user-attachments/assets/f9baac04-fe28-4aeb-a8ea-eb2af37ff6cb

</div>

## Community Guides

- [Tryx Panorama 360 on Linux: What Actually Works (Hands-On, 2026)](https://pimpmycooler.com/en/guides/tryx-panorama-360-linux) - PimpMyCooler hands-on guide for using the DXVSI project on a retail Panorama 360 ARGB.

## What was done

- Analysis of KANALI resources to identify the device-side preset catalog without redistributing its extracted media
- Full protocol analysis to discover device commands for system metrics display
- Implemented working real-time CPU/GPU/Disk temperature monitoring on the cooler screen
- Built complete Qt6 GUI from scratch (Homepage, Panorama, Settings pages)
- Auto-detection of CPU/GPU hardware names for badge display
- Auto-conversion of non-MP4 media formats (WebM, MKV, AVI, GIF) before upload to device
- Fixed serial communication issues (timeouts, wrong command formats, broken ADB quoting)
- Restructured into a single qmake project

## Features

- Upload images, videos, GIFs (auto-converts non-MP4 formats)
- Origin-aware PASE media catalog that labels device presets separately from user uploads
- Real-time system metrics on display (temperature, usage, frequency, power and date/time)
- Hardware name badges (auto-detected from system)
- Brightness control (0-100)
- Display settings: position, alignment, color, filter
- Keepalive daemon for persistent display
- Auto-detects legacy devices through `/dev/ttyACM*` and PASE firmware through direct libusb discovery
- System tray integration (KDE Plasma native)
- Settings persistence between sessions
- Async device communication (non-blocking GUI)
- Native firmware update flow for locally selected Panorama SE OTA and Rockchip packages
- Device information and media list over the new KANALI USB printer-class protocol
- Direct asynchronous libusb transport with one request-scoped IN armed before OUT and bounded response reads after known OUT completion
- Exact operation IDs, progress, cancellation, verified completion, and manual retry through D-Bus Manager2
- Backward-compatible media catalog through D-Bus Manager1 and an enhanced origin-aware catalog through Manager2 API version 6
- Content-aware Save that reuses a verified PASE copy instead of uploading the same local media again
- Verified deletion of one eligible user media file at a time, with crash-safe reconciliation and no automatic FileRemove replay
- One shared Panorama operation banner with progress, cancellation, and one fail-closed manual retry candidate
- Daemon-owned PASE metric configuration and one-second sampling that continue after the GUI closes
- Runtime API compatibility check that prevents a new GUI from silently using an outdated background daemon

## Media-free distribution

The application does not bundle, install, or search for the extracted KANALI video library. Factory media already stored on PASE appears in the unified Media Library as a read-only `DEVICE PRESET`; it can be selected and applied but never deleted by the application. User uploads remain labelled separately. A user file with the same name as a known device file is still treated according to its catalog origin and is never promoted to a preset by filename.

Manual user upload remains available. Thumbnails are generated from the user-selected source and shown only after the runtime has confirmed the uploaded origin in its device-scoped XDG media catalog. Legacy files left by an older installation under `/usr/share/tryx-panorama-manager/media` are ignored by the current runtime and are not deleted automatically.

## Native Linux packages

Version 2.0 uses one source version to build separate native packages for
Fedora, Ubuntu/Linux Mint, and Arch Linux. A Fedora binary is not reused on
other distributions.

Release assets use these formats:

- RPM `x86_64` for supported Fedora releases
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
sudo dnf install --allowerasing ./tryx-panorama-manager-2.0.0-1.fc44.x86_64.rpm

# Ubuntu 24.04 or Linux Mint 22
sudo apt install ./tryx-panorama-manager_2.0.0-1_amd64.deb

# Arch Linux
sudo pacman -U ./tryx-panorama-manager-2.0.0-1-x86_64.pkg.tar.zst
```

For Fedora, follow the
[RPM Fusion configuration instructions](https://rpmfusion.org/Configuration)
before installing the RPM. Fedora's `ffmpeg-free` can provide an `ffmpeg`
executable without the `libx264` encoder required by PASE media preparation.
Use `--allowerasing` when installing the RPM so DNF can replace an existing
`ffmpeg-free` package with RPM Fusion's full `ffmpeg` build.

Native packages install the binary, desktop entry, icon, systemd user unit,
and two PASE udev rules. They do not enable autostart or restart an existing
daemon during an upgrade. Reconnect the PASE USB cable after installation,
launch the application once, and enable autostart in Settings only if wanted.

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
- Qt6 (Core, D-Bus, Gui, Network, Widgets)
- C++17 compiler
- qmake6
- Qt6 translation tools with `lrelease`
- protoc and the matching full C++ protobuf development runtime
- libudev development files
- libusb 1.0 development files
- systemd development metadata

Fedora build dependencies:

```fish
sudo dnf install -y gcc-c++ git make dbus-daemon pkgconf-pkg-config qt6-qtbase-devel qt6-linguist protobuf-compiler protobuf-devel systemd-devel libusb1-devel
```

Ubuntu 24.04 and Linux Mint 22 build dependencies:

```fish
sudo apt install build-essential dbus-user-session git libprotobuf-dev libsystemd-dev libudev-dev libusb-1.0-0-dev pkg-config protobuf-compiler qmake6 qt6-base-dev qt6-l10n-tools
```

Arch Linux build dependencies:

```fish
sudo pacman -S --needed base-devel dbus git libusb protobuf qt6-base qt6-tools systemd
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

Fedora runtime dependencies:

```fish
sudo dnf install -y android-tools unzip e2fsprogs ffmpeg mesa-demos
```

**Permissions:**
- User must be in `dialout` group (or `uucp` on Arch) for serial access
- New KANALI firmware exposes Panorama SE as USB printer-class `391a:1021`; direct libusb access uses `/dev/bus/usb/*/*` and requires the `lp` group or a seat ACL from `TAG+="uaccess"`
- Fedora's generic printer rule must not start CUPS `configure-printer` for this vendor protocol. The qmake install target places an early access rule and a late printer-suppression rule in `/usr/lib/udev/rules.d`; do not create same-named overrides in `/etc/udev/rules.d`, because they would shadow packaged updates.

## Firmware Updates

The firmware updater supports two local package formats for Panorama SE:

- Legacy Android OTA `update.zip` for `cm01_se` devices. The app validates `META-INF/com/android/metadata`, copies the package to `/sdcard/update.zip` over ADB, verifies the copied size, and reboots the cooler into recovery.
- New KANALI Rockchip loader ZIP bundles for `PASE`. The app validates the required Rockchip files, checks `parameter.txt` for `RK3568`, and inspects `rootfs:/usr/bin/panorama` for the product marker. Flashing uses an external Rockchip `upgrade_tool` executable when it is available. If an ADB device is present, the app reboots it into Loader first; if RockUSB Loader or Maskrom is already present, the app can continue directly without ADB.

The `upgrade_tool` executable is not bundled in this open source repository because its redistribution rights are not clear. The app looks for it in `TRYX_UPGRADE_TOOL`, `PATH`, next to the app binary, `tools/upgrade_tool`, and `~/.local/bin/upgrade_tool`.

Rockchip RK3568 loader access may require a local udev rule for USB VID/PID `2207:350a` so the flashing backend can reset or inspect the device without root.

After updating to the new KANALI firmware, the cooler no longer exposes ADB by default. It appears as `391a:1021 RK PASE` with a bidirectional printer interface. The app generates C++ types from three minimal, project-owned schemas under `protocol/wire-v1`; recovered vendor descriptor sources are not a build or release dependency. The production path does not read or write `/dev/usb/lp*`: it claims the `07/01/02` interface through usbfs, temporarily detaches `usblp`, arms one bulk IN before each request, never re-arms that endpoint while the matching bulk OUT is still active, drains optional periodic responses to a complete frame boundary after OUT, and releases the interface on shutdown.

All printer operations are serialized by one worker-owned session, while cancellable ffmpeg conversion runs outside the USB worker. Passive udev discovery recognizes the `391a:0006 rk3xxx` Rockchip gadget identity but never opens it. Discovery is based on physical USB device events and stable bus/port identity, so the app does not mistake its own `usblp` detach or attach for a physical reconnect. Printer Class `GET_PORT_STATUS` is deliberately not used because PASE does not provide a reliable readiness signal through that request. A physical remove/add creates a new connection generation, interrupts old I/O through its cancellation gate, and discards stale results. Recovery confirms protocol readiness through an exact DeviceInfo response, completes the remaining bootstrap once, sends one post-bootstrap Ping, restores the confirmed overlay at most once, and only then starts metrics. It never retries a complete bootstrap in the same physical generation or automatically replays user configuration, upload, delete, or apply mutations.

The readiness phase has a 20-second monotonic deadline. It retries only a DeviceInfo request whose USB OUT is confirmed to have transferred zero bytes, keeping the same claimed handle and using capped `500`, `1000`, then `2000` millisecond backoff. A partial or unknown OUT, cancellation, malformed response, or a complete OUT without the exact DeviceInfo response is terminal for that physical generation. System configuration and authentication queries are each sent at most once after readiness. Keepalive uses the observed untracked Ping frame and drains an optional asynchronous Pong. Metrics sampling and mutations start only after the post-bootstrap barrier. Manual upload uses the response-driven begin/data/end flow, converts media to the device's raw H264 format, gives data chunks a dedicated 15-second OUT deadline, and verifies the exact new name, prepared size, writable flag, and user source through a fresh media catalog before reporting success or applying it. Save first hashes the opened source file, looks up the source hash and versioned conversion profile in the device-scoped catalog, refreshes that catalog, and applies an exact verified match without conversion or retransmission. No completed IN transfer is re-armed while any OUT remains active, preventing queued response fragments or `EPROTO` completions from starving the writer. Periodic write-only commands perform a bounded post-OUT drain; no response is acceptable, but a partial or malformed frame closes the session fail-closed. A persistent bulk-IN failure latches the current USB endpoint generation as lost. Production does not call `libusb_reset_device`, retry the same generation, or replay its last mutation; recovery requires an observed physical remove/add cycle or a full PASE power cycle that creates a new generation. Conversion and preview subprocesses have bounded deadlines; a preview timeout falls back to an honest placeholder without discarding valid H264. The direct USB reader can recover a complete tracked protobuf when faulty PASE firmware drops only the `TRYX` frame header after an IN transport error; recovery still requires the exact transaction ID and expected response body. Manager2 API version 6 exposes stable UUIDs, structured operation states, origin-aware catalog entries, typed display mutations, confirmed display state, per-side overlay configuration, and explicit backlight power control. Manager1 retains its original catalog tuple for ABI compatibility. A verified prepared file and its staged JPEG preview are cached atomically after a failed transfer and can only be retried manually after prepared-file hash, device-generation, and media-catalog checks; the original source file is not required after conversion. If a data transfer ends partially or with an unknown outcome, its recovery requirement remains sticky across retries and daemon restarts. Upload, Retry, Apply, Delete, and metrics changes remain blocked until the runtime observes removal and reconnection of the current PASE endpoint, because closing libusb or issuing a generic USB reset does not prove that firmware discarded its hidden transfer session. A successful verification promotes the preview and content identity into the XDG media catalog. Apply is not atomic: uncertain writes are reported as partial or unknown, the session is closed, and no automatic rollback or replay is attempted.

PASE full-screen mode supports up to three exact protocol metrics selected from CPU temperature, frequency, usage and power; GPU temperature, frequency, usage and power; memory frequency and usage; and date/time. A separate Manager2 operation sends the overlay layout, then the background daemon sends live values through a headerless metric batch every second. The two-second background scheduler supports two measured arms through `pase_overlay_lease_mode` in the existing XDG `config.json`: `ping-and-overlay-lease` alternates Ping with a full overlay lease, while `ping-only` sends only Ping after the initial reconnect overlay restoration. The default preserves the current `ping-and-overlay-lease` behavior until the A/B monitor selects an arm. The lease never writes user configuration or media state. An explicit protocol error from either metric update or layout lease is fail-closed instead of being discarded. Metric sampling pauses during upload or Apply and coalesces to the latest sample, while a delayed tracked response can still receive one bounded liveness command without replaying the mutation. The confirmed layout is stored only for the same non-empty device serial and survives GUI or daemon restarts. Missing sensors remain unavailable instead of being reported as zero. The Memory Frequency protocol label is retained for compatibility, but the current Linux runtime reports it as unavailable because upstream Linux does not expose a portable unprivileged source for the live DRAM clock; static SMBIOS transfer rates are not mislabeled as MHz.

PASE media deletion is limited to one exact user-owned, writable, unreferenced catalog entry per operation. The runtime persists a delete-intent journal before sending a single USB media-removal request and reports success only after a fresh media catalog no longer contains the exact name. A lost or ambiguous response enters read-only reconciliation; media removal is never replayed automatically.

PASE display configuration uses one read-modify-write user-configuration update, one complete overlay-layout update, and a bounded configuration readback. The UI supports brightness, display backlight power, Mirror, Waterfall, Full Screen, and Screen Splitting with two existing media files. Firmware-controlled standby enablement and standby media remain read-only and are never rewritten by the display power control. Rapid brightness input keeps at most one active operation and one latest pending value; the pending value is dispatched only after the prior operation, exact readback, and a subsequent background keepalive all succeed. Each screen area can contain up to three metrics plus CPU and GPU badges with exact `#RRGGBB` text colors. Mirror uses `media_rotation=180`, Waterfall uses `ui_rotation=90`, and split mode uses the dual-media wire mode with independent left and right media. A display operation succeeds only when the requested fields match the fresh device readback. A matching explicit protocol error from the optional overlay response is treated as a logical rejection and is never discarded as stale. Direct printer-protocol firmware writes and loader reboot remain intentionally disabled.

Automatic firmware download is not enabled yet. KANALI uses SM2-encrypted request/response bodies for its firmware version and download URL requests, so plain REST requests cannot retrieve official packages.

## Build

```fish
git clone --branch production https://github.com/DXVSI/tryx-panorama-se-360-linux-gui.git tryx-panorama-current; and cd tryx-panorama-current
qmake6 tryx-panorama.pro; and make -j(nproc)
./build/tryx-panorama-manager
```

System installation includes the binary, user service, PASE usbfs rule, desktop entry, icon, and translations. It does not install a video library:

```fish
sudo make install; and sudo udevadm control --reload-rules; and sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=391a --attr-match=idProduct=1021; and sudo udevadm settle --timeout=10
systemctl --user daemon-reload; and systemctl --user start tryx-panorama.service; and systemctl --user is-active tryx-panorama.service
```

The command above is the first-install path. When updating an existing manual
source installation, first finish or cancel every active media operation, then
install the new files and restart the daemon explicitly:

```fish
sudo make install; and systemctl --user daemon-reload; and systemctl --user restart tryx-panorama.service; and systemctl --user is-active tryx-panorama.service
```

Native package upgrades deliberately do not force this restart because a
package transaction cannot prove that another user's media operation is idle.

The install target supplies a user preset that keeps autostart disabled by
default. Enable it later from Settings or explicitly with
`systemctl --user enable tryx-panorama.service`.

The version command is safe to use without a graphical or D-Bus session:

```fish
./build/tryx-panorama-manager --version
```

Offline printer-protocol tests do not access physical USB hardware:

```fish
cd tests; and qmake6 printerprotocol_tests.pro; and make -j(nproc); and ../build/tests/printerprotocol-tests
```

## Project Structure

```
src/
  core/              # Device protocol library
  main.cpp           # Entry point
  mainwindow.*       # Main window with navigation
  panoramapage.*     # Display + metrics configuration
  homepage.*         # System monitoring dashboard
  settingspage.*     # App settings
  devicemanager.*    # Async device communication
  printerprotocol.*  # PASE framing, direct libusb transport and udev discovery
  systemmonitor.*    # System metrics reader
  traymanager.*      # System tray
include/panorama/    # Protocol headers
protocol/wire-v1/    # Minimal project-owned protobuf wire schema
tests/               # Offline protocol, transport and discovery tests
debian/              # Ubuntu 24.04 and Linux Mint 22 package metadata
packaging/
  arch/              # Arch Linux PKGBUILD
  rpm/               # Fedora RPM spec
  metainfo/          # AppStream metadata
  scripts/           # Release and package-content gates
  *.rules            # PASE permissions and printer suppression
```

## Tested on

| Distro | Kernel | CPU | GPU1 | GPU2 |
|--------|--------|-----|------|------|
| Fedora 44 | 7.1.3-200.fc44.x86_64 | AMD Ryzen 9 9950X3D | AMD Radeon RX 7900 XTX | AMD Radeon RX 7900 XTX |

## License

MIT. See [LICENSE](LICENSE).
