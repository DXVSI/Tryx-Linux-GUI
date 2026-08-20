#!/bin/sh
set -eu

project_root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)

require_tests() {
    suite_name=$1
    test_binary=$2
    isolated_dbus=${3:-false}

    if [ ! -x "$test_binary" ]; then
        printf 'Baseline suite %s is not built: %s\n' \
            "$suite_name" "$test_binary" >&2
        return 1
    fi

    if [ "$isolated_dbus" = true ]; then
        registered_tests=$(
            TRYX_RUNTIMEBOOTSTRAP_TEST_ISOLATED=1 \
                dbus-run-session -- \
                env QT_QPA_PLATFORM=offscreen \
                "$test_binary" -functions
        )
    else
        registered_tests=$(
            QT_QPA_PLATFORM=offscreen \
                "$test_binary" -functions
        )
    fi

    missing_tests=
    while IFS= read -r test_name; do
        case "$test_name" in
            ''|'#'*)
                continue
                ;;
        esac
        if ! printf '%s\n' "$registered_tests" |
            grep -Fqx "${test_name}()"; then
            missing_tests="${missing_tests}${test_name}\n"
        fi
    done

    if [ -n "$missing_tests" ]; then
        printf 'Baseline suite %s lost required tests:\n%b' \
            "$suite_name" "$missing_tests" >&2
        return 1
    fi

    printf 'Baseline suite %s verified\n' "$suite_name"
}

require_tests \
    printer-protocol \
    "$project_root/build/tests/printerprotocol-tests" <<'EOF'
runtimeOperationDbusRoundTrip
runtimeMetricsDbusRoundTrip
productProfilesExposeExactCapabilities
samePathReenumerationCancelsOldGeneration
productChangeDoesNotReuseSessionOrRecovery
generationChangeWaitsForStructuredApplyOutcome
finalizationUnknownAutoReconcilesWithoutRetransmission
finalizationUnknownRestartAutoReconcilesWithoutRetry
deleteReconcileOnlyNeverDispatchesFileRemove
turrisLostFinalAckDoesNotReconcileOrRetransmit
firmwareExclusiveGateRejectsDeviceWork
firmwareReleaseFenceWaitsForLateQuiesce
persistentUsbInputFailureStopsSameGenerationWithoutRecovery
partialUploadRequiresObservedDeviceRemovalBeforeRetry
legacyUnknownOutcomeRequiresObservedRecovery
EOF

require_tests \
    quick-client \
    "$project_root/build/quick-tests/tryx-quick-tests" <<'EOF'
legacyScreenConfigKeepsManager1Shape
operationsRejectStaleEvents
operationAcknowledgementRequiresExactIdentity
deviceMediaWorkflowInvalidationStopsReplaceMutations
windowChromeHidesAndRestoresOnlyWithTray
EOF

require_tests \
    runtime-bootstrap \
    "$project_root/build/runtimebootstrap-tests/quick/tryx-runtimebootstrap-tests" \
    true <<'EOF'
ownerChangeDuringApiProbeFailsClosed
activeIncompatibleOwnerIsNotRestarted
installedModeDoesNotRestartAnActiveOperation
invalidBuildRuntimeDoesNotFallBackToSystemd
EOF

require_tests \
    linux-tray \
    "$project_root/build/linuxtray-tests/linuxtraycontroller-tests" <<'EOF'
watcherLifecycleAndActions
trayQuitTerminatesGuiEventLoop
noWatcherFallsBackToUnavailable
EOF

require_tests \
    replace-journal \
    "$project_root/build/replacejournal-tests/replacejournal-tests" <<'EOF'
malformedAndUnexpectedFieldsFailClosed
unsafeFilesystemEntriesFailClosed
transitionsAreMonotonic
EOF

printf '%s\n' 'Runtime refactor characterization baseline verified'
