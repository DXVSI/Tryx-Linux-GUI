#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <staged-root>" >&2
    exit 2
fi

staged_root=${1%/}
if [ -z "$staged_root" ] || [ ! -d "$staged_root" ]; then
    echo "staged root is not a directory: $1" >&2
    exit 2
fi

script_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
project_root=$(unset CDPATH; cd -- "$script_dir/../.." && pwd)
expected_version=$(sed -n '1p' "$project_root/VERSION")
if ! grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' "$project_root/VERSION" ||
   [ "$(wc -l < "$project_root/VERSION")" -ne 1 ]; then
    echo "VERSION must contain one semantic version line" >&2
    exit 1
fi

require_file() {
    relative_path=$1
    if [ ! -f "$staged_root$relative_path" ]; then
        echo "missing package file: $relative_path" >&2
        exit 1
    fi
}

require_exact_manpage() {
    required_manpage=$1
    required_manpage_count=0
    for required_manpage_path in \
        "$staged_root/usr/share/man/man1/$required_manpage.1" \
        "$staged_root/usr/share/man/man1/$required_manpage.1.gz"
    do
        if [ -f "$required_manpage_path" ]; then
            required_manpage_count=$((required_manpage_count + 1))
        fi
    done
    if [ "$required_manpage_count" -ne 1 ]; then
        echo "package must contain exactly one compressed or uncompressed $required_manpage(1)" >&2
        exit 1
    fi
}

require_file /usr/bin/tryx-panorama-manager
require_file /usr/bin/tryx
require_file /usr/lib/tryx-panorama-manager/tryx-panorama-runtime
require_file /usr/lib/systemd/user/tryx-panorama.service
require_file /usr/lib/systemd/user-preset/90-tryx-panorama.preset
require_file /usr/lib/udev/rules.d/70-tryx-pase-access.rules
require_file /usr/lib/udev/rules.d/99-tryx-pase-printer.rules
require_file /usr/share/applications/tryx-panorama-manager.desktop
require_file /usr/share/icons/hicolor/256x256/apps/tryx-panorama.png
require_file /usr/share/metainfo/io.github.dxvsi.tryx_panorama_manager.metainfo.xml
require_file /usr/share/licenses/tryx-panorama-manager/LICENSE
require_file /usr/share/licenses/tryx-panorama-manager/picojson-BSD-2-Clause.txt

desktop_entry=$staged_root/usr/share/applications/tryx-panorama-manager.desktop
if ! grep -Fxq 'Exec=tryx-panorama-manager' "$desktop_entry"; then
    echo "desktop launcher must use the packaged GUI command" >&2
    exit 1
fi
if ! grep -Fxq 'Icon=tryx-panorama' "$desktop_entry"; then
    echo "desktop launcher must use the packaged application icon" >&2
    exit 1
fi

for forbidden_autostart in \
    "$staged_root/etc/xdg/autostart/tryx-panorama-manager.desktop" \
    "$staged_root/usr/etc/xdg/autostart/tryx-panorama-manager.desktop"
do
    if [ -e "$forbidden_autostart" ] ||
       [ -L "$forbidden_autostart" ]; then
        echo "package must not enable system-wide GUI autostart: ${forbidden_autostart#"$staged_root"}" >&2
        exit 1
    fi
done

expected_usb_product_ids=$(printf '%s\n' 1011 1021 2011)

udev_product_ids() {
    sed -n \
        's/.*ATTR{idVendor}=="391a".*ATTR{idProduct}=="\([[:xdigit:]]\{4\}\)".*/\1/p' \
        "$1" |
        tr '[:upper:]' '[:lower:]' |
        LC_ALL=C sort
}

appstream_product_ids() {
    sed -n \
        's|.*<modalias>usb:v391Ap\([[:xdigit:]]\{4\}\)d\*</modalias>.*|\1|p' \
        "$1" |
        tr '[:upper:]' '[:lower:]' |
        LC_ALL=C sort
}

for usb_product_source in \
    "$staged_root/usr/lib/udev/rules.d/70-tryx-pase-access.rules" \
    "$staged_root/usr/lib/udev/rules.d/99-tryx-pase-printer.rules"
do
    packaged_usb_product_ids=$(udev_product_ids "$usb_product_source")
    if [ "$packaged_usb_product_ids" != "$expected_usb_product_ids" ]; then
        echo "unexpected supported USB product IDs in ${usb_product_source#"$staged_root"}:" >&2
        printf '%s\n' "$packaged_usb_product_ids" >&2
        exit 1
    fi
done

metainfo_path=$staged_root/usr/share/metainfo/io.github.dxvsi.tryx_panorama_manager.metainfo.xml
packaged_usb_product_ids=$(appstream_product_ids "$metainfo_path")
if [ "$packaged_usb_product_ids" != "$expected_usb_product_ids" ]; then
    echo "unexpected supported USB product IDs in ${metainfo_path#"$staged_root"}:" >&2
    printf '%s\n' "$packaged_usb_product_ids" >&2
    exit 1
fi

if [ -e "$staged_root/usr/bin/tryx-panorama-quick" ]; then
    echo "package contains the retired secondary Quick launcher" >&2
    exit 1
fi

if [ -e "$staged_root/usr/bin/tryx-panorama-cli" ] ||
   [ -L "$staged_root/usr/bin/tryx-panorama-cli" ]; then
    echo "package must not contain the unpublished tryx-panorama-cli alias" >&2
    exit 1
fi

require_exact_manpage tryx-panorama-manager
require_exact_manpage tryx

for binary in \
    "$staged_root/usr/bin/tryx-panorama-manager" \
    "$staged_root/usr/bin/tryx" \
    "$staged_root/usr/lib/tryx-panorama-manager/tryx-panorama-runtime"
do
    if [ ! -x "$binary" ]; then
        echo "package binary is not executable: ${binary#"$staged_root"}" >&2
        exit 1
    fi
done

preset=$(sed -e 's/[[:space:]]*$//' \
    "$staged_root/usr/lib/systemd/user-preset/90-tryx-panorama.preset")
if [ "$preset" != "disable tryx-panorama.service" ]; then
    echo "user preset must keep tryx-panorama.service disabled by default" >&2
    exit 1
fi

for forbidden_path in \
    "$staged_root/usr/share/tryx-panorama-manager/media" \
    "$staged_root/usr/share/tryx-panorama-manager/proto" \
    "$staged_root/usr/share/tryx-panorama-manager/tools"
do
    if [ -e "$forbidden_path" ]; then
        echo "forbidden package path: ${forbidden_path#"$staged_root"}" >&2
        exit 1
    fi
done

if find "$staged_root" -type f \
    \( -iname '*.mp4' -o -iname '*.webm' -o -iname '*.mkv' \
       -o -iname '*.avi' -o -iname '*.mov' -o -iname '*.zip' \
       -o -iname '*.img' -o -iname '*.bin' -o -name 'upgrade_tool' \) \
    -print -quit | grep -q .; then
    echo "package contains forbidden media or proprietary tooling" >&2
    exit 1
fi

version=$("$staged_root/usr/bin/tryx-panorama-manager" --version)
if [ "$version" != "tryx-panorama-manager $expected_version" ]; then
    echo "unexpected version output: $version" >&2
    exit 1
fi

runtime_version=$(
    "$staged_root/usr/lib/tryx-panorama-manager/tryx-panorama-runtime" \
        --version
)
if [ "$runtime_version" != "tryx-panorama-runtime $expected_version" ]; then
    echo "unexpected runtime version output: $runtime_version" >&2
    exit 1
fi

cli_version=$(
    env -u DISPLAY -u WAYLAND_DISPLAY -u QT_QPA_PLATFORM \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/tryx-package-verifier-no-bus \
        "$staged_root/usr/bin/tryx" --version
)
if [ "$cli_version" != "tryx $expected_version" ]; then
    echo "unexpected CLI version output: $cli_version" >&2
    exit 1
fi

cli_help=$(
    env -u DISPLAY -u WAYLAND_DISPLAY -u QT_QPA_PLATFORM \
        DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/tryx-package-verifier-no-bus \
        "$staged_root/usr/bin/tryx" --help
)
if ! printf '%s\n' "$cli_help" | grep -Fq 'Usage: tryx'; then
    echo "packaged CLI help is missing its canonical usage" >&2
    exit 1
fi

for binary in \
    "$staged_root/usr/bin/tryx-panorama-manager" \
    "$staged_root/usr/bin/tryx" \
    "$staged_root/usr/lib/tryx-panorama-manager/tryx-panorama-runtime"
do
    if ldd "$binary" | grep -q 'not found'; then
        echo "package binary has unresolved dynamic libraries: ${binary#"$staged_root"}" >&2
        exit 1
    fi
    if readelf -d "$binary" |
       grep -Eq 'Shared library: \[libQt6Widgets\.so'; then
        echo "package binary links forbidden Qt Widgets: ${binary#"$staged_root"}" >&2
        exit 1
    fi
done

cli_dependencies=$(ldd "$staged_root/usr/bin/tryx")
if printf '%s\n' "$cli_dependencies" |
   grep -Eq 'libQt6(Widgets|Qml|Quick)[^[:space:]]*\.so'; then
    echo "package CLI links forbidden Qt Widgets, QML, or Quick libraries" >&2
    exit 1
fi

service_exec=$(
    sed -n 's/^ExecStart=//p' \
        "$staged_root/usr/lib/systemd/user/tryx-panorama.service"
)
if [ "$service_exec" != \
    "/usr/lib/tryx-panorama-manager/tryx-panorama-runtime" ]; then
    echo "systemd unit does not start the package-private runtime" >&2
    exit 1
fi

echo "package contents verified: $staged_root"
