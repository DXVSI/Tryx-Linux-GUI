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
if [ ! -x "$lupdate" ]; then
    qt_host_libexecs=$($qmake_command -query QT_HOST_LIBEXECS)
    lupdate="$qt_host_libexecs/lupdate-pro"
fi
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

verify_finished_only_catalog() {
    input_catalog=$1
    guard_prefix=$2

    # QPH retains every non-Finished message, including empty obsolete forms.
    # Reject converter diagnostics too: duplicate removal precedes filtering.
    "$lconvert" -no-finished -of qph -i "$input_catalog" \
        -o "$guard_prefix.qph" > "$guard_prefix.log" 2>&1 || return 1
    [ ! -s "$guard_prefix.log" ] || return 1
    awk '
        NR == 1 && $0 == "<!DOCTYPE QPH>" { next }
        NR == 2 && /^<QPH( [^<>]*)?>$/ { next }
        NR == 3 && $0 == "</QPH>" { next }
        { failed = 1 }
        END { exit(failed || NR != 3) }
    ' "$guard_prefix.qph"
}

release_finished_catalog() {
    finished_input=$1
    finished_output=$2
    verify_finished_only_catalog "$finished_input" \
        "$finished_output-guard" || return 1
    "$lrelease" -silent "$finished_input" -qm "$finished_output" \
        > "$finished_output.log" 2>&1 || return 1
    [ ! -s "$finished_output.log" ] || return 1
    # Qt 6.4 can return success after a failed QDataStream write.
    [ -f "$finished_output" ] && [ -s "$finished_output" ] || return 1
    "$lconvert" -i "$finished_output" -of ts \
        -o "$finished_output-verified.ts" \
        > "$finished_output-verify.log" 2>&1 || return 1
    [ ! -s "$finished_output-verify.log" ]
}

check_finished_only_fixture() {
    fixture_name=$1
    expected_status=$2
    fixture_messages=$3
    fixture_path="$temp_dir/$fixture_name.ts"
    printf '%s\n' \
        '<TS version="2.1" language="ru_RU"><context><name>GuardFixture</name>' \
        "$fixture_messages" '</context></TS>' > "$fixture_path"
    actual_status=0
    release_finished_catalog "$fixture_path" \
        "$temp_dir/$fixture_name.qm" || actual_status=1
    if [ "$actual_status" -ne "$expected_status" ]; then
        echo "finished-only catalog guard failed fixture: $fixture_name" >&2
        exit 1
    fi
}

check_finished_only_fixture finished 0 \
    '<message><source>Visible &lt;phrase&gt;</source><translation>Текст &lt;phrase&gt;</translation></message>'
check_finished_only_fixture unfinished 1 \
    '<message><source>Visible</source><translation type="unfinished">Текст</translation></message>'
check_finished_only_fixture vanished 1 \
    '<message><source>Visible</source><translation type="vanished">Текст</translation></message>'
check_finished_only_fixture empty_obsolete_plural 1 \
    '<message numerus="yes"><source>%n item</source><translation type="obsolete"><numerusform></numerusform><numerusform>Invalid %1</numerusform><numerusform>%n</numerusform></translation></message>'
check_finished_only_fixture excess_finished_plural 1 \
    '<message numerus="yes"><source>%n item</source><translation><numerusform>%n a</numerusform><numerusform>%n b</numerusform><numerusform>%n c</numerusform><numerusform>%n extra</numerusform></translation></message>'
check_finished_only_fixture duplicate_unfinished 1 \
    '<message><source>Same</source><translation>Текст</translation></message><message><source>Same</source><translation type="unfinished">Текст</translation></message>'
check_finished_only_fixture malformed 1 '<message>'

ln -s /dev/full "$temp_dir/full-device.qm"
if release_finished_catalog "$temp_dir/finished.ts" \
    "$temp_dir/full-device.qm"; then
    echo "finished-only catalog guard accepted a failed QM write" >&2
    exit 1
fi

lrelease_help=$("$lrelease" -help)
strict_lrelease=false
if printf '%s\n' "$lrelease_help" | grep -Fq -- '-fail-on-unfinished' &&
        printf '%s\n' "$lrelease_help" | grep -Fq -- '-fail-on-invalid'; then
    strict_lrelease=true
fi
if [ "$strict_lrelease" = true ]; then
    echo "translation compiler guard: native strict options"
else
    echo "translation compiler guard: finished-only compatibility check"
fi

release_catalog() {
    release_input=$1
    release_output=$2
    if [ "$strict_lrelease" = true ]; then
        "$lrelease" -silent -fail-on-unfinished -fail-on-invalid \
            "$release_input" -qm "$release_output"
    else
        # Older tools accept only a warning-free, entirely Finished input.
        release_finished_catalog "$release_input" "$release_output"
    fi
}

verify_current_qm() {
    current_qm=$1
    current_prefix=$2
    "$lconvert" -i "$current_qm" -of ts \
        -o "$current_prefix.ts" > "$current_prefix.log" 2>&1 || return 1
    [ ! -s "$current_prefix.log" ] || return 1
    "$lupdate" \
        "$project_root/tryx-panorama.pro" \
        "$project_root/tryx-panorama-quick.pro" \
        -locations none -no-obsolete -silent \
        -ts "$current_prefix.ts" || return 1
    verify_finished_only_catalog "$current_prefix.ts" "$current_prefix-guard"
}

verify_catalog() {
    source_catalog=$1
    output_prefix=$2

    release_catalog "$source_catalog" "$output_prefix.qm" || return 1
    "$lconvert" -i "$output_prefix.qm" -o "$output_prefix-active.ts" ||
        return 1
    "$lupdate" \
        "$project_root/tryx-panorama.pro" \
        "$project_root/tryx-panorama-quick.pro" \
        -locations none -no-obsolete -silent \
        -ts "$output_prefix-active.ts" || return 1
    release_catalog "$output_prefix-active.ts" "$output_prefix-current.qm" ||
        return 1
    verify_current_qm "$output_prefix-current.qm" "$output_prefix-final"
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

awk '
    /<message>/ { message = $0 ORS; in_message = 1; next }
    in_message {
        message = message $0 ORS
        if (/<\/message>/) {
            if (message ~ /<source>Start TRYX Panorama Manager when you sign in<\/source>/)
                removed++
            else
                printf "%s", message
            in_message = 0
        }
        next
    }
    { print }
    END { if (removed != 1 || in_message) exit 2 }
' "$catalog" > "$temp_dir/missing-current.ts"

if verify_catalog \
    "$temp_dir/missing-current.ts" \
    "$temp_dir/missing-current" >/dev/null 2>&1; then
    echo "translation completeness checker accepted a missing current tuple" >&2
    exit 1
fi

dd if="$temp_dir/tracked-current.qm" of="$temp_dir/truncated-final.qm" \
    bs=16 count=1 2>/dev/null
if verify_current_qm "$temp_dir/truncated-final.qm" \
        "$temp_dir/truncated-final" >/dev/null 2>&1; then
    echo "translation completeness checker accepted a truncated final QM" >&2
    exit 1
fi

echo "translation catalog completeness verified"
