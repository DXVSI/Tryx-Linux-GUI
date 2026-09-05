#!/bin/sh
set -eu

project_root=$(unset CDPATH; cd -- "$(dirname -- "$0")/.." && pwd)

require_source_pattern() {
    relative_path=$1
    pattern=$2
    source_path="$project_root/$relative_path"

    if [ ! -f "$source_path" ] ||
        ! grep -Fq "$pattern" "$source_path"; then
        printf 'Runtime refactor source invariant is missing: %s: %s\n' \
            "$relative_path" "$pattern" >&2
        return 1
    fi
}

reject_source_pattern() {
    relative_path=$1
    pattern=$2
    source_path="$project_root/$relative_path"

    if [ -f "$source_path" ] &&
        grep -Fq "$pattern" "$source_path"; then
        printf 'Runtime refactor source invariant is stale: %s: %s\n' \
            "$relative_path" "$pattern" >&2
        return 1
    fi
}

require_source_pattern \
    src/printersessioncontroller.h \
    'class PrinterSessionController final : public QObject'
require_source_pattern \
    src/printersessioncontroller.h \
    'const State &state() const'
require_source_pattern \
    src/devicemanager.h \
    'PrinterSessionController sessionController_;'
require_source_pattern \
    tryx-panorama.pro \
    'src/printersessioncontroller.h'
require_source_pattern \
    tryx-panorama.pro \
    'src/printersessioncontroller.cpp'
require_source_pattern \
    tests/printerprotocol_tests.pro \
    '$$PWD/../src/printersessioncontroller.cpp'
require_source_pattern \
    tests/printerprotocol_tests.pro \
    '$$PWD/../src/printersessioncontroller.h'
reject_source_pattern \
    src/devicemanager.cpp \
    'sessionController_.state_'
for session_field in \
    printerSnapshot_ printerGeneration_ printerClassConnected_ \
    printerDisplaySessionActive_ printerDisplaySessionLost_ \
    printerRecoveryRequired_ printerRecoveryRemovalObserved_ \
    firmwareExclusiveLeaseId_ firmwareReleasePendingLeaseId_ \
    firmwareQuiesceGeneration_ deviceSpecificationsCache_ \
    metricsState_ displayState_; do
    reject_source_pattern src/devicemanager.h "$session_field"
    reject_source_pattern src/devicemanager.cpp "$session_field"
done
reject_source_pattern \
    src/devicemanager.h \
    'paseMetricsConfigStore_'

require_source_pattern \
    src/printermediapreparer.h \
    'class PrinterMediaPreparer : public QObject'
require_source_pattern \
    src/printermediapreparer.cpp \
    'PrinterMediaPreparer::PrinterMediaPreparer'
reject_source_pattern \
    src/devicemanager.h \
    'class PrinterMediaPreparer : public QObject'
reject_source_pattern \
    src/devicemanager.cpp \
    'PrinterMediaPreparer::PrinterMediaPreparer'

require_source_pattern \
    src/printeroperationcoordinator.h \
    'class PrinterOperationCoordinator final : public QObject'
require_source_pattern \
    src/printeroperationcoordinator.cpp \
    'PrinterOperationCoordinator::PrinterOperationCoordinator'
require_source_pattern \
    tryx-panorama.pro \
    'src/printeroperationcoordinator.h'
require_source_pattern \
    tryx-panorama.pro \
    'src/printeroperationcoordinator.cpp'
require_source_pattern \
    tests/printerprotocol_tests.pro \
    '$$PWD/../src/printeroperationcoordinator.h'
require_source_pattern \
    tests/printerprotocol_tests.pro \
    '$$PWD/../src/printeroperationcoordinator.cpp'
reject_source_pattern \
    src/devicemanager.h \
    'struct OperationRecord'
reject_source_pattern \
    src/devicemanager.h \
    'operations_'
reject_source_pattern \
    src/devicemanager.h \
    'operationOrder_'
reject_source_pattern \
    src/devicemanager.h \
    'activeOperationId_'
reject_source_pattern \
    src/devicemanager.h \
    'operationRevision_'
reject_source_pattern \
    src/devicemanager.cpp \
    'operationCoordinator_.operations_'
reject_source_pattern \
    src/devicemanager.cpp \
    'operationCoordinator_.operationOrder_'
reject_source_pattern \
    src/devicemanager.cpp \
    'operationCoordinator_.activeOperationId_'
reject_source_pattern \
    src/devicemanager.cpp \
    'operationCoordinator_.retryCacheSnapshot_'
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::finishOperation('
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::publishOperation('

require_source_pattern \
    src/turrismediaformat.h \
    'namespace tryx::turris_media'
require_source_pattern \
    src/turrismediaformat.cpp \
    'WriteResult writeBlob('
reject_source_pattern \
    src/devicemanager.cpp \
    'TurrisMediaBlobResult writeTurrisMediaBlob('
reject_source_pattern \
    src/printerprotocol.cpp \
    'bool validateTurrisMediaBlob('

require_source_pattern \
    src/printermediafileintegrity.h \
    'namespace tryx::printer_media_file_integrity'
require_source_pattern \
    src/printermediafileintegrity.cpp \
    'SafeSourceHashResult hashRegularSourceFile('
require_source_pattern \
    src/printermediaidentity.h \
    'namespace tryx::printer_media_identity'
require_source_pattern \
    src/printermediaidentity.cpp \
    'QString printerConversionProfile('
reject_source_pattern \
    src/devicemanager.cpp \
    '#include "printermediapreparersupport_p.h"'
reject_source_pattern \
    src/printermediapreparer.cpp \
    '#include "printermediapreparersupport_p.h"'
require_source_pattern \
    src/printermediavalidator.h \
    'namespace tryx::printer_media_validator'
require_source_pattern \
    src/printermediavalidator.cpp \
    'bool validateRecoveredH264('
reject_source_pattern \
    src/devicemanager.cpp \
    'bool validateRecoveredH264('
require_source_pattern \
    src/paseoverlayconfig.h \
    'namespace tryx::pase_overlay_config'
require_source_pattern \
    src/paseoverlayconfig.cpp \
    'PrinterProtocol::PaseOverlayConfig paseOverlayFromApplyRequest('
require_source_pattern \
    src/runtimeapplyrequestcodec.h \
    'namespace tryx::runtime_apply_request_codec'
require_source_pattern \
    src/runtimeapplyrequestcodec.cpp \
    'QJsonObject runtimeApplyRequestToJson('
reject_source_pattern \
    src/runtimeapplyrequestcodec.h \
    'stringListToJson'
reject_source_pattern \
    src/devicemanager.cpp \
    'QJsonObject runtimeApplyRequestToJson('
require_source_pattern \
    src/privateruntimepaths.h \
    'namespace tryx::private_runtime_paths'
require_source_pattern \
    src/privateruntimepaths.cpp \
    'bool atomicRenameNoReplace('
reject_source_pattern \
    src/devicemanager.cpp \
    'bool ensurePrivateDirectory('
require_source_pattern \
    src/mediacatalogstore.h \
    'class MediaCatalogStore'
require_source_pattern \
    src/mediacatalogstore.cpp \
    'MediaCatalogStore::LoadResult MediaCatalogStore::load()'
reject_source_pattern \
    src/devicemanager.h \
    'QJsonObject mediaCatalogIndex_'
reject_source_pattern \
    src/devicemanager.h \
    'bool mediaCatalogWriteEnabled_'
require_source_pattern \
    src/pasemetricsconfigstore.h \
    'class PaseMetricsConfigStore final'
require_source_pattern \
    src/pasemetricsconfigstore.cpp \
    'PaseMetricsConfigStore::LoadResult PaseMetricsConfigStore::load()'
reject_source_pattern \
    src/devicemanager.h \
    'QString persistedPaseMetricsSerial_'
reject_source_pattern \
    src/devicemanager.h \
    'PrinterProtocol::PaseOverlayConfig persistedPaseOverlay_'
require_source_pattern \
    src/devicemediaartifactstore.h \
    'class DeviceMediaArtifactStore final'
require_source_pattern \
    src/devicemediaartifactstore.cpp \
    'DeviceMediaArtifactStore::reserve('
reject_source_pattern \
    src/devicemanager.h \
    'struct DeviceMediaArtifactRecord'
reject_source_pattern \
    src/devicemanager.h \
    'deviceMediaArtifacts_'
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::validateArtifactRecord'
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::removeDeviceMediaArtifact'
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::ensureDeviceMediaOutbox'
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::isValidDbusUniqueName'
require_source_pattern \
    src/deleteintentstore.h \
    'class DeleteIntentStore final'
require_source_pattern \
    src/deleteintentstore.cpp \
    'DeleteIntentStore::LoadResult DeleteIntentStore::load() const'
require_source_pattern \
    src/deleteintentstore.cpp \
    'DeleteIntentStore::MutationResult DeleteIntentStore::write('
reject_source_pattern \
    src/devicemanager.cpp \
    'kDeleteIntentFormatVersion'
reject_source_pattern \
    src/devicemanager.cpp \
    'writeJsonObjectAtomically('
require_source_pattern \
    src/printeroperationcoordinator.cpp \
    'PrinterOperationCoordinator::dispatchPreparedUploadWithRetryBarrier('
reject_source_pattern \
    src/devicemanager.cpp \
    'DeviceManager::dispatchPreparedUploadWithRetryBarrier('
require_source_pattern \
    src/retrycachetransitionstore.h \
    'class RetryCacheTransitionStore final'
require_source_pattern \
    src/retrycachetransitionstore.cpp \
    'RetryCacheTransitionStore::protect('
require_source_pattern \
    src/retrycachestore.h \
    'class RetryCacheStore final'
require_source_pattern \
    src/retrycachestore.cpp \
    'RetryCacheStore::LoadResult RetryCacheStore::load()'
reject_source_pattern \
    src/devicemanager.h \
    'SuspendedRetryCacheState'
reject_source_pattern \
    src/devicemanager.h \
    'suspendedRetryCache_'
reject_source_pattern \
    src/devicemanager.cpp \
    'writeRetryCache('
reject_source_pattern \
    src/devicemanager.cpp \
    'retryCacheManifestPath('
reject_source_pattern \
    src/devicemanager.cpp \
    'clearRetryCache('
reject_source_pattern \
    src/devicemanager.cpp \
    'consumeAcknowledgedRetryCache('
reject_source_pattern \
    src/devicemanager.cpp \
    'preserveActivePreparedMediaForShutdown('
reject_source_pattern \
    src/devicemanager.cpp \
    'kRetryCacheFormatVersion'
reject_source_pattern \
    src/devicemanager.cpp \
    'kRetryCacheStrictApplyVersion'
reject_source_pattern \
    src/devicemanager.cpp \
    'kMaxRetryManifestBytes'

require_source_pattern \
    src/quick/main.cpp \
    '&WindowChromeController::requestExplicitQuit'
require_source_pattern \
    src/quick/main.cpp \
    '&WindowChromeController::explicitQuitApproved'
require_source_pattern \
    tests/quick/qml/tst_mainlifecycleguard.qml \
    'test_approvedCloseCannotBypassLaterDirtyState'
reject_source_pattern \
    qml/pages/PanoramaPage.qml \
    'root.runtime.setBrightness('
reject_source_pattern \
    qml/pages/PanoramaPage.qml \
    'root.runtime.setOrientation('
require_source_pattern \
    qml/pages/PanoramaPage.qml \
    'runtime.submitSavedLayoutDraft('
require_source_pattern \
    src/quick/runtimeclient.cpp \
    'QStringLiteral("QueueSavedLayoutApplyV1")'
require_source_pattern \
    src/devicemanager.cpp \
    'QStringLiteral("VerifyingSavedLayout")'
require_source_pattern \
    src/savedlayoutstore.cpp \
    'SavedLayoutStore::MutationResult SavedLayoutStore::put('
require_source_pattern \
    src/runtimecontract.cpp \
    'QString tryxRuntimeCacheCleanupV1Token()'
require_source_pattern \
    src/runtimebridge.h \
    'QString QueueCacheCleanupV1(const QString &operationId);'
require_source_pattern \
    src/quick/cachemanagementcontroller.h \
    'class CacheManagementController final : public QObject'
require_source_pattern \
    src/mediacatalogstore.cpp \
    'MediaCatalogStore::planThumbnailOrphanCleanup('
require_source_pattern \
    src/devicemediaartifactstore.cpp \
    'DeviceMediaArtifactStore::cleanupAssessment('
reject_source_pattern \
    src/quick/mediapreviewcontroller.cpp \
    'removeRecursively'

tray_quit_wiring=$(
    awk '
        /&tray, &LinuxTrayController::quitRequested/ {
            for (line = 0; line < 6; ++line) {
                print
                if (getline <= 0)
                    break
            }
        }
    ' "$project_root/src/quick/main.cpp"
)
case "$tray_quit_wiring" in
    *'&WindowChromeController::requestExplicitQuit'*) ;;
    *)
        printf '%s\n' \
            'Tray Quit no longer routes through the guarded explicit-quit request' >&2
        exit 1
        ;;
esac
case "$tray_quit_wiring" in
    *'exit('*|*'QCoreApplication'*)
        printf '%s\n' \
            'Tray Quit is wired directly to process exit' >&2
        exit 1
        ;;
esac
upload_dispatch_emit_count=$(
    grep -Fc 'emit requestUploadPrepared' \
        "$project_root/src/printeroperationcoordinator.cpp" || true
)
if [ "$upload_dispatch_emit_count" -ne 1 ]; then
    printf '%s\n' \
        'Upload dispatch must have exactly one barrier-protected emit site' \
        >&2
    exit 1
fi
reject_source_pattern \
    src/devicemanager.h \
    'QJsonObject pendingDeleteIntent_'

reject_source_pattern \
    src/quick/appsettingscontroller.cpp \
    'systemctl'
reject_source_pattern \
    src/quick/appsettingscontroller.cpp \
    'tryx-panorama.service'
reject_source_pattern \
    src/quick/guiautostart.cpp \
    'systemctl'
reject_source_pattern \
    src/quick/guiautostart.cpp \
    'tryx-panorama.service'
require_source_pattern \
    src/quick/guiautostart.cpp \
    'openPinnedAutostartDirectory'
require_source_pattern \
    src/quick/guiautostart.cpp \
    'RENAME_NOREPLACE'
require_source_pattern \
    src/quick/guiautostart.cpp \
    'RENAME_EXCHANGE'
require_source_pattern \
    src/quick/guiautostart.cpp \
    'discardPreparedEntryIfMatches'
require_source_pattern \
    src/quick/guiautostart.cpp \
    'gAfterLeafPreconditionCheckHook'
reject_source_pattern \
    src/quick/guiautostart.cpp \
    'QSaveFile'
require_source_pattern \
    src/quick/runtimebootstrap.cpp \
    'bindNativeUnixSocket'
require_source_pattern \
    src/quick/runtimebootstrap.cpp \
    'exchangeInReplacementSocket'
require_source_pattern \
    src/quick/runtimebootstrap.cpp \
    'descriptorRelativePath'
reject_source_pattern \
    src/quick/runtimebootstrap.cpp \
    'QLocalServer::removeServer'
require_source_pattern \
    src/quick/main.cpp \
    'QStringLiteral("--autostart")'
require_source_pattern \
    src/quick/main.cpp \
    'StartupVisibilityController startupVisibility'
require_source_pattern \
    src/quick/main.cpp \
    'startupVisibility.beginAutostart()'
require_source_pattern \
    src/quick/main.cpp \
    'drainPendingInstanceLaunchConnections'
require_source_pattern \
    src/quick/main.cpp \
    'instanceLaunchIntentRequestsWindow(intent)'
reject_source_pattern \
    src/quick/main.cpp \
    'autostartVisibilityPending'
reject_source_pattern \
    src/quick/main.cpp \
    'finishAutostartVisible'
require_source_pattern \
    qml/Main.qml \
    'visible: !quickSmokeTest && !autostartRequested'

require_source_pattern \
    src/runtimecontract.cpp \
    'runtime.support-snapshot.v1'
require_source_pattern \
    src/runtimecontract.cpp \
    'runtime.media-preparation-profile.v1'
require_source_pattern \
    src/runtimecontract.cpp \
    'device.media-split-area.v1'
require_source_pattern \
    src/runtimebridge.h \
    'QueueUploadWithPreparationProfileV1('
require_source_pattern \
    src/runtimebridge.h \
    'QueueRecoveredMediaUploadWithPreparationProfileV1('
require_source_pattern \
    src/runtimebridge.h \
    'QueueReplaceDeviceMediaWithPreparationProfileV1('
require_source_pattern \
    src/runtimebridge.h \
    'QString GetSupportSnapshotV1() const;'
require_source_pattern \
    src/supportsnapshot.cpp \
    'QString buildSupportSnapshotV1('
require_source_pattern \
    src/quick/supportbundle.cpp \
    'RENAME_NOREPLACE'
require_source_pattern \
    src/quick/supportbundle.cpp \
    'O_NOFOLLOW'
require_source_pattern \
    src/quick/supportbundle.cpp \
    'openDirectoryWithoutSymlinks'
require_source_pattern \
    resources/quick.qrc \
    'qml/components/SupportBundleExportPicker.qml'
reject_source_pattern \
    src/supportsnapshot.cpp \
    'journalctl'
reject_source_pattern \
    src/supportsnapshot.cpp \
    'qgetenv('
reject_source_pattern \
    src/quick/supportbundle.cpp \
    'qgetenv('
reject_source_pattern \
    src/quick/supportbundle.cpp \
    'QProcessEnvironment'
reject_source_pattern \
    src/quick/supportbundle.cpp \
    'QSaveFile'

require_source_pattern_count() {
    relative_path=$1
    pattern=$2
    expected_count=$3
    source_path="$project_root/$relative_path"

    actual_count=0
    if [ -f "$source_path" ]; then
        actual_count=$(grep -Fc "$pattern" "$source_path" || :)
    fi
    if [ "$actual_count" -ne "$expected_count" ]; then
        printf 'Runtime refactor source invariant count differs: %s: %s: expected %s, got %s\n' \
            "$relative_path" "$pattern" "$expected_count" \
            "$actual_count" >&2
        return 1
    fi
}

require_source_pattern \
    packaging/tryx.1 \
    '.TH TRYX 1'
require_source_pattern \
    packaging/tryx.1 \
    'tryx support-report'
require_source_pattern \
    README.md \
    "\`/usr/bin/tryx\`"
require_source_pattern \
    packaging/rpm/tryx-panorama-manager.spec \
    '%{_bindir}/tryx'
require_source_pattern \
    packaging/rpm/tryx-panorama-manager.spec \
    '%{_mandir}/man1/tryx.1*'
require_source_pattern \
    debian/clean \
    'Makefile.cli'
require_source_pattern \
    debian/clean \
    'tests/cli/target_wrapper.sh'
require_source_pattern \
    .gitignore \
    'Makefile.cli'
require_source_pattern \
    .gitignore \
    'tests/cli/target_wrapper.sh'
require_source_pattern \
    packaging/scripts/verify-package-contents.sh \
    'require_file /usr/bin/tryx'
require_source_pattern \
    packaging/scripts/verify-package-contents.sh \
    "\$staged_root/usr/bin/tryx-panorama-cli"
require_source_pattern \
    packaging/scripts/verify-package-contents.sh \
    'libQt6(Widgets|Qml|Quick)'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'tryx-cli.pro'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'src/cli/main.cpp'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'src/cli/sessionbusguard.cpp'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'src/cli/sessionbusguard.h'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'src/cli/tryxclirunner.cpp'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'tests/cli/cli_tests.cpp'
require_source_pattern \
    packaging/scripts/create-source-archive.sh \
    'packaging/tryx.1'
require_source_pattern \
    tryx-panorama-all.pro \
    '$(MAKE) -f Makefile.cli cli-check'
require_source_pattern \
    tryx-cli.pro \
    'packaging/tryx.1'
require_source_pattern \
    src/cli/main.cpp \
    'localSessionBusAddressIsSafe'
require_source_pattern \
    src/cli/sessionbusguard.cpp \
    "address.contains(';')"
require_source_pattern \
    src/cli/sessionbusguard.cpp \
    'O_NOFOLLOW'
require_source_pattern \
    src/cli/tryxclirunner.cpp \
    'codePoint == 0x061c'
require_source_pattern_count \
    .github/workflows/package-linux.yml \
    "test \"\$(tryx --version)\"" \
    6
require_source_pattern_count \
    .github/workflows/package-linux.yml \
    'for binary in tryx-panorama-manager tryx; do' \
    6
require_source_pattern_count \
    .github/workflows/package-linux.yml \
    'tryx --help >/dev/null' \
    6

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

require_qml_tests() {
    suite_name=$1
    test_file=$2
    component_name=$3
    translation_file=${4-}

    if [ ! -f "$test_file" ]; then
        printf 'Baseline QML suite %s is missing: %s\n' \
            "$suite_name" "$test_file" >&2
        return 1
    fi

    if [ -n "${QMLTESTRUNNER:-}" ]; then
        qml_test_runner=$QMLTESTRUNNER
    else
        if ! qt_host_bins=$(qmake6 -query QT_HOST_BINS); then
            printf 'Baseline QML suite %s cannot locate Qt host binaries\n' \
                "$suite_name" >&2
            return 1
        fi
        qml_test_runner=$qt_host_bins/qmltestrunner
    fi

    if [ ! -x "$qml_test_runner" ]; then
        printf 'Baseline QML suite %s runner is unavailable: %s\n' \
            "$suite_name" "$qml_test_runner" >&2
        return 1
    fi
    if [ -n "$translation_file" ] &&
        [ ! -f "$translation_file" ]; then
        printf 'Baseline QML suite %s translation is missing: %s\n' \
            "$suite_name" "$translation_file" >&2
        return 1
    fi

    failed_tests=
    while IFS= read -r test_name; do
        case "$test_name" in
            ''|'#'*)
                continue
                ;;
        esac
        if [ -n "$translation_file" ]; then
            test_output=$(
                QT_QPA_PLATFORM=offscreen \
                    QT_QUICK_CONTROLS_STYLE=Material \
                    "$qml_test_runner" \
                    -translation "$translation_file" \
                    -input "$test_file" \
                    -import "$project_root/qml" \
                    -o -,txt \
                    "${component_name}::${test_name}" 2>&1
            ) || test_status=$?
        else
            test_output=$(
                QT_QPA_PLATFORM=offscreen \
                    QT_QUICK_CONTROLS_STYLE=Material \
                    "$qml_test_runner" \
                    -input "$test_file" \
                    -import "$project_root/qml" \
                    -o -,txt \
                    "${component_name}::${test_name}" 2>&1
            ) || test_status=$?
        fi
        if [ "${test_status:-0}" -ne 0 ]; then
            printf '%s\n' "$test_output" >&2
            failed_tests="${failed_tests}${test_name}\n"
            unset test_status
            continue
        fi
        unset test_status
        if ! printf '%s\n' "$test_output" |
            grep -Fq \
                "PASS   : qmltestrunner::${component_name}::${test_name}"; then
            failed_tests="${failed_tests}${test_name}\n"
        fi
    done

    if [ -n "$failed_tests" ]; then
        printf 'Baseline QML suite %s lost or failed required tests:\n%b' \
            "$suite_name" "$failed_tests" >&2
        return 1
    fi

    printf 'Baseline QML suite %s verified\n' "$suite_name"
}

require_qml_tests \
    b2-home-presentation \
    "$project_root/tests/quick/qml/tst_homepagelayout.qml" \
    HomePageLayout <<'EOF'
test_temperatureFormattingComesFromRuntime
EOF

require_qml_tests \
    b2-settings-presentation \
    "$project_root/tests/quick/qml/tst_settingslayout.qml" \
    SettingsLayout <<'EOF'
test_presentationPreferencesUseConfirmedRuntimeState
EOF

require_qml_tests \
    b2-panorama-presentation \
    "$project_root/tests/quick/qml/tst_panoramalayout.qml" \
    PanoramaLayout <<'EOF'
test_dateAndTimeMetricUsesCanonicalToken
EOF

require_qml_tests \
    b3-settings-support-report \
    "$project_root/tests/quick/qml/tst_settingslayout.qml" \
    SettingsLayout <<'EOF'
test_supportBundleStatesRemainAccessible
EOF

require_qml_tests \
    b10-settings-device-specifications \
    "$project_root/tests/quick/qml/tst_settingslayout.qml" \
    SettingsLayout <<'EOF'
test_deviceSpecificationsStates
test_deviceSpecificationsReadyValuesAreSafeAndHonest
test_deviceSummaryStaysInsideNarrowContent
EOF

require_qml_tests \
    b10-settings-device-specifications-ru \
    "$project_root/tests/quick/qml/tst_settingslayout.qml" \
    SettingsLayout \
    "$project_root/build/quick/i18n/tryx-panorama_ru.qm" <<'EOF'
test_deviceSpecificationsRussianTranslation
EOF

require_qml_tests \
    b3-folder-only-export \
    "$project_root/tests/quick/qml/tst_supportbundleexportpickerlayout.qml" \
    SupportBundleExportPickerLayout <<'EOF'
test_folderOnlyExportAndAccessibility
test_busyDisablesSaveAndEscapeRestoresFocus
EOF

require_qml_tests \
    b9-home-gpu-telemetry \
    "$project_root/tests/quick/qml/tst_homepagelayout.qml" \
    HomePageLayout <<'EOF'
test_gpuTelemetryShowsPowerAndVramAvailability
test_metricsGridUsesResponsiveColumns
EOF

require_qml_tests \
    b9-dashboard-demand \
    "$project_root/tests/quick/qml/tst_mainlifecycleguard.qml" \
    MainLifecycleGuard <<'EOF'
test_dashboardSamplingFollowsVisibleHomePage
EOF

require_qml_tests \
    c15-metric-selector \
    "$project_root/tests/quick/qml/tst_metricselector.qml" \
    MetricSelector <<'EOF'
test_groupsCounterAndLocalizedSearch
test_fourthChoiceWaitsForExplicitPosition
test_cancelButtonAndProgrammaticCloseDoNotEdit
test_escapeCancelsAndRestoresOriginFocus
test_keyboardReplacementTraversesAndConfirmsExactPosition
test_selectedUnavailableRemainsVisibleAndRemovable
test_catalogUnavailableStillShowsAndRemovesSelection
test_narrowReplacementDialogBoundsLongLocalizedLabels
EOF

require_qml_tests \
    c15-metric-selector-ru \
    "$project_root/tests/quick/qml/tst_metricselector.qml" \
    MetricSelector \
    "$project_root/build/quick/i18n/tryx-panorama_ru.qm" <<'EOF'
test_groupsCounterAndLocalizedSearch
test_keyboardReplacementTraversesAndConfirmsExactPosition
test_narrowReplacementDialogBoundsLongLocalizedLabels
EOF

require_qml_tests \
    c15-panorama-integration \
    "$project_root/tests/quick/qml/tst_panoramalayout.qml" \
    PanoramaLayout <<'EOF'
test_metricSelectorsHaveIndependentCountersAndReflow
test_twoColumnControlsStayInsideContentBounds
test_pendingFourthMetricDoesNotChangeDraftOrQueuedApply
test_confirmedMetricReplacementUsesExactDraftPosition
test_metricDraftOverLimitIsBlockedInQml
test_availabilityLossPreservesDraftDirtyStateAndApplyToken
EOF

require_qml_tests \
    c3-panorama \
    "$project_root/tests/quick/qml/tst_panoramalayout.qml" \
    PanoramaLayout <<'EOF'
test_mediaMetadataUsesApi8Roles
test_mediaMetadataSizeBoundaries
test_mediaMetadataFallbacksAndLegacyBoundary
test_mediaCardMetadataFitsCompactWidthAndKeyboard
test_emptyMediaLibraryStateIsOriginNeutral
EOF

require_qml_tests \
    c12-media-editor-metadata \
    "$project_root/tests/quick/qml/tst_mediaeditorlayout.qml" \
    MediaEditorLayout <<'EOF'
test_recoveredCopyWarnsOnlyForNeutralGeometry
test_deviceCopyMetadataStates
test_deviceCopyMetadataIsHiddenForLocalMedia
test_deviceCopyMetadataFormatsLongDuration
test_recoveredResolutionUsesProbeNotFilenameOrTarget
test_deviceCopyMetadataIsNarrowAndAccessible
EOF

require_qml_tests \
    c12-media-editor-metadata-ru \
    "$project_root/tests/quick/qml/tst_mediaeditorlayout.qml" \
    MediaEditorLayout \
    "$project_root/build/quick/i18n/tryx-panorama_ru.qm" <<'EOF'
test_deviceCopyMetadataRussianTranslation
EOF

require_qml_tests \
    c4-media-editor-target \
    "$project_root/tests/quick/qml/tst_mediaeditorlayout.qml" \
    MediaEditorLayout <<'EOF'
test_preparationTargetSelectorUsesActualAreaCanvas
test_unavailableSplitTargetHasVisibleAccessibleReason
test_preparationTargetSelectorReflowsAtNarrowWidth
EOF

require_qml_tests \
    c4-media-editor-target-ru \
    "$project_root/tests/quick/qml/tst_mediaeditorlayout.qml" \
    MediaEditorLayout \
    "$project_root/build/quick/i18n/tryx-panorama_ru.qm" <<'EOF'
test_preparationTargetRussianTranslation
EOF

require_qml_tests \
    c5-saved-layouts-panel \
    "$project_root/tests/quick/qml/tst_savedlayoutspanel.qml" \
    SavedLayoutsPanel <<'EOF'
test_readyEmptyIsDistinctFromUnsupportedAndUnavailable
test_readyListReplacesEmptyState
test_nameIsBoundedAndExistingNameRequiresOverwrite
test_enterCannotBypassDisabledSaveAction
test_backgroundRefreshDoesNotStealFocus
test_dirtyLoadRequiresConfirmationAndEscapeRestoresFocus
test_mutationBusyDefersFocusRestoreUntilCompletion
test_confirmationCannotDispatchAfterRuntimeInvalidation
test_deleteRequiresConfirmationAndNeverLoadsOrDeletesMedia
test_narrowActionsReflowWithoutOverflowAndRemainAccessible
EOF

require_qml_tests \
    c5-saved-layouts-panel-ru \
    "$project_root/tests/quick/qml/tst_savedlayoutspanel.qml" \
    SavedLayoutsPanel \
    "$project_root/build/quick/i18n/tryx-panorama_ru.qm" <<'EOF'
test_savedLayoutsRussianTranslationAndNarrowAccessibility
EOF

require_qml_tests \
    c5-saved-layouts-panorama \
    "$project_root/tests/quick/qml/tst_panoramalayout.qml" \
    PanoramaLayout <<'EOF'
test_saveCurrentCapturesFullDraftWithoutApplyOrDirtyReset
test_loadFullAndSplitPreservesEveryOrderedFieldInDraft
test_editedLoadedDraftKeepsProvenanceForApply
test_bindingClearsOnlyOnExplicitLifecycleBoundaries
test_savedLayoutActionsFailClosedForApplyAndConflict
test_reconnectNeverLoadsOrAppliesSavedLayout
EOF

require_qml_tests \
    c6-temporary-files-panel \
    "$project_root/tests/quick/qml/tst_settingslayout.qml" \
    SettingsLayout <<'EOF'
test_temporaryFilesPanelIsSafeResponsiveAndAccessible
EOF

require_qml_tests \
    c6-temporary-files-panel-ru \
    "$project_root/tests/quick/qml/tst_settingslayout.qml" \
    SettingsLayout \
    "$project_root/build/quick/i18n/tryx-panorama_ru.qm" <<'EOF'
test_temporaryFilesRussianTranslation
EOF

require_qml_tests \
    c2-main-lifecycle \
    "$project_root/tests/quick/qml/tst_mainlifecycleguard.qml" \
    MainLifecycleGuard <<'EOF'
test_routeFirstIntentStayAndDiscardUseProductionMain
test_hideAndExplicitQuitWaitForDiscard
EOF

require_tests \
    b9-nvidia-provider \
    "$project_root/build/nvidiasmiprovider-tests/nvidiasmiprovider-tests" <<'EOF'
parsesZeroAndNormalizesIdentity
preservesIndependentUnavailableFields
supervisorIsolatesArgvEnvironmentAndDescriptors
supervisorDeadlineSurvivesBlockedSpawnAndFencesLatePid
providerInvalidatesOnTtl
providerBacksOffAndOpensBreaker
providerOffInvalidatesAndCancelsInflight
resolverValidatesEverySymlinkHopParent
inventoryUsesStablePrimaryPolicyAndIgnoresInputOrder
gpuSelectionPinEnrichesAndRebindsOnlyByExactUuid
inventoryClearsNvidiaTelemetryWithoutProviderSnapshot
EOF

require_tests \
    c4-runtime-downgrade-store \
    "$project_root/build/runtime-downgradestore-tests/runtime-downgradestore-tests" <<'EOF'
defaultDirectoryUsesGenericStateLocation
currentExecutableIdentityIsComplete
runtimeStartupGateBlocksExactExecutableBeforeDbus
runtimeStartupGateIgnoresDifferentExecutable
persistCreatesOwnerOnlyDurableMarker
fullFrameRoundTripIsExact
differentExecutableDoesNotBlockOrAbort
abortRemovesOnlyExactCurrentExecutableMarker
offlineAbortHashesTheInstalledMarkerExecutable
offlineAbortRejectsDifferentOrSymlinkExecutable
persistIsIdempotentButCannotChangeCurrentIntent
postRenameFailureReportsCommittedUnknownOutcome
aNewExecutableCanReplaceAValidStaleMarker
invalidInputDoesNotCreateMarker
corruptMarkersFailClosed
unsafeDirectoryAndMarkerFailClosed
abortRefusesCorruptOrUnsafeMarker
EOF

require_tests \
    c5-saved-layout-store \
    "$project_root/build/savedlayoutstore-tests/savedlayoutstore-tests" <<'EOF'
versionedRoundTripPreservesExactDeviceScopedDraft
otherDeviceLayoutsAreFilteredButNotDeleted
deleteRequiresExactDeviceAndProductScope
putUsesSnapshotCasAndCaseInsensitiveNames
updateAndDeleteAreAtomicAcrossRestart
recordRevisionAndDeviceScopeAreImmutable
limitsAndCanonicalSerializationAreEnforced
invalidLayoutNeverMutatesStore
malformedFutureOversizedAndUnsafeStateFailClosed
postCommitDirectoryRaceReportsUnknownAndDisablesWrites
EOF

require_tests \
    c6-cache-cleanup-store \
    "$project_root/build/cachecleanupstore-tests/cachecleanupstore-tests" <<'EOF'
catalogCleanupEmptySuccessIsExact
catalogCleanupPreservesAcceptedIndexAndReportsExactBytes
catalogCleanupFailsClosedForUnsafeCandidateAndBound
catalogCleanupRejectsHardlinkAndSpecialCandidates
catalogCleanupRequiresAcceptedLoadAndPinsParentIdentity
catalogCleanupRejectsChangedLeafIdentity
catalogCleanupReportsConfirmedMutationBeforeFsyncFailure
artifactCleanupEmptySuccessIsExact
artifactCleanupRequiresExpiryAndReportsExactBytes
artifactCleanupBlocksActiveLeaseAndPreservesHeldFile
artifactCleanupBlocksReservationAndOperationHold
artifactCleanupDefersOwnerDisconnectAndReportsExactIds
artifactCleanupAcceptsSymlinkLeafWithoutFollowing
artifactCleanupRejectsHardlinkAndSpecialLeaves
artifactCleanupRejectsChangedLeafIdentity
artifactCleanupBoundsPlanAndReportsRemovalFailures
EOF

require_tests \
    b12-headless-cli \
    "$project_root/build/cli-tests/tryx-cli-tests" \
    true <<'EOF'
versionAndHelpNeedNoBusOrDisplay
downgradePreparationFormatsHumanAndJson
noBusDowngradePreparationReturnsStableError
downgradePreparationRequiresCapability
downgradePreparationUnknownMethodIsUnsupported
downgradePreparationRefusalUsesFixedError
downgradePreparationRejectsInvalidReply
downgradePreparationRejectsInvalidSignature
downgradePreparationRequiresOwnerExit
downgradePreparationRejectsReplacementOwner
noBusAbortDowngradeReturnsStableError
abortDowngradeRejectsActiveOwnerWithoutTouchingMarker
abortDowngradeMissingMarkerIsBlocked
abortDowngradeRemovesExactOfflineMarker
invalidArgumentsUseFixedUsage
noBusStatusReturnsStableJsonError
localSessionBusGuardAcceptsCurrentIsolatedBus
unsafeSessionBusAddressesFailClosed
unsafeFilesystemSessionBusFailsClosed
unsetSessionBusDoesNotAutolaunch
unsafeOutputDirectoryControlsFailClosed
noOwnerStatusReturnsStableJsonError
hostOnlyReportWorksWithoutBus
hostOnlyReportWorksForUnsupportedRuntime
statusUsesValidatedSupportSnapshot
operationsUseBoundedSupportSnapshotTail
capabilitiesUseCanonicalAllowlistsAndContext
runtimeRequestTargetsExactOwnerWithoutAutostart
legacyUnknownMethodCapabilitiesSucceed
apiMismatchReturnsStableError
malformedSupportSnapshotFailsWithoutLeak
runtimeCapabilityErrorFailsWithoutLeak
invalidDeviceContextFailsClosed
advertisedSupportReportIncludesValidatedRuntimeSnapshot
invalidAdvertisedSupportReportCreatesNoFile
delayedSnapshotReplyAfterOwnerLossFailsClosed
shortInternalDeadlineReturnsTimeout
ownerLossBeforeSupportReportPublishCreatesNoFile
EOF

require_tests \
    printer-protocol \
    "$project_root/build/tests/printerprotocol-tests" <<'EOF'
runtimeOperationDbusRoundTrip
operationCoordinatorOwnsLedgerBehindSynchronousManagerFacade
sessionGenerationTransitionsPreserveEventBoundaries
sessionControllerPublishesThroughSynchronousManagerFacade
sessionTransitionStopsAfterReentrantFence
firmwareAcquireOrdersReentrantReleaseAfterQuiesce
sessionTeardownPreservesReentrantReconnect
runtimeMetricsDbusRoundTrip
runtimeMediaPreparationProfileDbusRoundTrip
runtimeDeviceCapabilitiesDbusRoundTrip
runtimeDeviceSpecificationsDbusRoundTrip
runtimeDeviceSpecificationsCacheIsGenerationBounded
runtimePresentationPreferencesDbusRoundTrip
runtimeSavedLayoutsDbusRoundTrip
supportSnapshotUsesExactAllowlistAndBounds
supportSnapshotRejectsNonStringScalars
supportLifecycleEventsUseTypedAllowlistAndBounds
supportSnapshotAdaptorUsesCachedStateWithoutIo
presentationPreferenceFormattingIsExact
runtimePresentationPreferencesStoreRoundTripsAndFailsClosed
runtimePresentationPreferencesRejectUnsafeModesAndDirectoryReplacement
runtimePresentationPreferencesUseCasAndPersistBeforePublish
presentationPreferenceWorkerCallbackCannotRollbackPublication
presentationPreferenceRevisionExhaustionFailsBeforePersistence
metricsCatalogTokensAndPaseGroupIdsRemainFrozen
paseFormatterRejectsInvalidOrFourthMetricBeforeTransport
wireCriticalGoldenFixtures
pasePresentationPreferencesReachInitialAndPeriodicOverlay
paseMetricBatchMatchesHeaderlessWireFrame
paseSchedulerAcceptsSplitAndDisplayOnly
nvidiaDemandFollowsCommittedGpuMetrics
restoredGpuOverlayKeepsExactIdentityAfterDelayedUuid
verificationFailureKeepsHealthySessionActive
runtimeCapabilitiesAreProfileBounded
runtimeCapabilityTokensAreStrictAndBounded
runtimeApi8ContractIsAdditive
savedLayoutsCrudIsDeviceScopedAndDispatchFree
savedLayoutDowngradeLatchBlocksCrudWithoutDispatch
savedLayoutQueueRejectsBeforeWorkerDispatch
savedLayoutQueuePreservesEditedDraftAndFreshProof
savedLayoutWorkerFreshFileListBoundary
runtimeDowngradeV10PreparationIsAtomicAndFailClosed
productProfilesExposeExactCapabilities
mediaTransformChangesConversionProfile
mediaTransformProfilePreventsOriginReuse
runtimePreparationProfileAdaptorPreservesSplitTarget
splitPreparationTargetRejectsTurrisBeforePreparation
pasePreparationProfilesProduceExactGeometry
udbSessionBootstrapDecodesDeviceSpecifications
udbSessionActivationFailureDiscardsDeviceSpecifications
turrisWorkerSessionSendsNoPaseTraffic
restoredOverlayWaitsForKeepaliveBeforeSessionReady
restoredOverlayFailureBecomesLostWithoutReplay
quickStagedSourceValidationAndLegacyBoundary
mediaRuntimeStartupCleanupIsBounded
samePathReenumerationCancelsOldGeneration
productChangeDoesNotReuseSessionOrRecovery
generationChangeWaitsForStructuredApplyOutcome
finalizationUnknownAutoReconcilesWithoutRetransmission
finalizationUnknownContinuesCompositeOperation
finalizationUnknownRestartReconcilesReadOnly
ambiguousFileListNeverConfirmsPreparedUpload
uploadDispatchBarrierRevalidatesAfterUploadingPublication
deleteReconcileOnlyNeverDispatchesFileRemove
turrisLostFinalAckDoesNotReconcileOrRetransmit
firmwareExclusiveGateRejectsDeviceWork
firmwareReleaseFenceWaitsForLateQuiesce
persistentUsbInputFailureStopsSameGenerationWithoutRecovery
partialUploadRequiresObservedDeviceRemovalBeforeRetry
retryCacheStoreNormalizesLegacySemantics
turrisMediaFormatFrameCountParserIsStrict
turrisMediaFormatWriterIsAtomicAndRewinds
turrisMediaFormatValidatorRejectsMalformedMetadata
turrisImagePreparationBuildsMxhdBlob
turrisUploadRejectsMalformedMxhdBeforeUsb
recoveredMediaProbeParserIsExact
recoveredMediaDiagnosticTailIsBounded
recoveredMediaValidatorIsStrictAndCancellable
sha256ValidationRequiresLowercaseAscii
privateRuntimePathsEnforceFilesystemBoundaries
runtimeApplyRequestCodecRoundTripsAndPreservesV8Defaults
runtimeDeviceMediaArtifactDbusRoundTrip
runtimeDeviceMediaMetadataDbusRoundTrip
runtimeDeviceMediaMetadataValidatorIsStrict
runtimeArtifactAdaptorCapturesCallerIdentity
deviceMediaArtifactStoreLifecycleAndOwnership
deviceMediaArtifactMetadataIsTransientAndHashBound
deviceManagerPropagatesExactArtifactMetadataProof
deviceMediaArtifactStoreExpiryAndOwnerDisconnect
deviceMediaArtifactStoreStartupCleanupIsBounded
deviceMediaArtifactStoreRejectsUnsafeReplacement
deviceMediaArtifactOwnershipAndLease
deviceMediaArtifactOwnerDisconnectRejectsLateSuccess
deviceMediaArtifactDisconnectedOwnerCannotStartRecoveredOperation
recoveredOperationIdempotencySurvivesActiveHold
mediaCatalogMediaIdPreservesApi8Domain
mediaCatalogStorePersistsOriginAndScopesReuseToDevice
mediaCatalogStoreRejectsUnsafePersistentState
mediaCatalogStoreRollsBackThumbnailWhenIndexCommitFails
mediaCatalogStoreRejectsSelfProducedThumbnailBudgetOverflow
mediaCatalogStoreFailClosedLoadRejectsThumbnailMutation
paseMetricsConfigStoreRoundTripsAndScopesToDevice
paseMetricsConfigStoreRejectsUnsafePersistentState
paseMetricsConfigStoreMutationFailurePreservesPreviousState
metricsConfigurationValidatesPersistsAndDisables
paseOverlayV1MigrationLoadsSingleArea
deleteIntentStoreRoundTripsV2AndClearsExpectedIdentity
deleteIntentStoreLoadsLegacyV1WithoutRewriting
deleteIntentStoreRejectsInvalidAndUnsafeState
deleteIntentStoreEnforcesMonotonicTransitions
deleteIntentStoreWriteFailurePreservesCommittedState
deleteIntentDispatchPersistenceFailureStopsBeforeFileRemove
deleteIntentSurvivesRestartAndOnlyReconcilesSameDevice
replaceJournalRestartRestoresExactProduct
replaceDeleteRestartRequiresCommonIdentity
pendingPreparationPreservesMediaTransform
successfulUploadDoesNotDeleteUnrelatedRetryCandidate
retryCacheStoreRecoversArtifactCommittedBesideCandidate
retryCacheStoreArmsWithSeparateConservativeV10Shadow
retryCacheStoreRemapsCandidateOperationId
retryCacheStorePreservesUnknownCanonicalState
retryCacheStoreMigratesLegacyVersions
retryCacheStoreResumesLegacyMigrationAfterStop
retryCacheStoreFencesMissingRetryShadow
retryCacheStoreRestoresCandidateWhenRetryStopsBeforeShadowSwitch
retryCacheStoreResumesShadowFenceResolutionAfterCrash
retryCacheStoreResumesPreparingCleanupFromDurableTombstone
retryCacheStoreRecoversLocalCommitCanonicalCrash
retryCacheStoreRejectsInexactProtectedTransitionIdentity
retryCacheStoreRejectsRestartedProtectedTransitionIdentityTamper
retryCacheStoreUpgradesLegacyProtectedTransitionIdentity
retryCacheStoreCleansPreparedManifestConflict
retryCacheStoreReleasedV10DowngradeRoundTrip
retryCacheStoreReleasedV10ReadsCompatibilityShadows
retryCacheReleasedV10DowngradeGateIsExact
retryCacheSplitShadowCleanupPreservesCanonicalV11
retryCacheSplitPreparationPersistsExactGeometry
retryCacheTransitionStoreCompletesInterruptedCleanup
retryCacheTransitionStoreLegacyV1RebindSyncFailureBlocksMutations
retryCacheTransitionStoreRejectsIncompleteProtectedBackup
retryCacheTransitionStoreRejectsReusedProtectedOperationId
retryCacheTransitionStoreRejectsAliasedDispatchOperationId
uploadDispatchBarrierPersistsBeforeUsb
uploadDispatchBarrierFailureStopsBeforeUsb
uploadDispatchBarrierSyncFailureStopsBeforeUsb
retryCacheStoreRecoversCompletedProtectingAggregate
retryCacheStoreRecoversShadowCommittedPreparingDispatch
startupRetryCacheValidationBatchRemapsCollision
retryCacheArtifactValidationHonorsPreStartCancellation
mediaPreparationFinalStartGateRejectsCancellation
mediaPreparationBoundsAndThumbnailFallback
shutdownPreservesPreparedMediaAsUnknownRetry
cacheCleanupRuntimeContractAndOperationAreExact
cacheCleanupBlockersAndCancellationAreExact
cacheCleanupPartialOutcomesAreExact
EOF

require_tests \
    runtime-capability-handshake \
    "$project_root/build/runtimeclient-handshake-tests/runtimeclient-handshake-tests" \
    true <<'EOF'
legacyApi8KeepsBaselineAndUsesEmptyCapabilities
fullMetricsCatalogUsesExactUniqueOwner
metricsCatalogIsSeparateFromLiveAvailability
missingMetricsCatalogMethodUsesBoundedFallback
onlyExactUnknownMethodUsesBoundedFallback
malformedMetricsCatalogFailsClosed
nonUnknownMetricsCatalogErrorFailsClosed
staleMetricsCatalogReplyFromSupersededAttemptSameOwnerIsDiscarded
staleMetricsCatalogReplyFromPreviousOwnerIsDiscarded
currentOwnerMetricsAndDisplaySignalsAreAccepted
nonLegacyCapabilityErrorFailsClosed
validCapabilitiesAreFilteredAndDeviceBound
deviceSpecificationsAreCapabilityGatedAndDeviceBound
deviceSpecificationsContextStatesAreExact
deviceSpecificationsGetterErrorsFailClosed
malformedDeviceSpecificationsFailClosed
displaySessionRefreshTransitionsSpecificationsToReady
staleDeviceSpecificationsReplyAfterRevisionChangeIsDiscarded
printerOperationsCancellationFencesDeviceSpecifications
staleDeviceSpecificationsReplyFromPreviousOwnerIsDiscarded
invalidDeviceCapabilitiesStayFailClosed
connectionRevisionInvalidatesAndRequeriesDeviceCapabilities
generationOnlyCancellationInvalidatesAndRequeriesCapabilities
repeatedRegistrationDuringPendingApiHandshakeRestartsHandshake
staleDeviceReplyAfterRevisionChangeIsDiscarded
staleReplyFromPreviousOwnerIsDiscarded
presentationPreferencesAreConfirmedAndWritable
invalidPresentationPreferencesFailClosed
presentationSignalBeforeGetterReplyWins
presentationSetterFailureKeepsConfirmedState
presentationSetterFailureWaitsForReconciliation
stalePresentationSignalCallbackFromPreviousOwnerIsDiscarded
stalePresentationSetterReplyFromPreviousOwnerIsDiscarded
supportSnapshotIsCapabilityGatedAndValidated
staleSupportSnapshotReplyFromPreviousOwnerIsDiscarded
offlineDeviceMediaMetadataRequestRecordsExactArtifactLease
deviceMediaMetadataIsCapabilityGatedAndArtifactBound
malformedDeviceMediaMetadataFailsClosed
deviceMediaMetadataFailureDoesNotInvalidateArtifactClaim
newArtifactSupersedesPendingDeviceMediaMetadataRequest
staleDeviceMediaMetadataReplyFromPreviousOwnerIsDiscarded
savedLayoutsRequireExactCapabilityAndConfirmedSnapshot
advertisedSavedLayoutsUnknownMethodFailsClosed
malformedSavedLayoutsSnapshotFailsClosed
staleSavedLayoutsReplyFromPreviousOwnerIsDiscarded
staleSavedLayoutPutReplyFromPreviousOwnerIsDiscarded
deviceIdentityChangeInvalidatesAndRequeriesSavedLayoutsWithoutMutation
nonCanonicalConnectionIdentityCannotAuthorizeOrRetainReadySavedLayouts
savedLayoutPutAndDeletePublishOnlyConfirmedSnapshots
savedLayoutMutationRepliesRequireExactNextRevision
savedLayoutReadsRejectSameRevisionChangesAndRollback
savedLayoutConflictReconcilesAndCommitUnknownFailsClosed
savedLayoutMutationsRequireCurrentOwnerBeforeSend
savedLayoutTransportAmbiguityFailsClosedAndReconciles
savedLayoutPreSendOwnerFailureIsDeliveredAsynchronously
cacheCleanupNoReplyReconcilesOnceAndCancelDoesNotReplay
cacheCleanupOwnerReplacementNeverTargetsNewOwner
supportBundleControllerExportsUnavailableHostReport
supportBundleControllerExportsFullAndHostOnlyReports
supportBundleControllerAllowsOnlyOneInflightRequest
supportBundleControllerRejectsInvalidSnapshotWithoutFile
EOF

require_tests \
    quick-client \
    "$project_root/build/quick-tests/tryx-quick-tests" <<'EOF'
legacyScreenConfigKeepsManager1Shape
savedLayoutModelUsesExactDecimalRevisionAndFullStateDto
savedLayoutsSnapshotValidationFailsClosed
savedLayoutsRequireCapabilityAndExactDevice
savedLayoutSaveQueuesOnlyCanonicalStoreCas
savedLayoutDeleteQueuesOnlyStoreCas
savedLayoutApplyKeepsProvenanceAndDisplayLifecycle
operationsExposeStableRoles
mediaPreparationTargetIsCapabilityBoundAndResetsTransform
recoveredTargetPreselectionRequiresExactActiveLayout
mediaPreparationProfileDispatchIsCapabilityBound
pendingCapabilityLossResetsTargetAfterSubmissionFailure
catalogExposesOriginMetadataRoles
operationsRejectStaleEvents
capabilityLifecycleSignalsInvalidatePresentationGates
displayStateRequiresStrictlyIncreasingRevision
untrustedDirectMetricsAndDisplayCallbacksAreDiscarded
operationAcknowledgementRequiresExactIdentity
displayApplySubmissionIdentityIsSynchronousAndUnique
modernDisplayApplyRequiresTerminalAndMatchingState
modernDisplayApplyRequiresExactDeviceIdentity
modernControlOnlyDisplayApplyPreservesLayoutAndOverlay
modernDisplayApplyFailuresNeverSucceed
modernDisplayApplyAmbiguousAcknowledgementStaysUnresolved
displayApplyTimeoutStaysUnresolvedUntilAbandoned
legacyDisplayApplyUsesOneMatchingManager1Mutation
legacyDisplayStateIsBoundToExactDeviceIdentity
legacyDisplayPreflightClassifiesBeforeDispatchFailure
legacyDisplayApplyRejectsUnsupportedComposites
unavailableConfirmedMetricCanBeAppliedInSameArea
unavailableMetricGrandfatheringIsContextBound
legacyFallbackCatalogDropsStaleConfirmedDisplayTokens
metricSelectionLimitIsThreePerAreaAtClient
unconfirmedCatalogDoesNotAuthorizeNewMetric
mediaEditorMetadataWaitsForCapabilityHandshake
mediaEditorDeviceCopyMetadataIsArtifactScoped
deviceMediaWorkflowInvalidationStopsReplaceMutations
deviceMediaWorkflowRejectsMismatchedClaimIdentity
deviceMediaWorkflowSaveAsNewCompletesAndReleasesLease
systemMetricsModelMapsAvailability
systemMetricsModelPollsOnlyWhileDashboardActive
appSettingsDefaultToEnglishAndPreserveConfig
guiAutostartRoundTripsWithoutRuntimeService
guiAutostartCreatesPrivateDirectoryWithPermissiveUmask
guiAutostartRejectsUnsafeConfigHierarchy
guiAutostartRejectsUnsafeAndForeignEntries
guiAutostartPreservesPostCheckRacedEntries
guiAutostartRebindsManagedEntryToCurrentExecutable
guiAutostartRejectsRelativeTryExecWithPathComponent
guiAutostartRejectsLeafSwapToFifoWithoutBlocking
guiAutostartMasksSystemEntry
guiAutostartWriteFailureAndOfflineStayBounded
guiAutostartEscapesExecutablePath
supportBundleReportUsesHostAllowlist
supportBundleWriterCreatesPrivateFileWithoutClobbering
supportBundleWriterRejectsBeforePublishGuardFails
supportBundleWriterRollsBackAfterPublishGuardFails
supportBundleWriterPreservesReplacedRollbackTarget
supportBundleWriterRejectsUnsafeTargets
supportBundleWriterRejectsPreparedTargetAndDirectoryRaces
previewCleanupIsAllowlistedAndInterlocked
previewCleanupBoundsAllDirectoryEntries
cacheCleanupRequestsAreExactAndTracked
cacheManagementRunsRuntimeBeforePreviewCleanup
cacheManagementPreservesPartialAndUnresolvedOutcomes
cacheManagementTerminalMatrixIsExact
windowChromeHidesAndRestoresOnlyWithTray
windowChromeAutostartHidesOnlyWithPolicyAndTray
EOF

require_tests \
    runtime-bootstrap \
    "$project_root/build/runtimebootstrap-tests/quick/tryx-runtimebootstrap-tests" \
    true <<'EOF'
instanceLaunchIntentPayloadIsStrict
instanceSocketPathRequiresPrivateRuntimeDirectory
activeSingleInstanceSocketCannotBeStolen
singleInstanceGuardNotifiesLiveOwner
singleInstanceGuardRejectsUnacknowledgedOwner
singleInstanceGuardFailsClosedWhileOwnerStarts
singleInstanceGuardRecoversCrashedOwner
singleInstanceGuardRecoversLeaseAcquiredDuringWait
singleInstanceGuardPreservesSocketWonAfterFinalProbe
singleInstanceGuardPreservesSocketWonBeforeExchange
singleInstanceGuardPreservesSocketWonAfterExchange
singleInstanceGuardRejectsSocketReboundAfterNativeBind
singleInstanceGuardRejectsRuntimeDirectoryReplacement
instanceLaunchConnectionsDrainPendingAndLatePayloads
ownerChangeDuringApiProbeFailsClosed
guiBootstrapExitLeavesBuildTreeRuntimeRunning
activeIncompatibleOwnerIsNotRestarted
installedModeDoesNotRestartAnActiveOperation
invalidBuildRuntimeDoesNotFallBackToSystemd
EOF

require_tests \
    linux-tray \
    "$project_root/build/linuxtray-tests/linuxtraycontroller-tests" <<'EOF'
watcherLifecycleAndActions
trayQuitWaitsForGuardApproval
guardedCloseSeparatesHideAndWindowQuit
windowChromeBridgesExplicitQuitApproval
windowCloseWithQuitPolicyTerminatesGuiEventLoop
windowCloseWithoutHostTerminatesGuiEventLoop
autostartHidesWhenHostIsAlreadyAvailable
autostartHidesWhenHostAppearsBeforeDeadline
autostartShowsWhenHostDeadlineExpires
autostartShowsImmediatelyWithQuitPolicy
manualActivationAndHostLossRestoreWindow
noWatcherFallsBackToUnavailable
EOF

require_tests \
    replace-journal \
    "$project_root/build/replacejournal-tests/replacejournal-tests" <<'EOF'
malformedAndUnexpectedFieldsFailClosed
unsafeFilesystemEntriesFailClosed
transitionsAreMonotonic
legacyVersion1LoadsAs1021AndUpgradesOnTransition
version2ProductIdentityIsExactAndImmutable
duplicateTopLevelKeysFailClosed
EOF

printf '%s\n' 'Runtime refactor characterization baseline verified'
