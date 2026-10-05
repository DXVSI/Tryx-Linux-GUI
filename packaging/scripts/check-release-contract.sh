#!/bin/sh
set -eu

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [vX.Y.Z]" >&2
    exit 2
fi

script_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
project_root=$(unset CDPATH; cd -- "$script_dir/../.." && pwd)
version_file=$project_root/VERSION

version=$(sed -n '1p' "$version_file")
if ! grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' "$version_file" ||
   [ "$(wc -l < "$version_file")" -ne 1 ]; then
    echo "VERSION must contain one semantic version line" >&2
    exit 1
fi

rpm_version=$(awk '$1 == "Version:" { print $2; exit }' \
    "$project_root/packaging/rpm/tryx-panorama-manager.spec")
debian_version=$(sed -n \
    '1s/^[^[:space:]]*[[:space:]]*(\([^)]*\)).*$/\1/p' \
    "$project_root/debian/changelog")
debian_version=${debian_version%-*}
arch_version=$(sed -n 's/^pkgver=//p' \
    "$project_root/packaging/arch/PKGBUILD" | sed -n '1p')
arch_version=$(printf '%s' "$arch_version" | tr -d "'\"")

for entry in \
    "RPM:$rpm_version" \
    "Debian:$debian_version" \
    "Arch:$arch_version"
do
    package_format=${entry%%:*}
    package_version=${entry#*:}
    if [ "$package_version" != "$version" ]; then
        echo "$package_format version $package_version does not match VERSION $version" >&2
        exit 1
    fi
done

if ! grep -Fq "<release version=\"$version\"" \
    "$project_root/packaging/metainfo/io.github.dxvsi.tryx_panorama_manager.metainfo.xml"; then
    echo "AppStream release does not match VERSION $version" >&2
    exit 1
fi

if [ "$#" -eq 1 ] && [ "$1" != "v$version" ]; then
    echo "tag $1 does not match VERSION $version" >&2
    exit 1
fi

echo "release contract verified: $version"
