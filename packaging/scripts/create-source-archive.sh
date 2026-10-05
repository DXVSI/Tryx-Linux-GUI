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

worktree_status=$(
    git -C "$project_root" status --porcelain --untracked-files=all
)
if [ -n "$worktree_status" ]; then
    echo "source archive requires a clean Git worktree" >&2
    exit 1
fi

version=$(sed -n '1p' "$project_root/VERSION")
archive_name="tryx-panorama-manager-$version.tar.xz"
archive_path=$output_dir/$archive_name
temporary_path=$archive_path.tmp
temporary_tar=$archive_path.tar.tmp

mkdir -p "$output_dir"
cleanup() {
    rm -f -- "$temporary_path" "$temporary_tar"
}
trap cleanup 0 1 2 15

git -C "$project_root" archive \
    --format=tar \
    --prefix="tryx-panorama-manager-$version/" \
    --output="$temporary_tar" \
    HEAD

archive_members=$(tar -tf "$temporary_tar")
while IFS= read -r required_cli_member; do
    archive_member="tryx-panorama-manager-$version/$required_cli_member"
    if ! printf '%s\n' "$archive_members" |
        grep -Fqx "$archive_member"; then
        echo "source archive is missing CLI member: $required_cli_member" >&2
        exit 1
    fi
done <<'EOF'
tryx-cli.pro
src/cli/main.cpp
src/cli/sessionbusguard.cpp
src/cli/sessionbusguard.h
src/cli/tryxclirunner.cpp
src/cli/tryxclirunner.h
tests/cli/cli_tests.cpp
tests/cli/cli_tests.pro
packaging/tryx.1
EOF

xz -T1 -9 --check=crc64 --stdout "$temporary_tar" > "$temporary_path"
mv "$temporary_path" "$archive_path"
rm -f -- "$temporary_tar"
trap - 0 1 2 15

sha256sum "$archive_path"
