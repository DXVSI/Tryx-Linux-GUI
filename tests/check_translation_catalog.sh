#!/bin/sh
set -eu

project_root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)
catalog="$project_root/translations/tryx-panorama_ru.ts"

translation_source_validator='
BEGIN { $failed = 0 }
{
    my $trivia =
        qr{(?:\s+|/\*[\s\S]*?\*/|//[^\r\n]*(?:\r?\n|$))*};
    if (/::${trivia}translate\b/) {
        warn "raw qualified translate is forbidden: $ARGV\n";
        $failed = 1;
    }

    my $literal = qr/(?:u8)?"(?:\\.|[^"\\])*"/;
    my $token = qr/\bDeviceManagerMessages\b/;
    my $declaration =
        qr/\bclass${trivia}DeviceManagerMessages\b/;
    my $call =
        qr/\bDeviceManagerMessages${trivia}::${trivia}tr${trivia}\(/;
    my $extractable =
        qr/$call${trivia}$literal(?:${trivia}$literal)*${trivia}\)/;
    my $token_count = () = /$token/g;
    my $declaration_count = () = /$declaration/g;
    my $call_count = () = /$call/g;
    my $extractable_count = () = /$extractable/g;
    if ($token_count != $call_count + $declaration_count ||
        $call_count != $extractable_count) {
        warn "DeviceManagerMessages::tr requires only direct string literals: $ARGV\n";
        $failed = 1;
    }
}
END { exit($failed ? 1 : 0) }
'

if ! find "$project_root/src" -type f \
        \( -name '*.cpp' -o -name '*.h' \) -print0 |
        xargs -0 -r perl -0777 -ne "$translation_source_validator"; then
    echo "DeviceManager translation source is hidden from lupdate" >&2
    exit 1
fi

if printf '%s\n' \
        'QCoreApplication::translate(' \
        '    "DeviceManager", hiddenSource);' |
        perl -0777 -ne "$translation_source_validator" \
            >/dev/null 2>&1; then
    echo "translation source checker accepted raw multiline translation" >&2
    exit 1
fi
if printf '%s\n' \
        'DeviceManagerMessages::tr(/* hidden */ hiddenSource);' |
        perl -0777 -ne "$translation_source_validator" \
            >/dev/null 2>&1; then
    echo "translation source checker accepted a hidden expression" >&2
    exit 1
fi
if printf '%s\n' \
        'QCoreApplication /* bridge */ :: translate(' \
        '    "DeviceManager", hiddenSource);' |
        perl -0777 -ne "$translation_source_validator" \
            >/dev/null 2>&1; then
    echo "translation source checker accepted token-comment raw translation" >&2
    exit 1
fi
if printf '%s\n' \
        'DeviceManagerMessages /* bridge */ :: tr(hiddenSource);' |
        perl -0777 -ne "$translation_source_validator" \
            >/dev/null 2>&1; then
    echo "translation source checker accepted token-comment hidden expression" >&2
    exit 1
fi
if ! printf '%s\n' \
        'DeviceManagerMessages::tr(u8"visible literal");' |
        perl -0777 -ne "$translation_source_validator" \
            >/dev/null 2>&1; then
    echo "translation source checker rejected an extractable u8 literal" >&2
    exit 1
fi

qmake_command=${QMAKE:-}
if [ -z "$qmake_command" ]; then
    if command -v qmake6 >/dev/null 2>&1; then
        qmake_command=$(command -v qmake6)
    elif command -v qmake >/dev/null 2>&1; then
        qmake_command=$(command -v qmake)
    else
        echo "qmake was not found for the translation catalog check" >&2
        exit 1
    fi
fi

qt_host_bins=$($qmake_command -query QT_HOST_BINS)
lupdate="$qt_host_bins/lupdate-pro"
lrelease="$qt_host_bins/lrelease"
lconvert="$qt_host_bins/lconvert"
for tool in "$lupdate" "$lrelease" "$lconvert"; do
    if [ ! -x "$tool" ]; then
        echo "required Qt translation tool is unavailable: $tool" >&2
        exit 1
    fi
done

temp_parent=${TMPDIR:-/tmp}
temp_parent=${temp_parent%/}
temp_dir=$(mktemp -d "$temp_parent/tryx-translation-check.XXXXXX")
cleanup() {
    if [ -d "$temp_dir" ]; then
        case "$temp_dir" in
            "$temp_parent"/tryx-translation-check.*)
                rm -r -- "$temp_dir"
                ;;
            *)
                echo "refusing to remove unexpected translation check path: $temp_dir" >&2
                ;;
        esac
    fi
}
trap cleanup EXIT HUP INT TERM

verify_catalog() {
    source_catalog=$1
    output_prefix=$2

    "$lrelease" -silent -fail-on-unfinished -fail-on-invalid \
        "$source_catalog" -qm "$output_prefix.qm" || return 1
    "$lconvert" -i "$output_prefix.qm" -o "$output_prefix-active.ts" ||
        return 1
    "$lupdate" \
        "$project_root/tryx-panorama.pro" \
        "$project_root/tryx-panorama-quick.pro" \
        -locations none -no-obsolete -silent \
        -ts "$output_prefix-active.ts" || return 1
    "$lrelease" -silent -fail-on-unfinished -fail-on-invalid \
        "$output_prefix-active.ts" \
        -qm "$output_prefix-current.qm"
}

if ! verify_catalog "$catalog" "$temp_dir/tracked"; then
    echo "translation catalog does not contain a finished translation for every current source tuple" >&2
    exit 1
fi

awk '
    /<source>Start TRYX Panorama Manager when you sign in<\/source>/ {
        target = 1
    }
    target && /<translation>/ {
        sub(/<translation>/, "<translation type=\"vanished\">")
        target = 0
        changed++
    }
    { print }
    END { if (changed != 1) exit 2 }
' "$catalog" > "$temp_dir/vanished-current.ts"

if verify_catalog \
    "$temp_dir/vanished-current.ts" \
    "$temp_dir/vanished-current" >/dev/null 2>&1; then
    echo "translation completeness checker accepted a current vanished tuple" >&2
    exit 1
fi

echo "translation catalog completeness verified"
