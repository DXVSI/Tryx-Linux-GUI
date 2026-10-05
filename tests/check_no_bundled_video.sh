#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if git -C "$project_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    candidates=$(
        git -C "$project_root" -c core.quotePath=false ls-files |
            grep -Ei '(^media/|^demo\.mp4$|\.(mp4|webm|mkv|avi|mov)$)' || true
    )
    forbidden=
    if [ -n "$candidates" ]; then
        while IFS= read -r relative_path; do
            if [ -n "$relative_path" ] &&
               [ -e "$project_root/$relative_path" ]; then
                forbidden="${forbidden}${relative_path}
"
            fi
        done <<EOF
$candidates
EOF
    fi
else
    forbidden=$(
        find "$project_root" \
            -path "$project_root/build" -prune -o \
            -type f \( \
                -iname '*.mp4' -o \
                -iname '*.webm' -o \
                -iname '*.mkv' -o \
                -iname '*.avi' -o \
                -iname '*.mov' \
            \) -print
    )
fi

if [ -n "$forbidden" ]; then
    printf '%s\n' "Bundled video files are not allowed:" "$forbidden" >&2
    exit 1
fi
