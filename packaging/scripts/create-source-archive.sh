#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <output-directory>" >&2
    exit 2
fi

script_dir=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
project_root=$(unset CDPATH; cd -- "$script_dir/../.." && pwd)
output_dir=$1

"$script_dir/check-release-contract.sh"

if [ -n "$(git -C "$project_root" status --porcelain --untracked-files=all)" ]; then
    echo "source archive requires a clean Git worktree" >&2
    exit 1
fi

version=$(sed -n '1p' "$project_root/VERSION")
archive_name="tryx-panorama-manager-$version.tar.xz"
archive_path=$output_dir/$archive_name
temporary_path=$archive_path.tmp

mkdir -p "$output_dir"
git -C "$project_root" archive \
    --format=tar \
    --prefix="tryx-panorama-manager-$version/" \
    HEAD |
    xz -T1 -9 --check=crc64 > "$temporary_path"
mv "$temporary_path" "$archive_path"

sha256sum "$archive_path"
