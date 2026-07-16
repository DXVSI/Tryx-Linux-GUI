# TRYX Panorama Linux GUI

Qt6 GUI application for managing TRYX Panorama AIO cooler displays on Linux.

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

- Unpacking KANALI (official Windows app) resources to extract built-in media library (15 videos)
- Full protocol analysis to discover device commands for system metrics display
- Implemented working real-time CPU/GPU/Disk temperature monitoring on the cooler screen
- Built complete Qt6 GUI from scratch (Homepage, Panorama, Settings pages)
- Auto-detection of CPU/GPU hardware names for badge display
- Auto-conversion of non-MP4 media formats (WebM, MKV, AVI, GIF) before upload to device
- Fixed serial communication issues (timeouts, wrong command formats, broken ADB quoting)
- Restructured into a single qmake project

## Features

- Upload images, videos, GIFs (auto-converts non-MP4 formats)
- 7 built-in device presets (Cooling delivery, Migration, etc.)
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
- Backward-compatible media catalog through D-Bus Manager1 and an enhanced origin-aware catalog through Manager2 API version 3
- Content-aware Save that reuses a verified PASE copy instead of uploading the same local media again
- Verified deletion of one eligible user media file at a time, with crash-safe reconciliation and no automatic FileRemove replay
- One shared operation banner on both Panorama tabs with progress, cancellation, and one fail-closed manual retry candidate
- Daemon-owned PASE metric configuration and one-second sampling that continue after the GUI closes
- Runtime API compatibility check that prevents a new GUI from silently using an outdated background daemon

## Requirements

**Build:**
- Qt6 (Core, Gui, Widgets)
- C++17 compiler
- qmake6
- protoc and the matching full C++ protobuf development runtime
- libudev development files
- libusb 1.0 development files

Fedora build dependencies:

```fish
sudo dnf install -y qt6-qtbase-devel qt6-qtmultimedia-devel qt6-linguist protobuf-compiler protobuf-devel systemd-devel libusb1-devel
```

**Runtime:**
- `adb` (android-tools) - legacy OTA file transfer and optional Rockchip reboot-to-loader
- `unzip` - firmware package validation and extraction
- `debugfs` (e2fsprogs) - Rockchip rootfs inspection
- `upgrade_tool` - optional external Rockchip flashing backend for new KANALI firmware bundles
- `ffmpeg` - media conversion
- `glxinfo` (mesa-utils) - GPU name detection (optional)

Fedora runtime dependencies:

```fish
sudo dnf install -y android-tools unzip e2fsprogs ffmpeg mesa-demos
```

**Permissions:**
- User must be in `dialout` group (or `uucp` on Arch) for serial access
- New KANALI firmware exposes Panorama SE as USB printer-class `391a:1021`; direct libusb access uses `/dev/bus/usb/*/*` and requires the `lp` group or a seat ACL from `TAG+="uaccess"`
- Fedora's generic printer rule must not start CUPS `configure-printer` for this vendor protocol. The qmake install target places the supplied rule in `/usr/lib/udev/rules.d`; do not create a same-named override in `/etc/udev/rules.d`, because it would shadow packaged updates.

## Firmware Updates

The firmware updater supports two local package formats for Panorama SE:

- Legacy Android OTA `update.zip` for `cm01_se` devices. The app validates `META-INF/com/android/metadata`, copies the package to `/sdcard/update.zip` over ADB, verifies the copied size, and reboots the cooler into recovery.
- New KANALI Rockchip loader ZIP bundles for `PASE`. The app validates the required Rockchip files, checks `parameter.txt` for `RK3568`, and inspects `rootfs:/usr/bin/panorama` for the product marker. Flashing uses an external Rockchip `upgrade_tool` executable when it is available. If an ADB device is present, the app reboots it into Loader first; if RockUSB Loader or Maskrom is already present, the app can continue directly without ADB.

The `upgrade_tool` executable is not bundled in this open source repository because its redistribution rights are not clear. The app looks for it in `TRYX_UPGRADE_TOOL`, `PATH`, next to the app binary, `tools/upgrade_tool`, and `~/.local/bin/upgrade_tool`.

Rockchip RK3568 loader access may require a local udev rule for USB VID/PID `2207:350a` so the flashing backend can reset or inspect the device without root.

After updating to the new KANALI firmware, the cooler no longer exposes ADB by default. It appears as `391a:1021 RK PASE` with a bidirectional printer interface. The app uses generated C++ classes from the KANALI 2.3.1 UDB protobuf descriptors and a direct asynchronous libusb transport. The production path does not read or write `/dev/usb/lp*`: it claims the `07/01/02` interface through usbfs, temporarily detaches `usblp`, arms one bulk IN before each request, never re-arms that endpoint while the matching bulk OUT is still active, drains optional periodic responses to a complete frame boundary after OUT, and releases the interface on shutdown. The seven recovered schema sources are pinned by the composite SHA-256 `dc54eb21679a9c7b28e1044a49b03e9f4809c87231ad4040d0ed47078786c990`.

All printer operations are serialized by one worker-owned session, while cancellable ffmpeg conversion runs outside the USB worker. Passive udev discovery recognizes the `391a:0006 rk3xxx` Rockchip gadget identity but never opens it. Discovery is based on physical USB device events and stable bus/port identity, so the app does not mistake its own `usblp` detach or attach for a physical reconnect. Printer Class `GET_PORT_STATUS` must report `Selected + Not Error` before the first bulk write. A physical remove/add creates a new connection generation and stale results are discarded. Recovery performs protocol bootstrap, restores the confirmed UI layout, resumes `FileListPb` reconciliation when required, and then starts keepalive. It never replays `UserConfigPb`, upload, or apply automatically.

The display session reproduces the KANALI UDB bootstrap sequence `GetDeviceInfoPb("NA")`, `GetSysConfigPb("NA")`, and `GetDeviceAuthPb(1)`, followed by a complete tracked empty `RunConfigPb` setter write. Keepalive uses the exact untracked `PingPb("hello?")` frame and drains an optional asynchronous `Pong`. When a persisted metrics or badge overlay exists after USB re-enumeration, external session readiness remains blocked until one complete post-bootstrap Ping write and exactly one complete tracked overlay `RunConfigPb` setter write; an optional matching `DummyMsgPb` may explicitly reject the setter but is not required for success. Metrics sampling and mutations start only after that barrier. Manual upload uses the response-driven `file_transmit_begin/data/end` flow, converts media to KANALI-style raw H264, gives data chunks a dedicated 15-second OUT deadline, and verifies the exact new name, prepared size, writable flag, and user source through `FileListPb` before reporting success or applying it. Save first hashes the opened source file, looks up the source hash and versioned conversion profile in the device-scoped catalog, refreshes `FileListPb`, and applies an exact verified match without conversion or retransmission. No completed IN transfer is re-armed while any OUT remains active, preventing queued response fragments or `EPROTO` completions from starving the writer. Periodic write-only commands perform a bounded post-OUT drain; no response is acceptable, but a partial or malformed frame closes the session fail-closed. A persistent bulk-IN failure latches the current USB endpoint generation as lost. Production does not call `libusb_reset_device`, retry the same generation, or replay its last mutation; recovery requires an observed physical remove/add cycle or a full PASE power cycle that creates a new generation. Conversion and preview subprocesses have bounded deadlines; a preview timeout falls back to an honest placeholder without discarding valid H264. The direct USB reader can recover a complete tracked protobuf when faulty PASE firmware drops only the `TRYX` frame header after an IN transport error; recovery still requires the exact transaction ID and expected response body. Manager2 API version 6 exposes stable UUIDs, structured operation states, origin-aware catalog entries, typed display mutations, confirmed display state, per-side overlay configuration, and explicit backlight power control. Manager1 retains its original catalog tuple for ABI compatibility. A verified prepared file and its staged JPEG preview are cached atomically after a failed transfer and can only be retried manually after prepared-file hash, device-generation, and FileList checks; the original source file is not required after conversion. If a data transfer ends partially or with an unknown outcome, its recovery requirement remains sticky across retries and daemon restarts. Upload, Retry, Apply, Delete, and metrics changes remain blocked until the runtime observes removal and reconnection of the current PASE endpoint, because closing libusb or issuing a generic USB reset does not prove that firmware discarded its hidden transfer session. A successful verification promotes the preview and content identity into the XDG media catalog. Apply is not atomic: uncertain writes are reported as partial or unknown, the session is closed, and no automatic rollback or replay is attempted.

PASE full-screen mode supports up to three exact KANALI metrics selected from CPU temperature, frequency, usage and power; GPU temperature, frequency, usage and power; memory frequency and usage; and date/time. A separate Manager2 operation sends the recovered `RunConfigPb` label layout, then the background daemon sends live values through the headerless `BatchGroupLabelUpdatePb` frame every second. The two-second background scheduler alternates the exact UDB `PingPb("hello?")` with an untracked full overlay layout lease after the initial layout is confirmed. The lease restores volatile labels if the PASE renderer drops them without USB re-enumeration and never writes `UserConfigPb` or media state. An explicit optional `ErrorPb` from either metric update or layout lease is fail-closed instead of being discarded. Metric sampling pauses during upload or Apply and coalesces to the latest sample, while a delayed tracked response can still receive one bounded liveness command without replaying the mutation. The confirmed layout is stored only for the same non-empty device serial and survives GUI or daemon restarts. Missing sensors remain unavailable instead of being reported as zero. The Memory Frequency protocol label is retained for compatibility, but the current Linux runtime reports it as unavailable because upstream Linux does not expose a portable unprivileged source for the live DRAM clock; static SMBIOS transfer rates are not mislabeled as MHz.

PASE media deletion is limited to one exact user-owned, writable, unreferenced catalog entry per operation. The runtime persists a delete-intent journal before sending a single USB `file_remove=403` request and reports success only after a fresh `FileListPb` no longer contains the exact name. A lost or ambiguous response enters read-only reconciliation; `FileRemovePb` is never replayed automatically.

PASE display configuration uses one read-modify-write `UserConfigPb`, one complete `RunConfigPb`, and a bounded `GetUserConfigPb` readback. The UI supports brightness, display backlight power, Mirror, Waterfall, Full Screen, and Screen Splitting with two existing media files. Firmware-controlled standby enablement and standby media remain read-only and are never rewritten by the display power control. Rapid brightness input keeps at most one active operation and one latest pending value; the pending value is dispatched only after the prior operation, exact readback, and a subsequent background keepalive all succeed. Each screen area can contain up to three metrics plus CPU and GPU badges with exact `#RRGGBB` text colors. Mirror uses `media_rotation=180`, Waterfall uses `ui_rotation=90`, and split mode uses `MediaMode_Dual` with independent left and right media. A display operation succeeds only when the requested fields match the fresh device readback. A matching explicit `ErrorPb` from the optional RunConfig response is treated as a logical rejection and is never discarded as stale. Direct printer-protocol firmware writes and loader reboot remain intentionally disabled.

Automatic firmware download is not enabled yet. KANALI uses SM2-encrypted request/response bodies for its firmware version and download URL requests, so plain REST requests cannot retrieve official packages.

## Build

```fish
git clone https://gitlab.com/dxvsi/tryx-panorama-linux.git; and cd tryx-panorama-linux
qmake6 tryx-panorama.pro; and make -j(nproc)
./build/tryx-panorama-manager
```

System installation includes the binary, user service, PASE usbfs rule, desktop entry, icon, translations, and built-in media library:

```fish
sudo make install; and sudo udevadm control --reload-rules; and sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=391a --attr-match=idProduct=1021; and sudo udevadm settle --timeout=10
systemctl --user daemon-reload; and systemctl --user enable tryx-panorama.service; and systemctl --user restart tryx-panorama.service; and systemctl --user is-active tryx-panorama.service
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
  printerprotocol.*  # KANALI framing, direct libusb transport and udev discovery
  systemmonitor.*    # System metrics reader
  traymanager.*      # System tray
include/panorama/    # Protocol headers
proto/               # Recovered KANALI 2.3.1 protobuf schema
tests/               # Offline protocol, transport and discovery tests
packaging/           # udev rule for PASE permissions and CUPS exclusion
media/               # Built-in videos
```

## Tested on

| Distro | Kernel | CPU | GPU1 | GPU2 |
|--------|--------|-----|------|------|
| Fedora 44 | 7.1.3-200.fc44.x86_64 | AMD Ryzen 9 9950X3D | AMD Radeon RX 7900 XTX | AMD Radeon RX 7900 XTX |

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

## License

MIT. See [LICENSE](LICENSE).
