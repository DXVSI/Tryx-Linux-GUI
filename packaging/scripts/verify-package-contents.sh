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

require_file /usr/bin/tryx-panorama-manager
require_file /usr/lib/systemd/user/tryx-panorama.service
require_file /usr/lib/systemd/user-preset/90-tryx-panorama.preset
require_file /usr/lib/udev/rules.d/70-tryx-pase-access.rules
require_file /usr/lib/udev/rules.d/99-tryx-pase-printer.rules
require_file /usr/share/applications/tryx-panorama-manager.desktop
require_file /usr/share/icons/hicolor/256x256/apps/tryx-panorama.png
require_file /usr/share/metainfo/io.github.dxvsi.tryx_panorama_manager.metainfo.xml
require_file /usr/share/licenses/tryx-panorama-manager/LICENSE
require_file /usr/share/licenses/tryx-panorama-manager/picojson-BSD-2-Clause.txt

manpage_count=0
for manpage in \
    "$staged_root/usr/share/man/man1/tryx-panorama-manager.1" \
    "$staged_root/usr/share/man/man1/tryx-panorama-manager.1.gz"
do
    if [ -f "$manpage" ]; then
        manpage_count=$((manpage_count + 1))
    fi
done
if [ "$manpage_count" -ne 1 ]; then
    echo "package must contain exactly one compressed or uncompressed man page" >&2
    exit 1
fi

if [ ! -x "$staged_root/usr/bin/tryx-panorama-manager" ]; then
    echo "package binary is not executable" >&2
    exit 1
fi

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

if ldd "$staged_root/usr/bin/tryx-panorama-manager" | grep -q 'not found'; then
    echo "package binary has unresolved dynamic libraries" >&2
    exit 1
fi

echo "package contents verified: $staged_root"
