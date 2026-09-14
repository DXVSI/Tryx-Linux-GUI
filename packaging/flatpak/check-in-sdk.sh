#!/bin/sh
# Executed by flatpak-builder in its build sandbox, before cleanup/export.
set -eu

test -f /.flatpak-info
test ! -e /dev/bus/usb
test ! -e /run/host/dev/bus/usb
test -x /app/test-tools/bin/dbus-run-session

# CI's root build sandbox can otherwise write through chmod-based fixtures.
# Keep the build UID/ownership, but remove DAC bypass for this test process tree.
# Non-root builds can have these bounding bits without any effective privilege.
cap_bounding=$(awk '$1 == "CapBnd:" { print $2 }' /proc/self/status)
test -n "$cap_bounding"
if [ "$(id -u)" -eq 0 ] && [ "$((0x$cap_bounding & 6))" -ne 0 ]; then
    command -v capsh >/dev/null || {
        echo "Flatpak SDK checks require capsh to drop root DAC bypass" >&2
        exit 1
    }
    exec capsh --drop=cap_dac_override,cap_dac_read_search --inh= --noamb \
        -- -c 'exec /bin/sh "$@"' tryx-sdk-check "$0" "$@"
fi
cap_effective=$(awk '$1 == "CapEff:" { print $2 }' /proc/self/status)
test -n "$cap_effective"
if [ "$((0x$cap_effective & 6))" -ne 0 ]; then
    echo "Flatpak SDK checks must not bypass filesystem permissions" >&2
    exit 1
fi

export PATH="/app/test-tools/bin:$PATH"
export LD_LIBRARY_PATH="/app/test-tools/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_CONTROLS_STYLE=Material
test_jobs=${FLATPAK_BUILDER_N_JOBS:-2}
case "$test_jobs" in ''|*[!0-9]*|0) exit 2 ;; esac
project_root=$(pwd)

# SDK lupdate lacks QML parsing. Native package CI checks source completeness;
# here require the tracked catalogue to compile without unfinished/invalid text.
lrelease -silent -fail-on-unfinished -fail-on-invalid \
    translations/tryx-panorama_ru.ts -qm build/flatpak-test-ru.qm
test -s build/flatpak-test-ru.qm

# No test below can contact the host bus or a real USB endpoint.
for suite in portalusb runtimeownership runtimebootstrap filechooser; do
    (
        cd tests/flatpak
        qmake "${suite}_tests.pro" -o "Makefile.$suite"
        make -f "Makefile.$suite" -j"$test_jobs"
    )
done
for executable in \
    build/portalusb-tests/portalusb-tests \
    build/flatpak-runtimeownership-tests/flatpak-runtimeownership-tests \
    build/flatpak-runtimebootstrap-tests/flatpak-runtimebootstrap-tests \
    build/flatpak-filechooser-tests/flatpak-filechooser-tests; do
    TRYX_FLATPAK_TEST_ISOLATED=1 dbus-run-session -- "$executable" -txt
done

(
    cd tests
    qmake printerprotocol_tests.pro CONFIG+=flatpak -o Makefile.flatpak-protocol
    make -f Makefile.flatpak-protocol -j"$test_jobs"
    dbus-run-session -- ../build/flatpak-protocol-tests/printerprotocol-tests \
        flatpakFirmwareIsBlockedBeforeValidationOrQuiesce \
        mediaPreparationKeepsRetryArtifactsPrivate -txt
    # Same source, unchanged native profile, full protocol regression suite.
    qmake printerprotocol_tests.pro -o Makefile.native-protocol
    make -f Makefile.native-protocol -j"$test_jobs"
    dbus-run-session -- ../build/tests/printerprotocol-tests -txt
)

# Existing Qt Quick/handshake/bootstrap/tray checks, on a private session bus.
dbus-run-session -- make -j"$test_jobs" -f Makefile.quick quick-check
(
    cd tests/quick
    qmake quick_tests.pro CONFIG+=document_portal_probe -o Makefile.document-probe
    make -f Makefile.document-probe -j"$test_jobs"
    # Offline fixture only. Real Documents grants are a separate opt-in probe.
    ../../build/documentportal-probe/documentportal-probe --fixture-only
)
(
    cd tests/cli
    qmake cli_tests.pro CONFIG+=flatpak -o Makefile.flatpak-cli
    make -f Makefile.flatpak-cli -j"$test_jobs"
    dbus-run-session -- ../../build/flatpak-cli-tests/tryx-cli-tests -txt
)

test -x /app/bin/tryx-panorama-manager
test -x /app/bin/tryx
test -x /app/lib/tryx-panorama-manager/tryx-panorama-runtime
for program in /app/bin/tryx-panorama-manager /app/bin/tryx \
    /app/lib/tryx-panorama-manager/tryx-panorama-runtime; do
    "$program" --version
    if ldd "$program" | grep -F 'not found'; then
        exit 1
    fi
done
test ! -e /app/lib/systemd/user/tryx-panorama.service
test ! -d /app/lib/udev
env -u DBUS_SESSION_BUS_ADDRESS /app/bin/tryx-panorama-manager --smoke-test
printf 'Flatpak SDK checks passed for %s\n' "$project_root"
