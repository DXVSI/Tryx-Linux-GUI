# TRYX Panorama Linux GUI

Qt6 GUI application for managing TRYX Panorama AIO cooler displays on Linux.

## Supported Models

| Model | Status |
|-------|--------|
| Panorama SE 360 | Tested |
| Panorama SE 240 | Should work (same display protocol) |
| Panorama | Should work |
| Panorama WB | Should work |

<div align="center">

https://github.com/user-attachments/assets/f9baac04-fe28-4aeb-a8ea-eb2af37ff6cb

</div>

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
- Real-time system metrics on display (temperature, usage, frequency)
- Hardware name badges (auto-detected from system)
- Brightness control (0-100)
- Display settings: position, alignment, color, filter
- Keepalive daemon for persistent display
- Auto-detects device (scans /dev/ttyACM*)
- System tray integration (KDE Plasma native)
- Settings persistence between sessions
- Async device communication (non-blocking GUI)

## Requirements

**Build:**
- Qt6 (Core, Gui, Widgets)
- C++17 compiler
- qmake6

**Runtime:**
- `adb` (android-tools) - file transfer
- `ffmpeg` - media conversion
- `glxinfo` (mesa-utils) - GPU name detection (optional)

**Permissions:**
- User must be in `dialout` group (or `uucp` on Arch) for serial access

## Build

```bash
git clone https://gitlab.com/dxvsi/tryx-panorama-linux.git
cd tryx-panorama-linux
qmake6
make -j$(nproc)
./build/tryx-panorama-manager
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
  systemmonitor.*    # System metrics reader
  traymanager.*      # System tray
include/panorama/    # Protocol headers
media/               # Built-in videos
```

## Tested on

| Distro | Kernel | CPU | GPU1 | GPU2 |
|--------|--------|-----|------|------|
| Fedora 43 | 6.19.10 | AMD Ryzen 9 9950X3D | AMD Radeon RX 7900 XTX | NVIDIA GeForce RTX 5060 Ti |

## License

MIT

## Credits

Based on [reed-tpse](https://github.com/fadli0029/reed-tpse) protocol library.
