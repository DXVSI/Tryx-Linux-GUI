#include <QtTest>

#include "appsettingscontroller.h"
#include "applicationpaths.h"
#include "cachemanagementcontroller.h"
#include "devicemediaworkflowcontroller.h"
#include "guiautostart.h"
#include "mediacatalogmodel.h"
#include "mediaeditorcontroller.h"
#include "mediapreviewcontroller.h"
#include "mediatransform.h"
#include "operationlistmodel.h"
#include "runtimeclient.h"
#include "supportbundle.h"
#include "supportsnapshot.h"
#include "systemmetricsmodel.h"
#include "windowchromecontroller.h"

#include <panorama/config.hpp>

#include <QDir>
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDBusPendingCallWatcher>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QWindow>

#include <atomic>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

QList<int> descriptorsForIdentity(const struct stat &identity) {
    QList<int> descriptors;
    const auto names = QDir(QStringLiteral("/proc/self/fd"))
                           .entryList(QDir::AllEntries | QDir::System |
                                      QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        bool numeric = false;
        const int descriptor = name.toInt(&numeric);
        struct stat status {};
        if (numeric && ::fstat(descriptor, &status) == 0 &&
            status.st_dev == identity.st_dev &&
            status.st_ino == identity.st_ino) {
            descriptors.append(descriptor);
        }
    }
    return descriptors;
}

class ScopedEnvironmentVariable final {
public:
    explicit ScopedEnvironmentVariable(const char *name)
        : name_(name),
          wasSet_(qEnvironmentVariableIsSet(name)),
          previousValue_(qgetenv(name)) {}

    ~ScopedEnvironmentVariable() {
        if (wasSet_) {
            qputenv(name_.constData(), previousValue_);
        } else {
            qunsetenv(name_.constData());
        }
    }

    void set(const QByteArray &value) {
        qputenv(name_.constData(), value);
    }

private:
    QByteArray name_;
    bool wasSet_ = false;
    QByteArray previousValue_;
};

class ScopedUmask final {
public:
    explicit ScopedUmask(mode_t mask)
        : previous_(::umask(mask)) {}

    ~ScopedUmask() {
        ::umask(previous_);
    }

private:
    mode_t previous_ = 0;
};

class ScopedCurrentDirectory final {
public:
    explicit ScopedCurrentDirectory(const QString &path)
        : previous_(QDir::currentPath()),
          changed_(QDir::setCurrent(path)) {}

    ~ScopedCurrentDirectory() {
        if (changed_) {
            QDir::setCurrent(previous_);
        }
    }

    bool changed() const {
        return changed_;
    }

private:
    QString previous_;
    bool changed_ = false;
};

class ScopedGuiAutostartConfigOverride final {
public:
    explicit ScopedGuiAutostartConfigOverride(
        const QString &directory) {
        gui_autostart::testing::setUserConfigDirectoryOverride(
            directory);
    }

    ~ScopedGuiAutostartConfigOverride() {
        gui_autostart::testing::clearUserConfigDirectoryOverride();
    }
};

bool createFakeSystemctl(const QString &directory,
                         const QString &logPath) {
    const QString programPath =
        QDir(directory).filePath(QStringLiteral("systemctl"));
    QFile program(programPath);
    if (!program.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray script = QByteArrayLiteral(
        "#!/bin/sh\n"
        "printf '%s\\n' \"$*\" >> \"$TRYX_GUI_AUTOSTART_TEST_SYSTEMCTL_LOG\"\n"
        "if [ \"$2\" = \"is-enabled\" ]; then\n"
        "    printf '%s\\n' 'disabled'\n"
        "    exit 1\n"
        "fi\n"
        "exit 0\n");
    if (program.write(script) != script.size()) {
        return false;
    }
    program.close();
    if (!QFile::setPermissions(
            programPath,
            QFileDevice::ReadOwner |
                QFileDevice::WriteOwner |
                QFileDevice::ExeOwner)) {
        return false;
    }
    qputenv(
        "TRYX_GUI_AUTOSTART_TEST_SYSTEMCTL_LOG",
        QFile::encodeName(logPath));
    return true;
}

QString guiAutostartEntryPath(const QString &configHome) {
    return QDir(configHome).filePath(QStringLiteral(
        "autostart/tryx-panorama-manager.desktop"));
}

bool writeSystemAutostartEntry(const QString &configDirectory,
                               bool hidden = false,
                               bool includeTryExec = true,
                               const QByteArray &extraKeys = {}) {
    const QString entryPath =
        guiAutostartEntryPath(configDirectory);
    if (!QDir().mkpath(QFileInfo(entryPath).absolutePath())) {
        return false;
    }
    QFile entry(entryPath);
    if (!entry.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray executable =
        QFile::encodeName(QCoreApplication::applicationFilePath());
    QByteArray contents = QByteArrayLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=System TRYX Panorama Manager\n"
        "Exec=\"");
    contents.append(executable);
    contents.append(QByteArrayLiteral("\" --autostart\n"));
    if (includeTryExec) {
        contents.append(QByteArrayLiteral("TryExec="));
        contents.append(executable);
        contents.append('\n');
    }
    contents.append(QByteArrayLiteral(
        "Icon=tryx-panorama\n"
        "Terminal=false\n"
        "StartupNotify=false\n"
        "Hidden="));
    contents.append(hidden
        ? QByteArrayLiteral("true\n")
        : QByteArrayLiteral("false\n"));
    contents.append(extraKeys);
    return entry.write(contents) == contents.size();
}

QString expectedDesktopString(const QString &value) {
    QString escaped;
    for (const QChar character : value) {
        if (character == QLatin1Char('\\')) {
            escaped.append(QStringLiteral("\\\\"));
        } else if (character == QLatin1Char(' ')) {
            escaped.append(QStringLiteral("\\s"));
        } else {
            escaped.append(character);
        }
    }
    return escaped;
}

QString expectedExecExecutable(const QString &path) {
    QString escaped = QStringLiteral("\"");
    for (const QChar character : path) {
        if (character == QLatin1Char('\\')) {
            escaped.append(QStringLiteral("\\\\\\\\"));
        } else if (character == QLatin1Char('"') ||
                   character == QLatin1Char('`') ||
                   character == QLatin1Char('$')) {
            escaped.append(QStringLiteral("\\\\"));
            escaped.append(character);
        } else if (character == QLatin1Char('%')) {
            escaped.append(QStringLiteral("%%"));
        } else {
            escaped.append(character);
        }
    }
    escaped.append(QLatin1Char('"'));
    return escaped;
}

QByteArray managedAutostartEntry(const QString &executable,
                                 bool hidden,
                                 const QByteArray &extraKeys = {}) {
    QByteArray contents = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=TRYX Panorama Manager\n"
        "Name[ru]=Менеджер TRYX Panorama\n"
        "Exec=%1 --autostart\n"
        "TryExec=%2\n"
        "Icon=tryx-panorama\n"
        "Terminal=false\n"
        "StartupNotify=false\n"
        "Hidden=%3\n"
        "X-TRYX-Panorama-Managed=true\n")
        .arg(expectedExecExecutable(executable),
             expectedDesktopString(executable),
             hidden ? QStringLiteral("true")
                    : QStringLiteral("false"))
        .toUtf8();
    contents.append(extraKeys);
    return contents;
}

QByteArray managedEntryWithSlowBlankLineParsing(bool hidden) {
    QByteArray contents = QByteArrayLiteral(" \n");
    contents.append(managedAutostartEntry(
        QCoreApplication::applicationFilePath(), hidden));
    while (contents.size() + 1 < 60 * 1024) {
        contents.append('\n');
    }
    return contents;
}

struct GuardedAutostartChange {
    bool watcherInstalled = false;
    bool contentsMutated = false;
    bool operationSucceeded = false;
    QString error;
};

GuardedAutostartChange changeAfterFirstEntryRead(
    const QString &entryPath, bool enabled) {
    GuardedAutostartChange result;
    const QByteArray directory = QFile::encodeName(
        QFileInfo(entryPath).absolutePath());
    const QByteArray entryName = QFile::encodeName(
        QFileInfo(entryPath).fileName());
    const QByteArray encodedEntry = QFile::encodeName(entryPath);
    const int notifier = ::inotify_init1(IN_CLOEXEC);
    if (notifier < 0) {
        return result;
    }
    const int watch = ::inotify_add_watch(
        notifier, directory.constData(), IN_CLOSE_NOWRITE);
    if (watch < 0) {
        ::close(notifier);
        return result;
    }
    result.watcherInstalled = true;

    std::atomic_bool mutated = false;
    std::thread mutator([&]() {
        struct pollfd pollDescriptor {
            notifier, POLLIN, 0
        };
        if (::poll(&pollDescriptor, 1, 3000) <= 0) {
            return;
        }
        alignas(struct inotify_event) char buffer[4096];
        const ssize_t count = ::read(
            notifier, buffer, sizeof(buffer));
        if (count <= 0) {
            return;
        }
        const char *cursor = buffer;
        const char *end = buffer + count;
        while (cursor < end) {
            const auto *event =
                reinterpret_cast<const struct inotify_event *>(cursor);
            if ((event->mask & IN_CLOSE_NOWRITE) != 0 &&
                event->len > 0 &&
                QByteArray(event->name) == entryName) {
                const int descriptor = ::open(
                    encodedEntry.constData(),
                    O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
                if (descriptor >= 0) {
                    const char replacement = '\t';
                    mutated.store(
                        ::pwrite(descriptor, &replacement, 1, 0) == 1,
                        std::memory_order_release);
                    ::close(descriptor);
                }
                return;
            }
            cursor += sizeof(struct inotify_event) + event->len;
        }
    });

    result.operationSucceeded =
        gui_autostart::setEnabled(enabled, &result.error);
    mutator.join();
    result.contentsMutated =
        mutated.load(std::memory_order_acquire);
    ::inotify_rm_watch(notifier, watch);
    ::close(notifier);
    return result;
}

enum class AutostartNamespaceRaceAction {
    ReplaceLeaf,
    ReplaceDirectory,
    ReplacePreparedEntry,
};

struct AutostartNamespaceRaceContext {
    gui_autostart::testing::LeafNamespaceMutation expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Create;
    AutostartNamespaceRaceAction action =
        AutostartNamespaceRaceAction::ReplaceLeaf;
    QByteArray foreignContents;
    QString movedDirectoryPath;
    QString mutatedPath;
    bool afterPreconditionCheck = false;
    bool afterCommit = false;
    bool triggered = false;
    bool mutationSucceeded = false;
};

AutostartNamespaceRaceContext *gAutostartNamespaceRace = nullptr;

bool writeAutostartRaceEntry(const QString &path,
                             const QByteArray &contents) {
    QFile entry(path);
    if (!entry.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        entry.write(contents) != contents.size()) {
        return false;
    }
    entry.close();
    return QFile::setPermissions(
        path,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner);
}

void mutateAutostartNamespaceAtStage(
    bool afterPreconditionCheck,
    bool afterCommit,
    gui_autostart::testing::LeafNamespaceMutation operation,
    const QString &entryPath) {
    if (!gAutostartNamespaceRace ||
        operation != gAutostartNamespaceRace->expectedOperation ||
        afterPreconditionCheck !=
            gAutostartNamespaceRace->afterPreconditionCheck ||
        afterCommit != gAutostartNamespaceRace->afterCommit) {
        return;
    }
    AutostartNamespaceRaceContext &context =
        *gAutostartNamespaceRace;
    context.triggered = true;
    const QString directoryPath =
        QFileInfo(entryPath).absolutePath();
    if (context.action ==
        AutostartNamespaceRaceAction::ReplaceDirectory) {
        const QFileInfo directoryInfo(directoryPath);
        context.movedDirectoryPath =
            QDir(directoryInfo.absolutePath()).filePath(
                QStringLiteral("autostart-raced"));
        QDir().remove(context.movedDirectoryPath);
        context.mutationSucceeded =
            QDir(directoryInfo.absolutePath()).rename(
                directoryInfo.fileName(),
                QFileInfo(context.movedDirectoryPath).fileName()) &&
            QDir().mkpath(directoryPath) &&
            writeAutostartRaceEntry(
                entryPath, context.foreignContents);
        return;
    }
    if (context.action ==
        AutostartNamespaceRaceAction::ReplacePreparedEntry) {
        const QStringList temporaryEntries =
            QDir(directoryPath).entryList(
                {QStringLiteral(
                     ".tryx-panorama-manager.desktop.tmp.*")},
                QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                QDir::Name);
        if (temporaryEntries.isEmpty()) {
            return;
        }
        context.mutatedPath = QDir(directoryPath).filePath(
            temporaryEntries.constFirst());
        QFile::remove(context.mutatedPath);
        context.mutationSucceeded = writeAutostartRaceEntry(
            context.mutatedPath, context.foreignContents);
        return;
    }

    QFile::remove(entryPath);
    context.mutatedPath = entryPath;
    context.mutationSucceeded = writeAutostartRaceEntry(
        entryPath, context.foreignContents);
}

void mutateAutostartNamespaceBefore(
    gui_autostart::testing::LeafNamespaceMutation operation,
    const QString &entryPath) {
    mutateAutostartNamespaceAtStage(
        false, false, operation, entryPath);
}

void mutateAutostartNamespaceAfterPreconditionCheck(
    gui_autostart::testing::LeafNamespaceMutation operation,
    const QString &entryPath) {
    mutateAutostartNamespaceAtStage(
        true, false, operation, entryPath);
}

void mutateAutostartNamespaceAfter(
    gui_autostart::testing::LeafNamespaceMutation operation,
    const QString &entryPath) {
    mutateAutostartNamespaceAtStage(
        false, true, operation, entryPath);
}

QString savedLayoutMediaIdForTest(
    const QString &deviceIdentity,
    const TryxRuntimeSavedMediaRefV1 &reference) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    hash.addData(deviceIdentity.toUtf8());
    hash.addData(separator);
    hash.addData(reference.name.toUtf8());
    hash.addData(separator);
    hash.addData(QByteArray::number(reference.size));
    hash.addData(separator);
    hash.addData(QByteArray::number(reference.source));
    return QString::fromLatin1(hash.result().toHex());
}

TryxRuntimeSavedLayoutV1 canonicalSavedLayoutForTest(
    quint64 revision = 9007199254740993ULL) {
    TryxRuntimeSavedLayoutV1 layout;
    layout.layoutId =
        QStringLiteral("01234567-89ab-4cde-8f01-23456789abcd");
    layout.revision = revision;
    layout.deviceIdentity = QStringLiteral("device-a");
    layout.productId = QStringLiteral("391a:1021");
    layout.name = QStringLiteral("Quiet monitoring");

    TryxRuntimeSavedMediaRefV1 media;
    media.name = QStringLiteral("dashboard.h264_2240x1080");
    media.size = 9876543210ULL;
    media.source = 1U;
    media.readOnly = true;
    media.mediaId = savedLayoutMediaIdForTest(
        layout.deviceIdentity, media);
    layout.media = {media};

    TryxRuntimeApplyRequest &request = layout.request;
    request.media = {media.name};
    request.ratio = QStringLiteral("2:1");
    request.screenMode = QStringLiteral("Full Screen");
    request.playMode = QStringLiteral("Loop");
    request.sysinfoLabels = {
        QStringLiteral("GPU Usage"),
        QStringLiteral("CPU Temperature"),
    };
    request.settingsPosition = QStringLiteral("Bottom");
    request.settingsColor = QStringLiteral("#1a2b3c");
    request.settingsAlign = QStringLiteral("Center");
    request.settingsBadges = {
        QStringLiteral("GPU Badge"),
        QStringLiteral("CPU Badge"),
    };
    request.filterOpacity = 0;
    request.presetId.clear();
    request.waterfallMode = true;
    request.replaceOverlay = true;
    request.display.brightnessPresent = true;
    request.display.brightness = 73;
    request.display.standbyPresent = false;
    request.display.standbyEnabled = false;
    request.display.orientationPresent = true;
    request.display.mirrorMode = true;
    request.display.waterfallMode = true;
    request.display.backlightPresent = false;
    request.display.backlightEnabled = true;
    return layout;
}

TryxRuntimeSavedLayoutsSnapshotV1 savedLayoutsSnapshotForTest(
    const TryxRuntimeSavedLayoutV1 &layout,
    quint64 revision = 9007199254741001ULL) {
    TryxRuntimeSavedLayoutsSnapshotV1 snapshot;
    snapshot.revision = revision;
    snapshot.status = QStringLiteral("Ready");
    snapshot.deviceIdentity = layout.deviceIdentity;
    snapshot.productId = layout.productId;
    snapshot.layouts = {layout};
    return snapshot;
}

QVariantMap savedLayoutFullStateForTest(const QVariantMap &dto) {
    return {
        {QStringLiteral("layout"),
         dto.value(QStringLiteral("layout"))},
        {QStringLiteral("brightness"),
         dto.value(QStringLiteral("brightness"))},
        {QStringLiteral("orientation"),
         dto.value(QStringLiteral("orientation"))},
    };
}

}  // namespace

class QuickClientTests final : public QObject {
    Q_OBJECT

private slots:
    void sharedApplicationDataPathUsesStableManagerNamespace();
    void transformDefaultsAreCanonical();
    void mediaPreparationTargetIsCapabilityBoundAndResetsTransform();
    void pendingCapabilityLossResetsTargetAfterSubmissionFailure();
    void recoveredTargetPreselectionRequiresExactActiveLayout();
    void mediaPreparationProfileDispatchIsCapabilityBound();
    void runtimeMediaTargetFollowsProductId();
    void runtimeDeviceSummaryFollowsLiveSnapshot();
    void offlinePrinterLifecycleEventsDoNotReadOwnerlessSnapshots();
    void capabilityLifecycleSignalsInvalidatePresentationGates();
    void savedLayoutModelUsesExactDecimalRevisionAndFullStateDto();
    void savedLayoutsSnapshotValidationFailsClosed();
    void savedLayoutsRequireCapabilityAndExactDevice();
    void savedLayoutSaveQueuesOnlyCanonicalStoreCas();
    void savedLayoutDeleteQueuesOnlyStoreCas();
    void savedLayoutApplyKeepsProvenanceAndDisplayLifecycle();
    void customSavedLayoutUsesVersionedContract_data();
    void customSavedLayoutUsesVersionedContract();
    void nonCropFieldsAreNeutral();
    void cropRotationResetsViewport();
    void previewUsesCanonicalTransformFilter();
    void renderedTransformPreservesDisplayGeometry();
    void previewGenerationIsDebounced();
    void previewCleanupIsAllowlistedAndInterlocked();
    void previewCleanupBoundsAllDirectoryEntries();
    void restoredProtectedSourceSchedulesCurrentProfilePreview();
    void sourceSnapshotIsImmutableAfterCopy();
    void sourceSnapshotPreservesExistingFinal();
    void stageHelperRejectsSymlinkDotDotEscape();
    void protectedInboxSourceSurvivesControllerLifetime();
    void stagingTimeoutDoesNotBlockTheController();
    void drainingStageHelperBlocksRepeatedStart();
    void previewControllerRendersImmutableExactFrame();
    void publicPreviewRejectsRawDeviceH264();
    void exportHelperIsAtomicAndDoesNotClobber();
    void supportBundleReportUsesHostAllowlist();
    void supportBundleWriterCreatesPrivateFileWithoutClobbering();
    void supportBundleWriterRejectsBeforePublishGuardFails();
    void supportBundleWriterRollsBackAfterPublishGuardFails();
    void supportBundleWriterPinsIdentityThroughPublication_data();
    void supportBundleWriterPinsIdentityThroughPublication();
    void supportBundleWriterPreservesReplacedRollbackTarget();
    void supportBundleWriterRejectsUnsafeTargets();
    void supportBundleWriterRejectsPreparedTargetAndDirectoryRaces();
    void qmlImportScannerFindsResolvedModules();
    void catalogRejectsStaleRevision();
    void catalogResolvesThumbnailFromConfiguredDataRoot();
    void catalogExposesOriginMetadataRoles();
    void catalogExposesDeviceCopyEligibility();
    void legacyConnectionPopulatesCurrentMediaModel();
    void legacyScreenConfigKeepsManager1Shape();
    void legacyTransformBoundaryIsExplicit();
    void legacyUploadRetainsSourceUntilTerminalSignal();
    void operationsExposeStableRoles();
    void operationsRejectStaleEvents();
    void displayStateRequiresStrictlyIncreasingRevision();
    void untrustedDirectMetricsAndDisplayCallbacksAreDiscarded();
    void operationAcknowledgementRequiresExactIdentity();
    void cacheCleanupRequestsAreExactAndTracked();
    void cacheManagementRunsRuntimeBeforePreviewCleanup();
    void cacheManagementPreservesPartialAndUnresolvedOutcomes();
    void cacheManagementTerminalMatrixIsExact();
    void succeededOperationClearsBusyState();
    void mediaEditorMetadataWaitsForCapabilityHandshake();
    void mediaEditorDeviceCopyMetadataIsArtifactScoped();
    void deviceMediaWorkflowRejectsMismatchedClaimIdentity();
    void deviceMediaWorkflowSaveAsNewCompletesAndReleasesLease();
    void deviceMediaWorkflowInvalidationStopsReplaceMutations();
    void applyRequestPreservesConfirmedOverlaySettings();
    void applyRequestsUseExplicitValidatedOverlayStyles();
    void displayApplySubmissionIdentityIsSynchronousAndUnique();
    void modernDisplayApplyRequiresTerminalAndMatchingState();
    void badgeDisplaySubmissionRequiresCoherentSnapshot_data();
    void badgeDisplaySubmissionRequiresCoherentSnapshot();
    void modernDisplayApplyRequiresExactDeviceIdentity();
    void modernControlOnlyDisplayApplyPreservesLayoutAndOverlay();
    void modernDisplayApplyFailuresNeverSucceed();
    void modernDisplayApplyAmbiguousAcknowledgementStaysUnresolved();
    void displayApplyTimeoutStaysUnresolvedUntilAbandoned();
    void legacyDisplayApplyUsesOneMatchingManager1Mutation();
    void legacyDisplayStateIsBoundToExactDeviceIdentity();
    void legacyDisplayPreflightClassifiesBeforeDispatchFailure();
    void legacyDisplayApplyRejectsUnsupportedComposites();
    void unavailableConfirmedMetricCanBeAppliedInSameArea();
    void unavailableMetricGrandfatheringIsContextBound();
    void legacyFallbackCatalogDropsStaleConfirmedDisplayTokens();
    void metricSelectionLimitIsThreePerAreaAtClient();
    void unconfirmedCatalogDoesNotAuthorizeNewMetric();
    void metricsRequestUsesExplicitEnableDisableContract();
    void systemMetricsModelMapsAvailability();
    void systemMetricsModelPollsOnlyWhileDashboardActive();
    void appSettingsDefaultToEnglishAndPreserveConfig();
    void guiAutostartRoundTripsWithoutRuntimeService();
    void guiAutostartCreatesPrivateDirectoryWithPermissiveUmask_data();
    void guiAutostartCreatesPrivateDirectoryWithPermissiveUmask();
    void guiAutostartRejectsUnsafeConfigHierarchy_data();
    void guiAutostartRejectsUnsafeConfigHierarchy();
    void guiAutostartRejectsEmptyConfigDirectory();
    void guiAutostartRejectsUnsafeAndForeignEntries();
    void guiAutostartPreservesMalformedManagedEntries_data();
    void guiAutostartPreservesMalformedManagedEntries();
    void guiAutostartRejectsWritableUserPaths();
    void guiAutostartRequiresExactManagedKeys();
    void guiAutostartMasksSystemEntry();
    void guiAutostartHonorsMinimalSystemHiddenOverride();
    void guiAutostartHonorsCurrentDesktopForSystemEntries_data();
    void guiAutostartHonorsCurrentDesktopForSystemEntries();
    void guiAutostartRejectsRelativeTryExecWithPathComponent();
    void guiAutostartRejectsUnsafeSystemEntryWithoutBlocking_data();
    void guiAutostartRejectsUnsafeSystemEntryWithoutBlocking();
    void guiAutostartWriteFailureAndOfflineStayBounded();
    void guiAutostartRejectsConcurrentInPlaceChanges();
    void guiAutostartPreservesPostCheckRacedEntries();
    void guiAutostartRebindsManagedEntryToCurrentExecutable();
    void guiAutostartRejectsLeafSwapToFifoWithoutBlocking();
    void guiAutostartEscapesExecutablePath();
    void guiAutostartRejectsEqualsInExecutablePath();
    void guiAutostartRejectsControlInExecutablePath();
    void windowChromeRejectsOperationsWithoutWindow();
    void windowChromeHidesAndRestoresOnlyWithTray();
    void windowChromeAutostartHidesOnlyWithPolicyAndTray();

private:
    static void prepareModernDisplay(
        RuntimeClient *runtime,
        const QString &deviceIdentity =
            QStringLiteral("device-a"));
    static void preparePaseDeviceMedia(
        RuntimeClient *runtime, const QString &mediaId,
        const QString &mediaName,
        const QString &deviceIdentity);
    static TryxRuntimeDeviceMediaArtifact deviceMediaArtifact(
        const QString &operationId, const QString &artifactId,
        const QString &mediaId, const QString &mediaName,
        const QString &deviceIdentity);
    static TryxRuntimeDisplayState confirmedDisplayState(
        const TryxRuntimeApplyRequest &request, quint64 revision);
    static void configureSavedLayoutRuntime(
        RuntimeClient *runtime,
        const TryxRuntimeSavedLayoutV1 &layout);
};

void QuickClientTests::
    sharedApplicationDataPathUsesStableManagerNamespace() {
    QCOMPARE(
        panorama::sharedApplicationDataLocation(),
        QDir(QStandardPaths::writableLocation(
                 QStandardPaths::GenericDataLocation))
            .filePath(QStringLiteral(
                "DXVSI/TRYX Panorama Manager")));
}

void QuickClientTests::preparePaseDeviceMedia(
    RuntimeClient *runtime, const QString &mediaId,
    const QString &mediaName,
    const QString &deviceIdentity) {
    runtime->serviceAvailable_ = true;
    runtime->compatible_ = true;
    runtime->capabilitiesReady_ = true;
    runtime->connection_.revision = 1;
    runtime->connection_.printerClassConnected = true;
    runtime->connection_.printerClassDevicePresent = true;
    runtime->connection_.displaySessionActive = true;

    TryxRuntimeMediaEntry entry;
    entry.name = mediaName;
    entry.source = 1;
    entry.mediaId = mediaId;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = deviceIdentity;
    snapshot.entries = {entry};
    runtime->mediaModel()->applySnapshot(snapshot);
}

void QuickClientTests::prepareModernDisplay(
    RuntimeClient *runtime, const QString &deviceIdentity) {
    runtime->serviceAvailable_ = true;
    runtime->compatible_ = true;
    runtime->capabilitiesReady_ = true;
    runtime->connection_.printerClassConnected = true;
    runtime->connection_.printerClassDevicePresent = true;
    runtime->connection_.displaySessionActive = true;
    runtime->connection_.serial = deviceIdentity;
    runtime->display_.valid = true;
    runtime->display_.deviceSerial = deviceIdentity;
}

TryxRuntimeDeviceMediaArtifact
QuickClientTests::deviceMediaArtifact(
    const QString &operationId, const QString &artifactId,
    const QString &mediaId, const QString &mediaName,
    const QString &deviceIdentity) {
    TryxRuntimeDeviceMediaArtifact artifact;
    artifact.operationId = operationId;
    artifact.artifactId = artifactId;
    artifact.mediaId = mediaId;
    artifact.deviceIdentity = deviceIdentity;
    artifact.remoteName = mediaName;
    artifact.size = 1;
    artifact.decodedSha256 =
        QString(64, QLatin1Char('0'));
    artifact.localPath =
        QDir(tryxRuntimeDeviceMediaOutboxPath())
            .filePath(QStringLiteral("offline-test.h264"));
    artifact.logicalType = QStringLiteral("Video");
    artifact.leaseId = QStringLiteral("lease-1");
    artifact.leaseExpiresUtcMs =
        QDateTime::currentMSecsSinceEpoch() + 60000;
    return artifact;
}

TryxRuntimeDisplayState
QuickClientTests::confirmedDisplayState(
    const TryxRuntimeApplyRequest &request, quint64 revision) {
    TryxRuntimeDisplayState state;
    state.revision = revision;
    state.deviceSerial = QStringLiteral("device-a");
    state.valid = true;
    state.screenMode = request.screenMode;
    state.playMode = request.playMode;
    state.media = request.media;
    state.sysinfoLabels = request.sysinfoLabels;
    state.settingsBadges = request.settingsBadges;
    state.settingsPosition = request.settingsPosition;
    state.settingsColor = request.settingsColor;
    state.settingsAlign = request.settingsAlign;
    state.sysinfoLabels2 = request.sysinfoLabels2;
    state.settingsBadges2 = request.settingsBadges2;
    state.settingsPosition2 = request.settingsPosition2;
    state.settingsColor2 = request.settingsColor2;
    state.settingsAlign2 = request.settingsAlign2;
    if (request.display.brightnessPresent) {
        state.brightness = request.display.brightness;
    }
    if (request.display.orientationPresent) {
        state.mirrorMode = request.display.mirrorMode;
        state.waterfallMode = request.display.waterfallMode;
    } else {
        state.waterfallMode = request.waterfallMode;
    }
    if (request.display.backlightPresent) {
        state.backlightEnabled =
            request.display.backlightEnabled;
    }
    return state;
}

void QuickClientTests::configureSavedLayoutRuntime(
    RuntimeClient *runtime,
    const TryxRuntimeSavedLayoutV1 &layout) {
    runtime->serviceAvailable_ = true;
    runtime->compatible_ = true;
    runtime->capabilitiesReady_ = true;
    runtime->runtimeCapabilities_ = {
        tryxRuntimeSavedLayoutsV1Token(),
    };
    runtime->deviceCapabilitiesReady_ = true;
    runtime->deviceCapabilities_ = {
        tryxDeviceMediaCatalogV1Token(),
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceOverlayMetricsV1Token(),
        tryxDeviceMediaSplitAreaV1Token(),
    };
    runtime->metricsCatalogReady_ = true;
    runtime->metricsCatalog_ = tryxMetricsCatalog();
    runtime->connection_.revision = 17;
    runtime->connection_.connected = true;
    runtime->connection_.printerClassConnected = true;
    runtime->connection_.printerClassDevicePresent = true;
    runtime->connection_.displaySessionActive = true;
    runtime->connection_.serial = layout.deviceIdentity;
    runtime->connection_.productId = layout.productId;
    runtime->display_.revision = 41;
    runtime->display_.valid = true;
    runtime->display_.deviceSerial = layout.deviceIdentity;

    TryxRuntimeMediaCatalogSnapshot mediaSnapshot;
    mediaSnapshot.revision = 23;
    mediaSnapshot.deviceIdentity = layout.deviceIdentity;
    for (const TryxRuntimeSavedMediaRefV1 &reference : layout.media) {
        TryxRuntimeMediaEntry entry;
        entry.name = reference.name;
        entry.size = reference.size;
        entry.source = reference.source;
        entry.readOnly = reference.readOnly;
        entry.mediaId = reference.mediaId;
        mediaSnapshot.entries.append(entry);
    }
    runtime->mediaModel()->applySnapshot(mediaSnapshot);
}

void QuickClientTests::transformDefaultsAreCanonical() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    const TryxRuntimeMediaTransform transform = editor.transform();
    QCOMPARE(transform.schemaVersion, 1U);
    QCOMPARE(transform.mode, QStringLiteral("Fit"));
    QCOMPARE(transform.rotationQuarterTurns, 0U);
    QCOMPARE(transform.zoomPermille, 1000U);
    QCOMPARE(transform.focusX, 5000U);
    QCOMPARE(transform.focusY, 5000U);
    QCOMPARE(transform.backgroundRgb, 0U);
}

void QuickClientTests::
    mediaPreparationTargetIsCapabilityBoundAndResetsTransform() {
    RuntimeClient runtime(true);
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeMediaPreparationProfileV1Token(),
    };
    runtime.deviceCapabilitiesReady_ = true;
    runtime.deviceCapabilities_ = {
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceMediaSplitAreaV1Token(),
    };
    runtime.connection_.productId = QStringLiteral("391a:1021");
    MediaEditorController editor(&runtime);

    QCOMPARE(editor.preparationTarget(), QStringLiteral("FullFrame"));
    QVERIFY(editor.splitTargetAvailable());
    QCOMPARE(editor.targetWidth(), 2240);
    QCOMPARE(editor.targetHeight(), 1080);

    editor.setMode(QStringLiteral("Crop"));
    editor.setZoomPercent(250);
    editor.setFocusX(2000);
    editor.setFocusY(8000);
    editor.setRotation(90);
    editor.setPreparationTarget(QStringLiteral("SplitArea"));

    QCOMPARE(editor.preparationTarget(), QStringLiteral("SplitArea"));
    QCOMPARE(editor.targetWidth(), 1120);
    QCOMPARE(editor.targetHeight(), 1080);
    QCOMPARE(editor.mode(), QStringLiteral("Fit"));
    QCOMPARE(editor.rotation(), 0);
    QCOMPARE(editor.zoomPercent(), 100);
    QCOMPARE(editor.focusX(), 5000);
    QCOMPARE(editor.focusY(), 5000);
    QCOMPARE(editor.backgroundColor(), QStringLiteral("#000000"));
    const TryxRuntimeMediaPreparationProfileV1 split =
        editor.preparationProfile();
    QCOMPARE(split.schemaVersion, 1U);
    QCOMPARE(split.target, QStringLiteral("SplitArea"));
    QVERIFY(tryxMediaTransformIsLegacyFit(split.transform));

    editor.setMode(QStringLiteral("Crop"));
    editor.setZoomPercent(175);
    runtime.deviceCapabilities_.removeAll(
        tryxDeviceMediaSplitAreaV1Token());
    emit runtime.capabilitiesChanged();

    QVERIFY(!editor.splitTargetAvailable());
    QCOMPARE(editor.preparationTarget(), QStringLiteral("FullFrame"));
    QCOMPARE(editor.targetWidth(), 2240);
    QCOMPARE(editor.mode(), QStringLiteral("Fit"));
    QCOMPARE(editor.zoomPercent(), 100);
}

void QuickClientTests::
    pendingCapabilityLossResetsTargetAfterSubmissionFailure() {
    RuntimeClient runtime(true);
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeMediaPreparationProfileV1Token(),
    };
    runtime.deviceCapabilitiesReady_ = true;
    runtime.deviceCapabilities_ = {
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceMediaSplitAreaV1Token(),
    };
    runtime.connection_.printerClassDevicePresent = true;
    runtime.display_.valid = true;
    runtime.display_.deviceSerial = QStringLiteral("device-a");
    runtime.display_.screenMode = QStringLiteral("Screen Splitting");
    runtime.display_.media = {
        QStringLiteral("left.h264_1120x1080"),
        QStringLiteral("right.h264_1120x1080"),
    };
    MediaEditorController editor(&runtime);
    const TryxRuntimeDeviceMediaArtifact artifact = deviceMediaArtifact(
        QStringLiteral("stage-operation"),
        QStringLiteral("artifact-a"),
        QStringLiteral("media-a"),
        QStringLiteral("left.h264_1120x1080"),
        QStringLiteral("device-a"));

    const auto beginPendingSplit =
        [&runtime, &editor, &artifact](const QString &operationId) {
            editor.beginRecoveredVideo(artifact);
            QCOMPARE(editor.preparationTarget(),
                     QStringLiteral("SplitArea"));
            editor.setMode(QStringLiteral("Crop"));
            editor.setZoomPercent(175);
            editor.beginRecoveredSubmission(
                operationId, QStringLiteral("SaveAsNew"));
            QVERIFY(editor.submissionPending());

            runtime.deviceCapabilities_.removeAll(
                tryxDeviceMediaSplitAreaV1Token());
            emit runtime.capabilitiesChanged();
            QVERIFY(!editor.splitTargetAvailable());
            QCOMPARE(editor.preparationTarget(),
                     QStringLiteral("SplitArea"));
            QCOMPARE(editor.mode(), QStringLiteral("Crop"));
            QCOMPARE(editor.zoomPercent(), 175);
        };
    const auto verifyReset = [&editor]() {
        QVERIFY(!editor.submissionPending());
        QCOMPARE(editor.preparationTarget(),
                 QStringLiteral("FullFrame"));
        QCOMPARE(editor.targetWidth(), 2240);
        QCOMPARE(editor.targetHeight(), 1080);
        QCOMPARE(editor.mode(), QStringLiteral("Fit"));
        QCOMPARE(editor.zoomPercent(), 100);
        QCOMPARE(editor.focusX(), 5000);
        QCOMPARE(editor.focusY(), 5000);
        QCOMPARE(editor.rotation(), 0);
        QCOMPARE(editor.backgroundColor(), QStringLiteral("#000000"));
    };

    const QString localRejection =
        QStringLiteral("local-request-rejection");
    beginPendingSplit(localRejection);
    emit runtime.operationRequestRejected(
        localRejection, QStringLiteral("Upload"),
        QStringLiteral("request rejected"));
    verifyReset();

    runtime.deviceCapabilities_.append(
        tryxDeviceMediaSplitAreaV1Token());
    emit runtime.capabilitiesChanged();
    QVERIFY(editor.splitTargetAvailable());

    const QString recoveredFailure =
        QStringLiteral("recovered-operation-failure");
    beginPendingSplit(recoveredFailure);
    editor.finishRecoveredSubmission(
        recoveredFailure, false,
        QStringLiteral("operation failed"));
    verifyReset();
}

void QuickClientTests::
    recoveredTargetPreselectionRequiresExactActiveLayout() {
    RuntimeClient runtime(true);
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeMediaPreparationProfileV1Token(),
    };
    runtime.deviceCapabilitiesReady_ = true;
    runtime.deviceCapabilities_ = {
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceMediaSplitAreaV1Token(),
    };
    runtime.connection_.printerClassDevicePresent = true;
    runtime.display_.valid = true;
    runtime.display_.deviceSerial = QStringLiteral("device-a");
    runtime.display_.screenMode = QStringLiteral("Screen Splitting");
    runtime.display_.media = {
        QStringLiteral("left.h264_1120x1080"),
        QStringLiteral("right.h264_1120x1080"),
    };
    MediaEditorController editor(&runtime);

    TryxRuntimeDeviceMediaArtifact artifact = deviceMediaArtifact(
        QStringLiteral("stage-operation"),
        QStringLiteral("artifact-a"),
        QStringLiteral("media-a"),
        QStringLiteral("left.h264_1120x1080"),
        QStringLiteral("device-a"));
    editor.beginRecoveredVideo(artifact);
    QCOMPARE(editor.preparationTarget(), QStringLiteral("SplitArea"));
    QVERIFY(editor.replaceAllowed());

    editor.setPreparationTarget(QStringLiteral("FullFrame"));
    QVERIFY(!editor.replaceAllowed());
    QVERIFY(editor.replaceBlockReason().contains(
        QStringLiteral("target"), Qt::CaseInsensitive));

    runtime.display_.screenMode = QStringLiteral("Full Screen");
    emit runtime.displayChanged();
    editor.beginRecoveredVideo(artifact);
    QCOMPARE(editor.preparationTarget(), QStringLiteral("FullFrame"));
    QVERIFY(editor.replaceAllowed());

    artifact.deviceIdentity = QStringLiteral("device-b");
    editor.beginRecoveredVideo(artifact);
    QCOMPARE(editor.preparationTarget(), QStringLiteral("FullFrame"));
    QVERIFY(!editor.replaceAllowed());
}

void QuickClientTests::
    mediaPreparationProfileDispatchIsCapabilityBound() {
    const auto prepareRuntime = [](RuntimeClient *runtime) {
        runtime->serviceAvailable_ = true;
        runtime->compatible_ = true;
        runtime->connection_.printerClassDevicePresent = true;
        runtime->connection_.displaySessionActive = true;
    };
    TryxRuntimeMediaPreparationProfileV1 split;
    split.target = QStringLiteral("SplitArea");
    split.transform.mode = QStringLiteral("Crop");
    split.transform.zoomPermille = 1750;
    split.transform.focusX = 2500;
    split.transform.focusY = 7500;

    RuntimeClient unsupported(true);
    prepareRuntime(&unsupported);
    QVERIFY(unsupported.queueUploadWithPreparationProfile(
                QStringLiteral("/tmp/source.mp4"), split)
                .isEmpty());
    QVERIFY(unsupported.offlineRequests_.isEmpty());

    RuntimeClient modern(true);
    prepareRuntime(&modern);
    modern.capabilitiesReady_ = true;
    modern.runtimeCapabilities_ = {
        tryxRuntimeMediaPreparationProfileV1Token(),
    };
    modern.deviceCapabilitiesReady_ = true;
    modern.deviceCapabilities_ = {
        tryxDeviceDisplayConfigurationV1Token(),
        tryxDeviceMediaSplitAreaV1Token(),
    };

    QVERIFY(!modern.queueUploadWithPreparationProfile(
                 QStringLiteral("/tmp/source.mp4"), split)
                 .isEmpty());
    QVERIFY(!modern.queueRecoveredMediaUploadWithPreparationProfile(
                 QStringLiteral("artifact"), QStringLiteral("lease"),
                 split)
                 .isEmpty());
    TryxRuntimeApplyRequest request;
    request.screenMode = QStringLiteral("Screen Splitting");
    request.media = {
        QStringLiteral("left.h264_1120x1080"),
        QStringLiteral("right.h264_1120x1080"),
    };
    QVERIFY(!modern.queueReplaceDeviceMediaWithPreparationProfile(
                 QStringLiteral("artifact"), QStringLiteral("lease"),
                 QStringLiteral("media"), request, split)
                 .isEmpty());
    QCOMPARE(modern.offlineRequests_.size(), 3);
    QCOMPARE(modern.offlineRequests_.at(0).method,
             QStringLiteral("QueueUploadWithPreparationProfileV1"));
    QCOMPARE(modern.offlineRequests_.at(1).method,
             QStringLiteral(
                 "QueueRecoveredMediaUploadWithPreparationProfileV1"));
    QCOMPARE(modern.offlineRequests_.at(2).method,
             QStringLiteral(
                 "QueueReplaceDeviceMediaWithPreparationProfileV1"));
    for (const RuntimeClient::OfflineRequest &dispatched :
         modern.offlineRequests_) {
        const TryxRuntimeMediaPreparationProfileV1 actual =
            dispatched.arguments.constLast()
                .value<TryxRuntimeMediaPreparationProfileV1>();
        QCOMPARE(actual.target, QStringLiteral("SplitArea"));
        QCOMPARE(tryxMediaTransformCanonicalValue(actual.transform),
                 tryxMediaTransformCanonicalValue(split.transform));
    }

    RuntimeClient oldApi8(true);
    prepareRuntime(&oldApi8);
    oldApi8.capabilitiesReady_ = true;
    const TryxRuntimeMediaPreparationProfileV1 full =
        tryxFullFrameMediaPreparationProfile(split.transform);
    QVERIFY(!oldApi8.queueUploadWithPreparationProfile(
                 QStringLiteral("/tmp/source.mp4"), full)
                 .isEmpty());
    QCOMPARE(oldApi8.offlineRequests_.constLast().method,
             QStringLiteral("QueueUploadWithTransform"));
    const TryxRuntimeMediaTransform fallback =
        oldApi8.offlineRequests_.constLast().arguments.constLast()
            .value<TryxRuntimeMediaTransform>();
    QCOMPARE(tryxMediaTransformCanonicalValue(fallback),
             tryxMediaTransformCanonicalValue(full.transform));
}

void QuickClientTests::runtimeMediaTargetFollowsProductId() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    QVERIFY(runtime.productId().isEmpty());
    QCOMPARE(runtime.mediaTargetWidth(), 2240);
    QCOMPARE(runtime.mediaTargetHeight(), 1080);
    QCOMPARE(editor.targetWidth(), 2240);
    QCOMPARE(editor.targetHeight(), 1080);

    QSignalSpy targetChanged(
        &editor, &MediaEditorController::targetChanged);
    quint64 revision = 1;
    const QStringList panoramaProductIds{
        QStringLiteral("1011"),
        QStringLiteral("391a:1021"),
        QStringLiteral("unknown"),
    };
    for (const QString &productId : panoramaProductIds) {
        TryxRuntimeSnapshot snapshot;
        snapshot.revision = revision++;
        snapshot.productId = productId;
        runtime.applyConnectionSnapshot(snapshot);
        QCOMPARE(runtime.productId(), productId);
        QCOMPARE(runtime.mediaTargetWidth(), 2240);
        QCOMPARE(runtime.mediaTargetHeight(), 1080);
        QCOMPARE(editor.targetWidth(), 2240);
        QCOMPARE(editor.targetHeight(), 1080);
    }

    TryxRuntimeSnapshot turris;
    turris.revision = revision++;
    turris.productId = QStringLiteral("391a:2011");
    runtime.applyConnectionSnapshot(turris);
    QCOMPARE(runtime.productId(), QStringLiteral("391a:2011"));
    QCOMPARE(runtime.mediaTargetWidth(), 1280);
    QCOMPARE(runtime.mediaTargetHeight(), 720);
    QCOMPARE(editor.targetWidth(), 1280);
    QCOMPARE(editor.targetHeight(), 720);

    turris.revision = revision;
    turris.productId = QStringLiteral("0x2011");
    runtime.applyConnectionSnapshot(turris);
    QCOMPARE(runtime.mediaTargetWidth(), 1280);
    QCOMPARE(runtime.mediaTargetHeight(), 720);
    QCOMPARE(targetChanged.count(), 5);
}

void QuickClientTests::runtimeDeviceSummaryFollowsLiveSnapshot() {
    RuntimeClient runtime(true);

    QVERIFY(runtime.deviceModel().isEmpty());
    QVERIFY(runtime.firmwareVersion().isEmpty());
    QVERIFY(runtime.deviceAppVersion().isEmpty());
    QVERIFY(RuntimeClient::staticMetaObject.indexOfProperty("serial") < 0);
    QVERIFY(RuntimeClient::staticMetaObject.indexOfProperty("chipId") < 0);

    TryxRuntimeSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.connected = true;
    snapshot.productId = QStringLiteral("391a:1021");
    snapshot.firmware = QStringLiteral("PASE-FW-1");
    snapshot.appVersion = QStringLiteral("PASE-APP-2");
    runtime.applyConnectionSnapshot(snapshot);

    QCOMPARE(runtime.deviceModel(), QStringLiteral("PANORAMA SE"));
    QCOMPARE(runtime.productId(), QStringLiteral("391a:1021"));
    QCOMPARE(runtime.firmwareVersion(), QStringLiteral("PASE-FW-1"));
    QCOMPARE(runtime.deviceAppVersion(), QStringLiteral("PASE-APP-2"));

    TryxRuntimeSnapshot stale = snapshot;
    stale.productId = QStringLiteral("391a:2011");
    stale.firmware = QStringLiteral("stale-firmware");
    runtime.applyConnectionSnapshot(stale);
    QCOMPARE(runtime.deviceModel(), QStringLiteral("PANORAMA SE"));
    QCOMPARE(runtime.firmwareVersion(), QStringLiteral("PASE-FW-1"));

    snapshot.revision = 2;
    snapshot.productId = QStringLiteral("391a:1011");
    snapshot.firmware = QStringLiteral("PANORAMA-FW-3");
    snapshot.appVersion = QStringLiteral("PANORAMA-APP-4");
    runtime.applyConnectionSnapshot(snapshot);
    QCOMPARE(runtime.deviceModel(), QStringLiteral("PANORAMA"));
    QCOMPARE(runtime.firmwareVersion(), QStringLiteral("PANORAMA-FW-3"));
    QCOMPARE(runtime.deviceAppVersion(), QStringLiteral("PANORAMA-APP-4"));

    snapshot.revision = 3;
    snapshot.productId = QStringLiteral("0x2011");
    runtime.applyConnectionSnapshot(snapshot);
    QCOMPARE(runtime.deviceModel(), QStringLiteral("TURRIS 620"));

    snapshot.revision = 4;
    snapshot.productId = QStringLiteral("cm01");
    runtime.applyConnectionSnapshot(snapshot);
    QCOMPARE(runtime.deviceModel(), QStringLiteral("PANORAMA SE"));

    snapshot.revision = 5;
    snapshot.productId = QStringLiteral("cm01_se");
    snapshot.firmware = QStringLiteral(" unknown ");
    snapshot.appVersion = QStringLiteral("UNKNOWN");
    runtime.applyConnectionSnapshot(snapshot);
    QCOMPARE(runtime.deviceModel(), QStringLiteral("PANORAMA SE"));
    QVERIFY(runtime.firmwareVersion().isEmpty());
    QVERIFY(runtime.deviceAppVersion().isEmpty());

    snapshot.revision = 6;
    snapshot.productId = QStringLiteral("391a:ffff");
    runtime.applyConnectionSnapshot(snapshot);
    QVERIFY(runtime.deviceModel().isEmpty());

    snapshot.revision = 7;
    snapshot.connected = false;
    snapshot.productId.clear();
    snapshot.firmware.clear();
    snapshot.appVersion.clear();
    runtime.applyConnectionSnapshot(snapshot);
    QVERIFY(runtime.deviceModel().isEmpty());
    QVERIFY(runtime.firmwareVersion().isEmpty());
    QVERIFY(runtime.deviceAppVersion().isEmpty());
}

void QuickClientTests::
    offlinePrinterLifecycleEventsDoNotReadOwnerlessSnapshots() {
    RuntimeClient mediaRuntime(true);
    mediaRuntime.compatible_ = true;
    mediaRuntime.connection_.printerClassDevicePresent = true;
    mediaRuntime.connection_.displaySessionActive = false;

    mediaRuntime.onLegacyMediaListUpdated({}, 2);
    QCOMPARE(
        mediaRuntime.findChildren<QDBusPendingCallWatcher *>().size(), 0);

    RuntimeClient statusRuntime(true);
    statusRuntime.compatible_ = true;
    statusRuntime.connection_.printerClassDevicePresent = true;
    statusRuntime.connection_.displaySessionActive = false;

    statusRuntime.onLegacyUploadStatus(
        QStringLiteral("restricted recovery"), 3);
    QCOMPARE(
        statusRuntime.findChildren<QDBusPendingCallWatcher *>().size(), 0);

    RuntimeClient activeRuntime(true);
    activeRuntime.compatible_ = true;
    activeRuntime.connection_.printerClassDevicePresent = true;
    activeRuntime.connection_.displaySessionActive = true;

    activeRuntime.onLegacyMediaListUpdated({}, 4);
    activeRuntime.onLegacyUploadStatus(QStringLiteral("active"), 5);
    QCOMPARE(
        activeRuntime.findChildren<QDBusPendingCallWatcher *>().size(), 0);
}

void QuickClientTests::
    capabilityLifecycleSignalsInvalidatePresentationGates() {
    RuntimeClient runtime(true);
    runtime.compatible_ = true;
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeDeviceCapabilitiesV1Token(),
    };
    runtime.deviceCapabilitiesReady_ = true;
    runtime.deviceCapabilities_ = {
        tryxDeviceMediaUploadV1Token(),
    };
    runtime.connection_.revision = 1;
    runtime.connection_.connected = true;
    runtime.connection_.printerClassConnected = true;
    runtime.connection_.printerClassDevicePresent = true;
    runtime.connection_.displaySessionActive = true;
    runtime.connection_.serial = QStringLiteral("device-a");

    runtime.onPrinterPresenceChanged(false, false, 2);
    QVERIFY(runtime.capabilitiesReady());
    QVERIFY(!runtime.deviceCapabilitiesReady());
    QCOMPARE(runtime.deviceCapabilities(), QStringList());

    runtime.deviceCapabilitiesReady_ = true;
    runtime.deviceCapabilities_ = {
        tryxDeviceMediaUploadV1Token(),
    };
    runtime.onDeviceConnected(
        QStringLiteral("391a:1021"),
        QStringLiteral("device-b"), QString(), QString(),
        true, true, 3);
    QVERIFY(runtime.capabilitiesReady());
    QVERIFY(!runtime.deviceCapabilitiesReady());
    QCOMPARE(runtime.deviceCapabilities(), QStringList());
}

void QuickClientTests::
    savedLayoutModelUsesExactDecimalRevisionAndFullStateDto() {
    const TryxRuntimeSavedLayoutV1 layout =
        canonicalSavedLayoutForTest();
    RuntimeClient runtime(true);
    configureSavedLayoutRuntime(&runtime, layout);
    QVERIFY(runtime.applySavedLayoutsSnapshot(
        savedLayoutsSnapshotForTest(layout)));

    SavedLayoutListModel *model = runtime.savedLayoutModel();
    QCOMPARE(model->rowCount(), 1);
    const QModelIndex index = model->index(0, 0);
    const QVariant revision = model->data(
        index, SavedLayoutListModel::RevisionRole);
    QCOMPARE(revision.metaType().id(), QMetaType::QString);
    QCOMPARE(revision.toString(), QString::number(layout.revision));
    QCOMPARE(model->data(index, SavedLayoutListModel::LayoutIdRole),
             QVariant(layout.layoutId));
    QCOMPARE(model->data(index, SavedLayoutListModel::NameRole),
             QVariant(layout.name));
    QCOMPARE(model->data(index, SavedLayoutListModel::ScreenModeRole),
             QVariant(layout.request.screenMode));
    QCOMPARE(model->data(index, SavedLayoutListModel::MediaNamesRole),
             QVariant(layout.request.media));

    const QVariantMap dto = runtime.savedLayoutDraft(layout.layoutId);
    QCOMPARE(dto.size(), 7);
    QCOMPARE(dto.value(QStringLiteral("layoutId")).toString(),
             layout.layoutId);
    QCOMPARE(dto.value(QStringLiteral("revision")).metaType().id(),
             QMetaType::QString);
    QCOMPARE(dto.value(QStringLiteral("revision")).toString(),
             QString::number(layout.revision));
    QCOMPARE(dto.value(QStringLiteral("deviceIdentity")).toString(),
             layout.deviceIdentity);
    QCOMPARE(dto.value(QStringLiteral("name")).toString(), layout.name);

    const QVariantMap layoutDto =
        dto.value(QStringLiteral("layout")).toMap();
    QCOMPARE(layoutDto.size(), 8);
    QCOMPARE(layoutDto.value(QStringLiteral("split")).metaType().id(),
             QMetaType::Bool);
    QCOMPARE(layoutDto.value(QStringLiteral("split")).toBool(), false);
    QCOMPARE(layoutDto.value(QStringLiteral("media")).toStringList(),
             layout.request.media);
    QCOMPARE(layoutDto.value(QStringLiteral("playMode")).toString(),
             layout.request.playMode);
    QCOMPARE(layoutDto.value(QStringLiteral("metrics")).toStringList(),
             layout.request.sysinfoLabels);
    QCOMPARE(layoutDto.value(QStringLiteral("badges")).toStringList(),
             layout.request.settingsBadges);
    QCOMPARE(layoutDto.value(QStringLiteral("position")).toString(),
             layout.request.settingsPosition);
    QCOMPARE(layoutDto.value(QStringLiteral("color")).toString(),
             layout.request.settingsColor);
    QCOMPARE(layoutDto.value(QStringLiteral("alignment")).toString(),
             layout.request.settingsAlign);

    QCOMPARE(dto.value(QStringLiteral("brightness")).toInt(),
             layout.request.display.brightness);
    const QVariantMap orientation =
        dto.value(QStringLiteral("orientation")).toMap();
    QCOMPARE(orientation.size(), 2);
    QCOMPARE(orientation.value(QStringLiteral("mirror")).toBool(),
             layout.request.display.mirrorMode);
    QCOMPARE(orientation.value(QStringLiteral("waterfall")).toBool(),
             layout.request.display.waterfallMode);

    TryxRuntimeApplyRequest roundTrip;
    QString validationError;
    QVERIFY2(runtime.fullSavedLayoutDraftToRequest(
                 savedLayoutFullStateForTest(dto), &roundTrip,
                 &validationError),
             qPrintable(validationError));
    QVERIFY(roundTrip == layout.request);

    quint64 parsed = 0;
    QVERIFY(RuntimeClient::parseSavedLayoutRevision(
        QString::number(layout.revision), &parsed));
    QCOMPARE(parsed, layout.revision);
    QVERIFY(!RuntimeClient::parseSavedLayoutRevision(
        QStringLiteral("01"), &parsed));
    QVERIFY(!RuntimeClient::parseSavedLayoutRevision(
        QStringLiteral("+1"), &parsed));
    QVERIFY(!RuntimeClient::parseSavedLayoutRevision(
        QStringLiteral("1.0"), &parsed));
    QVERIFY(!RuntimeClient::parseSavedLayoutRevision(
        QStringLiteral("18446744073709551616"), &parsed));
}

void QuickClientTests::customSavedLayoutUsesVersionedContract_data() {
    QTest::addColumn<QString>("action");
    for (const char *name : {"put", "delete", "apply", "invalid-snapshot"}) QTest::newRow(name) << QString::fromLatin1(name);
}

void QuickClientTests::customSavedLayoutUsesVersionedContract() {
    QFETCH(QString, action);
    const auto legacy = canonicalSavedLayoutForTest();
    RuntimeClient runtime(true);
    configureSavedLayoutRuntime(&runtime, legacy);
    runtime.runtimeCapabilities_.append({tryxRuntimeSavedLayoutsV2Token(), tryxRuntimeApplyWithBadgesV1Token(), tryxRuntimeDisplaySnapshotV1Token()});
    runtime.deviceCapabilities_.append(tryxDeviceOverlayBadgeTextV1Token());
    runtime.deviceCapabilitiesSnapshot_ = {1, legacy.deviceIdentity, 17, 3, runtime.deviceCapabilities_};
    auto layout = tryxSavedLayoutV2FromV1(legacy);
    layout.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Saved text")};
    TryxRuntimeSavedLayoutsSnapshotV2 snapshot{2, legacy.revision + 1, QStringLiteral("Ready"), {}, legacy.deviceIdentity, legacy.productId, {layout}};
    if (action == QStringLiteral("invalid-snapshot")) {
        snapshot.layouts[0].badges.secondaryCpu = {QStringLiteral("Custom"), QStringLiteral("hidden")};
        QVERIFY(!runtime.applySavedLayoutsSnapshot(snapshot));
        QVERIFY(runtime.offlineRequests_.isEmpty());
        return;
    }
    QVERIFY(runtime.applySavedLayoutsSnapshot(snapshot));
    auto fullDraft = savedLayoutFullStateForTest(runtime.savedLayoutDraft(layout.layoutId));
    auto draft = fullDraft.value(QStringLiteral("layout")).toMap();
    QCOMPARE(draft.value(QStringLiteral("badgeChoices")).toMap(), tryxOverlayBadgesV1ToJson(layout.badges).toVariantMap());
    layout.badges.primaryCpu.text = QStringLiteral("Current draft");
    draft.insert(QStringLiteral("badgeChoices"), tryxOverlayBadgesV1ToJson(layout.badges).toVariantMap());
    fullDraft.insert(QStringLiteral("layout"), draft);
    TryxRuntimeDisplaySnapshotV1 display;
    display.revision = 41;
    display.connectionRevision = 17;
    display.physicalGeneration = 3;
    display.productId = legacy.productId;
    display.status = QStringLiteral("HostAccepted");
    display.display = confirmedDisplayState(legacy.request, 41);
    QVERIFY(runtime.applyDisplaySnapshotV1(display));
    if (action == QStringLiteral("put")) {
        runtime.putSavedLayout(legacy.name, legacy.layoutId, fullDraft);
        QCOMPARE(runtime.offlineRequests_.size(), 1);
        const auto call = runtime.offlineRequests_.first();
        QCOMPARE(call.method, QStringLiteral("PutSavedLayoutV2"));
        QCOMPARE(call.arguments.at(0).toULongLong(), snapshot.revision);
        QCOMPARE(call.arguments.at(1).value<TryxRuntimeSavedLayoutV2>().badges, layout.badges);
        QVERIFY(!runtime.displaySubmission_.active());
    } else if (action == QStringLiteral("delete")) {
        runtime.deleteSavedLayout(legacy.layoutId);
        QCOMPARE(runtime.offlineRequests_.size(), 1);
        QCOMPARE(runtime.offlineRequests_.first().method, QStringLiteral("DeleteSavedLayoutV2"));
        QVERIFY(!runtime.displaySubmission_.active());
    } else {
        const auto id = runtime.submitSavedLayoutDraft(legacy.layoutId, QString::number(legacy.revision), fullDraft);
        QVERIFY(!id.isEmpty());
        QCOMPARE(runtime.offlineRequests_.size(), 1);
        const auto call = runtime.offlineRequests_.first();
        QCOMPARE(call.method, QStringLiteral("QueueSavedLayoutApplyV2"));
        QCOMPARE(call.arguments.at(3).value<TryxRuntimeApplyWithBadgesV1>().badges, layout.badges);
        QCOMPARE(call.arguments.at(1).toString(), legacy.layoutId);
        QCOMPARE(call.arguments.at(2).toULongLong(), legacy.revision);
    }
}

void QuickClientTests::savedLayoutsSnapshotValidationFailsClosed() {
    const TryxRuntimeSavedLayoutV1 layout =
        canonicalSavedLayoutForTest();
    RuntimeClient runtime(true);
    configureSavedLayoutRuntime(&runtime, layout);
    const TryxRuntimeSavedLayoutsSnapshotV1 valid =
        savedLayoutsSnapshotForTest(layout);
    QVERIFY(runtime.applySavedLayoutsSnapshot(valid));
    QVERIFY(runtime.savedLayoutsReady());
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 1);

    TryxRuntimeSavedLayoutsSnapshotV1 malformed = valid;
    malformed.revision++;
    malformed.layouts[0].request.settingsColor =
        QStringLiteral("#A1B2C3");
    QString error;
    QVERIFY(!runtime.applySavedLayoutsSnapshot(malformed, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(runtime.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QVERIFY(!runtime.savedLayoutsDiagnostic().isEmpty());
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);
    QVERIFY(runtime.savedLayoutDraft(layout.layoutId).isEmpty());

    QVERIFY(runtime.applySavedLayoutsSnapshot(valid));
    TryxRuntimeSavedLayoutsSnapshotV1 duplicate = valid;
    duplicate.revision += 2;
    TryxRuntimeSavedLayoutV1 sameName = layout;
    sameName.layoutId =
        QStringLiteral("fedcba98-7654-4abc-8def-0123456789ab");
    sameName.revision++;
    sameName.name = QStringLiteral("QUIET MONITORING");
    duplicate.layouts.append(sameName);
    QVERIFY(!runtime.applySavedLayoutsSnapshot(duplicate, &error));
    QCOMPARE(runtime.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);

    TryxRuntimeSavedLayoutsSnapshotV1 unavailable;
    unavailable.revision = valid.revision + 3;
    unavailable.status = QStringLiteral("Unavailable");
    QVERIFY(!runtime.applySavedLayoutsSnapshot(unavailable, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);
}

void QuickClientTests::
    savedLayoutsRequireCapabilityAndExactDevice() {
    RuntimeClient runtime(true);
    QCOMPARE(runtime.savedLayoutsStatus(), QStringLiteral("NotSupported"));
    QVERIFY(!runtime.savedLayoutsSupported());
    runtime.refreshSavedLayouts();
    QVERIFY(runtime.offlineRequests_.isEmpty());

    const TryxRuntimeSavedLayoutV1 layout =
        canonicalSavedLayoutForTest();
    configureSavedLayoutRuntime(&runtime, layout);
    QVERIFY(runtime.savedLayoutsSupported());

    TryxRuntimeSavedLayoutV1 foreignDevice = layout;
    foreignDevice.deviceIdentity = QStringLiteral("device-b");
    for (TryxRuntimeSavedMediaRefV1 &reference : foreignDevice.media) {
        reference.mediaId = savedLayoutMediaIdForTest(
            foreignDevice.deviceIdentity, reference);
    }
    QString error;
    QVERIFY(!runtime.applySavedLayoutsSnapshot(
        savedLayoutsSnapshotForTest(foreignDevice), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!runtime.savedLayoutsReady());
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);

    TryxRuntimeSavedLayoutV1 foreignProduct = layout;
    foreignProduct.productId = QStringLiteral("391a:1011");
    QVERIFY(!runtime.applySavedLayoutsSnapshot(
        savedLayoutsSnapshotForTest(foreignProduct), &error));
    QVERIFY(!runtime.savedLayoutsReady());
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);

    const TryxRuntimeSavedLayoutsSnapshotV1 valid =
        savedLayoutsSnapshotForTest(layout);
    QVERIFY(runtime.applySavedLayoutsSnapshot(valid));
    QVERIFY(runtime.savedLayoutsReady());

    TryxRuntimeSnapshot changed = runtime.connection_;
    changed.revision++;
    changed.serial = QStringLiteral("device-b");
    runtime.applyConnectionSnapshot(changed);
    QVERIFY(!runtime.savedLayoutsReady());
    QCOMPARE(runtime.savedLayoutsStatus(), QStringLiteral("Unavailable"));
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);

    runtime.connection_.serial = layout.deviceIdentity;
    QVERIFY(runtime.applySavedLayoutsSnapshot(valid));
    runtime.clearCapabilityState();
    QVERIFY(!runtime.savedLayoutsSupported());
    QVERIFY(!runtime.savedLayoutsReady());
    QCOMPARE(runtime.savedLayoutsStatus(), QStringLiteral("NotSupported"));
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 0);
}

void QuickClientTests::
    savedLayoutSaveQueuesOnlyCanonicalStoreCas() {
    const TryxRuntimeSavedLayoutV1 layout =
        canonicalSavedLayoutForTest();
    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        savedLayoutsSnapshotForTest(layout);
    RuntimeClient runtime(true);
    configureSavedLayoutRuntime(&runtime, layout);
    QVERIFY(runtime.applySavedLayoutsSnapshot(snapshot));

    QVariantMap fullDraft = savedLayoutFullStateForTest(
        runtime.savedLayoutDraft(layout.layoutId));
    QVariantMap layoutDraft =
        fullDraft.value(QStringLiteral("layout")).toMap();
    layoutDraft.insert(QStringLiteral("metrics"), QStringList{
        QStringLiteral("CPU Temperature"),
        QStringLiteral("GPU Usage"),
    });
    layoutDraft.insert(QStringLiteral("color"),
                       QStringLiteral("#abcdef"));
    fullDraft.insert(QStringLiteral("layout"), layoutDraft);
    fullDraft.insert(QStringLiteral("brightness"), 88);
    QVariantMap orientation =
        fullDraft.value(QStringLiteral("orientation")).toMap();
    orientation.insert(QStringLiteral("mirror"), false);
    orientation.insert(QStringLiteral("waterfall"), false);
    fullDraft.insert(QStringLiteral("orientation"), orientation);

    TryxRuntimeApplyRequest expectedRequest;
    QString error;
    QVERIFY2(runtime.fullSavedLayoutDraftToRequest(
                 fullDraft, &expectedRequest, &error),
             qPrintable(error));
    QSignalSpy applyStarted(
        &runtime, &RuntimeClient::displayApplyStarted);

    runtime.putSavedLayout(
        QStringLiteral("Edited monitoring"), {}, fullDraft);

    QCOMPARE(applyStarted.count(), 0);
    QVERIFY(!runtime.displaySubmission_.active());
    QVERIFY(!runtime.displayApplyDeadline_.isActive());
    QVERIFY(!runtime.operationBusy());
    QVERIFY(runtime.savedLayoutsBusy());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const RuntimeClient::OfflineRequest call =
        runtime.offlineRequests_.constFirst();
    QCOMPARE(call.method, QStringLiteral("PutSavedLayoutV1"));
    QVERIFY(call.operationId.isEmpty());
    QCOMPARE(call.kind, QStringLiteral("PutSavedLayoutV1"));
    QCOMPARE(call.arguments.size(), 2);
    QCOMPARE(call.arguments.at(0).metaType().id(),
             QMetaType::ULongLong);
    QCOMPARE(call.arguments.at(0).toULongLong(), snapshot.revision);
    QCOMPARE(call.arguments.at(1).metaType().id(),
             QMetaType::fromType<TryxRuntimeSavedLayoutV1>().id());
    const TryxRuntimeSavedLayoutV1 queued =
        call.arguments.at(1).value<TryxRuntimeSavedLayoutV1>();
    QCOMPARE(queued.schemaVersion, 1U);
    QVERIFY(queued.layoutId.isEmpty());
    QCOMPARE(queued.revision, quint64(0));
    QCOMPARE(queued.deviceIdentity, layout.deviceIdentity);
    QCOMPARE(queued.productId, layout.productId);
    QCOMPARE(queued.name, QStringLiteral("Edited monitoring"));
    QVERIFY(queued.media == layout.media);
    QVERIFY(queued.request == expectedRequest);
    QCOMPARE(runtime.savedLayouts_.revision, snapshot.revision);
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 1);
}

void QuickClientTests::savedLayoutDeleteQueuesOnlyStoreCas() {
    const TryxRuntimeSavedLayoutV1 layout =
        canonicalSavedLayoutForTest();
    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        savedLayoutsSnapshotForTest(layout);
    RuntimeClient runtime(true);
    configureSavedLayoutRuntime(&runtime, layout);
    QVERIFY(runtime.applySavedLayoutsSnapshot(snapshot));
    QSignalSpy applyStarted(
        &runtime, &RuntimeClient::displayApplyStarted);

    runtime.deleteSavedLayout(layout.layoutId);

    QCOMPARE(applyStarted.count(), 0);
    QVERIFY(!runtime.displaySubmission_.active());
    QVERIFY(!runtime.displayApplyDeadline_.isActive());
    QVERIFY(!runtime.operationBusy());
    QVERIFY(runtime.savedLayoutsBusy());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const RuntimeClient::OfflineRequest call =
        runtime.offlineRequests_.constFirst();
    QCOMPARE(call.method, QStringLiteral("DeleteSavedLayoutV1"));
    QVERIFY(call.operationId.isEmpty());
    QCOMPARE(call.kind, QStringLiteral("DeleteSavedLayoutV1"));
    QCOMPARE(call.arguments.size(), 2);
    QCOMPARE(call.arguments.at(0).metaType().id(),
             QMetaType::ULongLong);
    QCOMPARE(call.arguments.at(0).toULongLong(), snapshot.revision);
    QCOMPARE(call.arguments.at(1).toString(), layout.layoutId);
    QCOMPARE(runtime.savedLayouts_.revision, snapshot.revision);
    QCOMPARE(runtime.savedLayouts_.layouts.size(), 1);
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 1);
}

void QuickClientTests::
    savedLayoutApplyKeepsProvenanceAndDisplayLifecycle() {
    const TryxRuntimeSavedLayoutV1 layout =
        canonicalSavedLayoutForTest();
    const TryxRuntimeSavedLayoutsSnapshotV1 snapshot =
        savedLayoutsSnapshotForTest(layout);
    RuntimeClient runtime(true);
    configureSavedLayoutRuntime(&runtime, layout);
    QVERIFY(runtime.applySavedLayoutsSnapshot(snapshot));

    QVariantMap fullDraft = savedLayoutFullStateForTest(
        runtime.savedLayoutDraft(layout.layoutId));
    QVariantMap layoutDraft =
        fullDraft.value(QStringLiteral("layout")).toMap();
    layoutDraft.insert(QStringLiteral("metrics"), QStringList{
        QStringLiteral("CPU Power"),
        QStringLiteral("GPU Usage"),
    });
    layoutDraft.insert(QStringLiteral("badges"), QStringList{
        QStringLiteral("CPU Badge"),
    });
    layoutDraft.insert(QStringLiteral("position"),
                       QStringLiteral("Top"));
    layoutDraft.insert(QStringLiteral("color"),
                       QStringLiteral("#0a1b2c"));
    layoutDraft.insert(QStringLiteral("alignment"),
                       QStringLiteral("Right"));
    fullDraft.insert(QStringLiteral("layout"), layoutDraft);
    fullDraft.insert(QStringLiteral("brightness"), 64);
    QVariantMap orientation =
        fullDraft.value(QStringLiteral("orientation")).toMap();
    orientation.insert(QStringLiteral("mirror"), false);
    orientation.insert(QStringLiteral("waterfall"), false);
    fullDraft.insert(QStringLiteral("orientation"), orientation);

    TryxRuntimeApplyRequest expectedRequest;
    QString error;
    QVERIFY2(runtime.fullSavedLayoutDraftToRequest(
                 fullDraft, &expectedRequest, &error),
             qPrintable(error));
    QSignalSpy started(
        &runtime, &RuntimeClient::displayApplyStarted);
    QSignalSpy finished(
        &runtime, &RuntimeClient::displayApplyFinished);

    const QString operationId = runtime.submitSavedLayoutDraft(
        layout.layoutId, QString::number(layout.revision), fullDraft);

    QVERIFY(!operationId.isEmpty());
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.constFirst().at(0).toString(), operationId);
    QVERIFY(runtime.operationBusy());
    QCOMPARE(runtime.activeOperationId(), operationId);
    QVERIFY(runtime.displaySubmission_.active());
    QCOMPARE(runtime.displaySubmission_.deviceIdentity,
             layout.deviceIdentity);
    QCOMPARE(runtime.displaySubmission_.startingDisplayRevision,
             quint64(41));
    QVERIFY(runtime.displaySubmission_.layoutPresent);
    QVERIFY(runtime.displaySubmission_.brightnessPresent);
    QVERIFY(runtime.displaySubmission_.orientationPresent);
    QVERIFY(runtime.displaySubmission_.request == expectedRequest);
    QVERIFY(runtime.displayApplyDeadline_.isActive());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const RuntimeClient::OfflineRequest call =
        runtime.offlineRequests_.constFirst();
    QCOMPARE(call.method,
             QStringLiteral("QueueSavedLayoutApplyV1"));
    QCOMPARE(call.operationId, operationId);
    QCOMPARE(call.kind, QStringLiteral("SavedLayoutApply"));
    QCOMPARE(call.arguments.size(), 4);
    QCOMPARE(call.arguments.at(0).toString(), operationId);
    QCOMPARE(call.arguments.at(1).toString(), layout.layoutId);
    QCOMPARE(call.arguments.at(2).metaType().id(),
             QMetaType::ULongLong);
    QCOMPARE(call.arguments.at(2).toULongLong(), layout.revision);
    QCOMPARE(call.arguments.at(3).metaType().id(),
             QMetaType::fromType<TryxRuntimeApplyRequest>().id());
    QVERIFY(call.arguments.at(3).value<TryxRuntimeApplyRequest>() ==
            expectedRequest);

    TryxRuntimeOperationInfo succeeded;
    succeeded.id = operationId;
    succeeded.kind = QStringLiteral("SavedLayoutApply");
    succeeded.state = QStringLiteral("Succeeded");
    succeeded.stage = QStringLiteral("Completed");
    succeeded.terminalOutcome = QStringLiteral("Completed");
    runtime.onOperationChanged(succeeded, 1);
    QCOMPARE(finished.count(), 0);
    QVERIFY(runtime.operationBusy());

    runtime.applyDisplayState(
        confirmedDisplayState(expectedRequest, 42));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.constFirst().at(0).toString(), operationId);
    QCOMPARE(finished.constFirst().at(1).toString(),
             QStringLiteral("Succeeded"));
    QVERIFY(!runtime.operationBusy());
    QVERIFY(!runtime.displaySubmission_.active());
    QVERIFY(!runtime.displayApplyDeadline_.isActive());
    QCOMPARE(runtime.savedLayouts_.revision, snapshot.revision);
    QCOMPARE(runtime.savedLayoutModel()->rowCount(), 1);
    QCOMPARE(runtime.savedLayoutDraft(layout.layoutId)
                 .value(QStringLiteral("revision")).toString(),
             QString::number(layout.revision));
}

void QuickClientTests::nonCropFieldsAreNeutral() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    editor.setMode(QStringLiteral("Crop"));
    editor.setZoomPercent(275);
    editor.setFocusX(1500);
    editor.setFocusY(8500);
    editor.setBackgroundColor(QStringLiteral("#aabbcc"));
    editor.setMode(QStringLiteral("Fill"));

    const TryxRuntimeMediaTransform transform = editor.transform();
    QCOMPARE(transform.mode, QStringLiteral("Fill"));
    QCOMPARE(transform.zoomPermille, 1000U);
    QCOMPARE(transform.focusX, 5000U);
    QCOMPARE(transform.focusY, 5000U);
    QCOMPARE(transform.backgroundRgb, 0U);
}

void QuickClientTests::cropRotationResetsViewport() {
    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);

    editor.setMode(QStringLiteral("Crop"));
    editor.setZoomPercent(400);
    editor.setFocusX(0);
    editor.setFocusY(10000);
    editor.setRotation(90);

    const TryxRuntimeMediaTransform transform = editor.transform();
    QCOMPARE(transform.mode, QStringLiteral("Crop"));
    QCOMPARE(transform.rotationQuarterTurns, 1U);
    QCOMPARE(transform.zoomPermille, 1000U);
    QCOMPARE(transform.focusX, 5000U);
    QCOMPARE(transform.focusY, 5000U);
}

void QuickClientTests::previewUsesCanonicalTransformFilter() {
    TryxRuntimeMediaTransform transform;
    transform.mode = QStringLiteral("Crop");
    transform.rotationQuarterTurns = 1;
    transform.zoomPermille = 4000;
    transform.focusX = 10000;
    transform.focusY = 0;

    const QString canonical =
        tryxMediaTransformFfmpegFilter(
            transform, kTryxMediaTargetWidth,
            kTryxMediaTargetHeight);
    QVERIFY(!canonical.isEmpty());
    QCOMPARE(
        MediaPreviewController::previewFilter(transform),
        canonical +
            QStringLiteral(",scale=1120:540:flags=lanczos"));
    const QString turrisCanonical =
        tryxMediaTransformFfmpegFilter(transform, 1280, 720);
    QVERIFY(!turrisCanonical.isEmpty());
    QCOMPARE(
        MediaPreviewController::previewFilter(
            transform, 1280, 720),
        turrisCanonical +
            QStringLiteral(",scale=640:360:flags=lanczos"));
    QVERIFY(canonical.startsWith(
        QStringLiteral("transpose=clock,")));
    QVERIFY(canonical.contains(
        QStringLiteral(
            "crop=2240:1080:"
            "'trunc((iw-2240)*10000/10000/2)*2':"
            "'trunc((ih-1080)*0/10000/2)*2'")));
    QVERIFY(turrisCanonical.contains(
        QStringLiteral(
            "crop=1280:720:"
            "'trunc((iw-1280)*10000/10000/2)*2':"
            "'trunc((ih-720)*0/10000/2)*2'")));

    for (quint32 rotation = 0; rotation < 4; ++rotation) {
        transform.rotationQuarterTurns = rotation;
        QVERIFY(!MediaPreviewController::previewFilter(
                     transform)
                     .isEmpty());
        QVERIFY(!MediaPreviewController::previewFilter(
                     transform, 1280, 720)
                     .isEmpty());
    }
}

void QuickClientTests::
    renderedTransformPreservesDisplayGeometry() {
    const QString ffmpeg = QStandardPaths::findExecutable(
        QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QSKIP("ffmpeg is optional for the unit-test environment");
    }

    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const auto render = [&](
                            const QString &source,
                            const TryxRuntimeMediaTransform &transform,
                            const QString &outputPath) {
        QProcess process;
        process.setProgram(ffmpeg);
        process.setArguments({
            QStringLiteral("-nostdin"),
            QStringLiteral("-hide_banner"),
            QStringLiteral("-loglevel"),
            QStringLiteral("error"),
            QStringLiteral("-y"),
            QStringLiteral("-f"),
            QStringLiteral("lavfi"),
            QStringLiteral("-i"),
            source,
            QStringLiteral("-frames:v"),
            QStringLiteral("1"),
            QStringLiteral("-vf"),
            tryxMediaTransformFfmpegFilter(transform),
            outputPath,
        });
        process.start();
        if (!process.waitForStarted(5000)) {
            return process.errorString().toUtf8();
        }
        if (!process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished(5000);
            return QByteArray("ffmpeg render timed out");
        }
        const QByteArray diagnostic = process.readAll();
        if (process.exitStatus() != QProcess::NormalExit ||
            process.exitCode() != 0) {
            return diagnostic.isEmpty()
                ? QByteArray("ffmpeg render failed")
                : diagnostic;
        }
        return QByteArray();
    };

    TryxRuntimeMediaTransform fit =
        tryxLegacyFitMediaTransform();
    fit.backgroundRgb = 0x00FF00;
    const QString fitPath =
        QDir(directory.path()).filePath(
            QStringLiteral("anamorphic-fit.png"));
    QByteArray diagnostic = render(
        QStringLiteral(
            "color=c=red:s=720x576:d=1,setsar=64/45"),
        fit, fitPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());

    const QImage fitImage(fitPath);
    QVERIFY(!fitImage.isNull());
    QCOMPARE(
        fitImage.size(),
        QSize(kTryxMediaTargetWidth,
              kTryxMediaTargetHeight));
    const QColor leftPadding = fitImage.pixelColor(80, 540);
    const QColor center = fitImage.pixelColor(1120, 540);
    const QColor rightPadding = fitImage.pixelColor(2160, 540);
    QVERIFY(leftPadding.green() > 160);
    QVERIFY(leftPadding.red() < 100);
    QVERIFY(center.red() > 160);
    QVERIFY(center.green() < 100);
    QVERIFY(rightPadding.green() > 160);
    QVERIFY(rightPadding.red() < 100);

    TryxRuntimeMediaTransform rotatedFit = fit;
    rotatedFit.rotationQuarterTurns = 1;
    const QString rotatedPath =
        QDir(directory.path()).filePath(
            QStringLiteral("anamorphic-rotated-fit.png"));
    diagnostic = render(
        QStringLiteral(
            "color=c=red:s=720x576:d=1,setsar=64/45"),
        rotatedFit, rotatedPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());

    const QImage rotatedImage(rotatedPath);
    QVERIFY(!rotatedImage.isNull());
    const QColor rotatedPadding =
        rotatedImage.pixelColor(740, 540);
    const QColor rotatedCenter =
        rotatedImage.pixelColor(1120, 540);
    QVERIFY(rotatedPadding.green() > 160);
    QVERIFY(rotatedPadding.red() < 100);
    QVERIFY(rotatedCenter.red() > 160);
    QVERIFY(rotatedCenter.green() < 100);

    TryxRuntimeMediaTransform fill =
        tryxLegacyFitMediaTransform();
    fill.mode = QStringLiteral("Fill");
    TryxRuntimeMediaTransform neutralCrop = fill;
    neutralCrop.mode = QStringLiteral("Crop");
    QCOMPARE(
        tryxMediaTransformFfmpegFilter(fill),
        tryxMediaTransformFfmpegFilter(neutralCrop));

    const QString fillPath =
        QDir(directory.path()).filePath(
            QStringLiteral("fill.png"));
    const QString cropPath =
        QDir(directory.path()).filePath(
            QStringLiteral("neutral-crop.png"));
    diagnostic = render(
        QStringLiteral(
            "testsrc=size=333x1000:rate=1:duration=1"),
        fill, fillPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());
    diagnostic = render(
        QStringLiteral(
            "testsrc=size=333x1000:rate=1:duration=1"),
        neutralCrop, cropPath);
    QVERIFY2(diagnostic.isEmpty(), diagnostic.constData());

    const QImage fillImage(fillPath);
    const QImage cropImage(cropPath);
    QVERIFY(!fillImage.isNull());
    QVERIFY(!cropImage.isNull());
    QCOMPARE(fillImage, cropImage);
}

void QuickClientTests::previewGenerationIsDebounced() {
    MediaPreviewController preview;
    const quint64 initialGeneration =
        preview.requestedRenderGeneration_;

    TryxRuntimeMediaTransform transform;
    transform.mode = QStringLiteral("Fill");
    preview.setTransform(transform);
    QCOMPARE(
        preview.requestedRenderGeneration_,
        initialGeneration + 1);
    QVERIFY(!preview.renderDebounce_.isActive());

    preview.setTransform(transform);
    QCOMPARE(
        preview.requestedRenderGeneration_,
        initialGeneration + 1);

    transform.rotationQuarterTurns = 3;
    preview.setTransform(transform);
    QCOMPARE(
        preview.requestedRenderGeneration_,
        initialGeneration + 2);
}

void QuickClientTests::
    previewCleanupIsAllowlistedAndInterlocked() {
    QTemporaryDir runtimeDirectory;
    QVERIFY(runtimeDirectory.isValid());
    QVERIFY(QFile::setPermissions(
        runtimeDirectory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    ScopedEnvironmentVariable runtimeRoot("XDG_RUNTIME_DIR");
    runtimeRoot.set(QFile::encodeName(runtimeDirectory.path()));

    const QString previewDirectory =
        QDir(runtimeDirectory.path()).filePath(
            QStringLiteral("tryx-panorama-manager/previews"));
    QString directoryError;
    QVERIFY2(
        MediaPreviewController::ensurePrivateDirectoryTree(
            previewDirectory, &directoryError),
        qPrintable(directoryError));

    const QString first = QDir(previewDirectory).filePath(
        QStringLiteral(
            "11111111-1111-4111-8111-111111111111-1.png"));
    const QString second = QDir(previewDirectory).filePath(
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222-2.png"));
    const QString unrelated = QDir(previewDirectory).filePath(
        QStringLiteral("keep.png"));
    const QByteArray firstBytes("first-preview");
    const QByteArray secondBytes("second-preview-bytes");
    for (const auto &file : {
             qMakePair(first, firstBytes),
             qMakePair(second, secondBytes),
             qMakePair(unrelated, QByteArrayLiteral("keep"))}) {
        QFile output(file.first);
        QVERIFY(output.open(
            QIODevice::WriteOnly | QIODevice::NewOnly));
        QCOMPARE(output.write(file.second), file.second.size());
        output.close();
        QVERIFY(QFile::setPermissions(
            file.first,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    MediaPreviewController preview;
    int cleanupSyncCalls = 0;
    bool identitiesPinnedDuringRemoval = true;
    preview.cleanupUnlinkFunctionForTesting_ =
        [&identitiesPinnedDuringRemoval](
            int directoryDescriptor, const QByteArray &name) {
            struct stat parent {};
            struct stat leaf {};
            if (::fstat(directoryDescriptor, &parent) != 0 ||
                ::fstatat(directoryDescriptor, name.constData(), &leaf,
                          AT_SYMLINK_NOFOLLOW) != 0) {
                identitiesPinnedDuringRemoval = false;
                return -1;
            }
            int parentPins = 0;
            int leafPins = 0;
            const auto descriptors = QDir(QStringLiteral("/proc/self/fd"))
                .entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
            for (const QString &descriptorName : descriptors) {
                bool numeric = false;
                const int descriptor = descriptorName.toInt(&numeric);
                struct stat status {};
                if (!numeric || ::fstat(descriptor, &status) != 0) {
                    continue;
                }
                if (status.st_dev == parent.st_dev &&
                    status.st_ino == parent.st_ino) {
                    ++parentPins;
                }
                if (status.st_dev == leaf.st_dev &&
                    status.st_ino == leaf.st_ino &&
                    (::fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0 &&
                    (::fcntl(descriptor, F_GETFL) & O_PATH) != 0) {
                    ++leafPins;
                }
            }
            identitiesPinnedDuringRemoval &= parentPins >= 2 && leafPins >= 1;
            return ::unlinkat(directoryDescriptor, name.constData(), 0);
        };
    preview.cleanupFsyncFunctionForTesting_ =
        [&cleanupSyncCalls](int descriptor) {
            ++cleanupSyncCalls;
            return ::fsync(descriptor);
        };
    QString interlockError;
    QVERIFY2(preview.acquireCleanupInterlock(&interlockError),
             qPrintable(interlockError));
    QVERIFY(preview.cleanupInterlockActive());

    TryxRuntimeMediaTransform transform;
    transform.mode = QStringLiteral("Fill");
    const quint64 generation = preview.requestedRenderGeneration_;
    preview.setTransform(transform);
    QCOMPARE(preview.requestedRenderGeneration_, generation);
    QVERIFY(!preview.renderDebounce_.isActive());

    const MediaPreviewController::PreviewCleanupReport report =
        preview.cleanupInactivePreviews();
    QVERIFY2(report.ok(), qPrintable(report.detail));
    QVERIFY(report.complete);
    QCOMPARE(report.plannedFiles, 2);
    QCOMPARE(report.removedFiles, 2);
    QCOMPARE(report.removedLogicalBytes,
             static_cast<qint64>(
                 firstBytes.size() + secondBytes.size()));
    QCOMPARE(cleanupSyncCalls, 2);
    QVERIFY(identitiesPinnedDuringRemoval);
    QVERIFY(!QFileInfo::exists(first));
    QVERIFY(!QFileInfo::exists(second));
    QVERIFY(QFileInfo::exists(unrelated));
    preview.releaseCleanupInterlock();
    QVERIFY(!preview.cleanupInterlockActive());

    const QString outside = QDir(runtimeDirectory.path()).filePath(
        QStringLiteral("outside.png"));
    QFile outsideFile(outside);
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    QCOMPARE(outsideFile.write("preserve"), qint64(8));
    outsideFile.close();
    const QString hardlink = QDir(previewDirectory).filePath(
        QStringLiteral(
            "33333333-3333-4333-8333-333333333333-3.png"));
    QVERIFY(QFile::link(outside, hardlink));
    QVERIFY(preview.acquireCleanupInterlock(&interlockError));
    const MediaPreviewController::PreviewCleanupReport unsafe =
        preview.cleanupInactivePreviews();
    QVERIFY(!unsafe.ok());
    QCOMPARE(
        unsafe.error,
        MediaPreviewController::PreviewCleanupError::UnsafeCandidate);
    QCOMPARE(unsafe.removedFiles, 0);
    QVERIFY(QFileInfo::exists(hardlink));
    QFile verifyOutside(outside);
    QVERIFY(verifyOutside.open(QIODevice::ReadOnly));
    QCOMPARE(verifyOutside.readAll(), QByteArrayLiteral("preserve"));
    preview.releaseCleanupInterlock();
    QVERIFY(QFile::remove(hardlink));
    QVERIFY(QFile::remove(outside));
    QVERIFY(QFile::remove(unrelated));

    RuntimeClient runtime(true);
    MediaEditorController editor(&runtime);
    QVERIFY(editor.acquirePreviewCleanupInterlock(&interlockError));
    editor.begin(QUrl::fromLocalFile(outside));
    QVERIFY(!editor.isOpen());
    QVERIFY(editor.error().contains(
        QStringLiteral("cleanup"), Qt::CaseInsensitive));
    editor.beginDropped({QUrl::fromLocalFile(outside)});
    QVERIFY(!editor.isOpen());
    editor.beginRecoveredVideo({});
    QVERIFY(!editor.isOpen());
    editor.releasePreviewCleanupInterlock();
}

void QuickClientTests::previewCleanupBoundsAllDirectoryEntries() {
    QTemporaryDir runtimeDirectory;
    QVERIFY(runtimeDirectory.isValid());
    QVERIFY(QFile::setPermissions(
        runtimeDirectory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    ScopedEnvironmentVariable runtimeRoot("XDG_RUNTIME_DIR");
    runtimeRoot.set(QFile::encodeName(runtimeDirectory.path()));
    const QString previewDirectory =
        QDir(runtimeDirectory.path()).filePath(
            QStringLiteral("tryx-panorama-manager/previews"));
    QString directoryError;
    QVERIFY(MediaPreviewController::ensurePrivateDirectoryTree(
        previewDirectory, &directoryError));
    for (int index = 0; index < 3; ++index) {
        QFile unrelated(QDir(previewDirectory).filePath(
            QStringLiteral("preserve-%1.txt").arg(index)));
        QVERIFY(unrelated.open(
            QIODevice::WriteOnly | QIODevice::NewOnly));
        unrelated.close();
        QVERIFY(QFile::setPermissions(
            unrelated.fileName(),
            QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    }

    MediaPreviewController preview;
    preview.cleanupPlanEntryLimitForTesting_ = 2;
    QVERIFY(preview.acquireCleanupInterlock(&directoryError));
    const auto report = preview.cleanupInactivePreviews();
    QCOMPARE(
        report.error,
        MediaPreviewController::PreviewCleanupError::PlanLimitExceeded);
    QCOMPARE(report.removedFiles, 0);
    for (int index = 0; index < 3; ++index) {
        QVERIFY(QFileInfo::exists(QDir(previewDirectory).filePath(
            QStringLiteral("preserve-%1.txt").arg(index))));
    }
    preview.releaseCleanupInterlock();
}

void QuickClientTests::
    restoredProtectedSourceSchedulesCurrentProfilePreview() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString stagedPath =
        QDir(directory.path()).filePath(QStringLiteral("source.png"));
    QFile staged(stagedPath);
    QVERIFY(staged.open(QIODevice::WriteOnly));
    QCOMPARE(staged.write("snapshot"), qint64(8));
    staged.close();

    MediaPreviewController preview;
    preview.stagedPath_ = stagedPath;
    preview.sourceKind_ =
        MediaPreviewController::SourceKind::InboxSnapshot;
    preview.sourceProtected_ = true;
    preview.ready_ = true;

    preview.setTargetSize(1280, 720);
    QVERIFY(!preview.renderDebounce_.isActive());

    preview.restoreStagedSourceOwnership();
    QVERIFY(!preview.sourceProtected_);
    QVERIFY(preview.renderDebounce_.isActive());
}

void QuickClientTests::sourceSnapshotIsImmutableAfterCopy() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.mp4"));
    const QString stagedPath =
        QDir(directory.path()).filePath(
            QStringLiteral("snapshot.mp4"));
    const QByteArray original("immutable-source-payload");

    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(original), original.size());
    source.close();

    const QFileInfo sourceInfo(sourcePath);
    const MediaPreviewController::StageResult result =
        MediaPreviewController::copySourceSnapshot(
            sourcePath, stagedPath, sourceInfo.size(),
            sourceInfo.lastModified());
    QVERIFY2(result.error.isEmpty(),
             qPrintable(result.error));

    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray replacement("changed-after-preview");
    QCOMPARE(source.write(replacement), replacement.size());
    source.close();

    QFile snapshot(stagedPath);
    QVERIFY(snapshot.open(QIODevice::ReadOnly));
    QCOMPARE(snapshot.readAll(), original);
    const QFileInfo snapshotInfo(stagedPath);
    QVERIFY(!(snapshotInfo.permissions() &
              (QFileDevice::ReadGroup |
               QFileDevice::WriteGroup |
               QFileDevice::ExeGroup |
               QFileDevice::ReadOther |
               QFileDevice::WriteOther |
               QFileDevice::ExeOther)));
}

void QuickClientTests::sourceSnapshotPreservesExistingFinal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.mp4"));
    const QString finalPath =
        QDir(directory.path()).filePath(
            QStringLiteral("existing.mp4"));
    const QByteArray sourceBytes("new-snapshot");
    const QByteArray existingBytes("must-survive");

    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(sourceBytes), sourceBytes.size());
    source.close();
    QFile existing(finalPath);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write(existingBytes), existingBytes.size());
    existing.close();

    const QFileInfo sourceInfo(sourcePath);
    const MediaPreviewController::StageResult result =
        MediaPreviewController::copySourceSnapshot(
            sourcePath, finalPath, sourceInfo.size(),
            sourceInfo.lastModified());
    QVERIFY(!result.error.isEmpty());

    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), existingBytes);
    QVERIFY(!QFileInfo::exists(
        finalPath + QStringLiteral(".part")));
}

void QuickClientTests::
    stageHelperRejectsSymlinkDotDotEscape() {
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    if (inboxPath.isEmpty()) {
        QSKIP("Qt RuntimeLocation is unavailable");
    }
    QString directoryError;
    QVERIFY2(
        MediaPreviewController::ensurePrivateDirectoryTree(
            inboxPath, &directoryError),
        qPrintable(directoryError));

    QTemporaryDir sourceDirectory;
    QVERIFY(sourceDirectory.isValid());
    const QString sourcePath =
        QDir(sourceDirectory.path()).filePath(
            QStringLiteral("source.png"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write("snapshot"), qint64(8));
    source.close();

    const QString escapeParent =
        QDir(QFileInfo(inboxPath).absolutePath()).filePath(
            QStringLiteral("escape-parent"));
    const QString escapeChild =
        QDir(escapeParent).filePath(
            QStringLiteral("child"));
    QVERIFY(QDir().mkpath(escapeChild));
    const QString linkPath =
        QDir(inboxPath).filePath(
            QStringLiteral("link"));
    QVERIFY(QFile::link(escapeChild, linkPath));
    QVERIFY(QFileInfo(linkPath).isSymLink());

    const QString fileName =
        QStringLiteral("%1.png")
            .arg(QUuid::createUuid().toString(
                QUuid::WithoutBraces));
    const QString escapedPath =
        linkPath + QStringLiteral("/../") + fileName;
    const QString escapedTarget =
        QDir(escapeParent).filePath(fileName);
    QVERIFY(!MediaPreviewController::isManagedInboxPath(
        escapedPath));

    const QFileInfo sourceInfo(sourcePath);
    QCOMPARE(
        MediaPreviewController::runStageCopyHelper({
            sourcePath,
            escapedPath,
            QString::number(sourceInfo.size()),
            QString::number(
                sourceInfo.lastModified()
                    .toMSecsSinceEpoch()),
        }),
        2);
    QVERIFY(!QFileInfo::exists(escapedTarget));
}

void QuickClientTests::
    protectedInboxSourceSurvivesControllerLifetime() {
    const QString inboxPath = tryxRuntimeMediaInboxPath();
    if (inboxPath.isEmpty()) {
        QSKIP("Qt RuntimeLocation is unavailable");
    }
    QString directoryError;
    QVERIFY2(
        MediaPreviewController::ensurePrivateDirectoryTree(
            inboxPath, &directoryError),
        qPrintable(directoryError));
    const QString stagedPath =
        QDir(inboxPath).filePath(
            QStringLiteral("%1.png")
                .arg(QUuid::createUuid().toString(
                    QUuid::WithoutBraces)));

    QFile staged(stagedPath);
    QVERIFY(staged.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(staged.write("snapshot"), qint64(8));
    staged.close();
    QVERIFY(QFile::setPermissions(
        stagedPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    {
        MediaPreviewController preview;
        preview.stagedPath_ = stagedPath;
        preview.sourceKind_ =
            MediaPreviewController::SourceKind::InboxSnapshot;
        preview.ready_ = true;
        QVERIFY(preview.protectStagedSource());
    }
    QVERIFY(QFileInfo::exists(stagedPath));
    QVERIFY(QFile::remove(stagedPath));
}

void QuickClientTests::
    stagingTimeoutDoesNotBlockTheController() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.png"));
    QImage source(64, 64, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(sourcePath, "PNG"));

    const QString blockingHelper =
        QDir(directory.path()).filePath(
            QStringLiteral("blocking-helper"));
    QFile helper(blockingHelper);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    const QByteArray script("#!/bin/sh\nexec sleep 30\n");
    QCOMPARE(helper.write(script), script.size());
    helper.close();
    QVERIFY(QFile::setPermissions(
        blockingHelper,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    MediaPreviewController preview;
    preview.stageCopyProgram_ = blockingHelper;
    preview.stagingDeadline_.setInterval(50);
    const QFileInfo sourceInfo(sourcePath);
    preview.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QTRY_VERIFY_WITH_TIMEOUT(!preview.busy(), 2000);
    QVERIFY(preview.error().contains(
        QStringLiteral("timed out"), Qt::CaseInsensitive));
    QVERIFY(preview.pendingStagePath_.isEmpty());
    QVERIFY(preview.stageProcess_ == nullptr);

    const QString inboxPath = tryxRuntimeMediaInboxPath();
    QCOMPARE(
        QDir(inboxPath).entryList(
            QDir::Files | QDir::NoDotAndDotDot),
        QStringList{});
    QTRY_VERIFY_WITH_TIMEOUT(
        !MediaPreviewController::
            stageHelperDrainInProgress(),
        2000);
}

void QuickClientTests::
    drainingStageHelperBlocksRepeatedStart() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.png"));
    QImage source(64, 64, QImage::Format_RGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(sourcePath, "PNG"));

    const QString blockingHelper =
        QDir(directory.path()).filePath(
            QStringLiteral("blocking-helper"));
    QFile helper(blockingHelper);
    QVERIFY(helper.open(QIODevice::WriteOnly));
    const QByteArray script("#!/bin/sh\nexec sleep 30\n");
    QCOMPARE(helper.write(script), script.size());
    helper.close();
    QVERIFY(QFile::setPermissions(
        blockingHelper,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    MediaPreviewController first;
    first.stageCopyProgram_ = blockingHelper;
    const QFileInfo sourceInfo(sourcePath);
    first.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QTRY_VERIFY_WITH_TIMEOUT(
        first.stageProcess_ != nullptr &&
            first.stageProcess_->state() ==
                QProcess::Running,
        2000);
    first.cancel();
    QVERIFY(
        MediaPreviewController::
            stageHelperDrainInProgress());

    MediaPreviewController second;
    second.stageCopyProgram_ = blockingHelper;
    second.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QVERIFY(second.stageProcess_ == nullptr);
    QVERIFY(second.error().contains(
        QStringLiteral("still stopping"),
        Qt::CaseInsensitive));
    second.startStaging(
        sourcePath, QStringLiteral("png"),
        sourceInfo.size(), sourceInfo.lastModified());
    QVERIFY(second.stageProcess_ == nullptr);
    QVERIFY(second.error().contains(
        QStringLiteral("still stopping"),
        Qt::CaseInsensitive));

    QTRY_VERIFY_WITH_TIMEOUT(
        !MediaPreviewController::
            stageHelperDrainInProgress(),
        2000);
}

void QuickClientTests::
    previewControllerRendersImmutableExactFrame() {
    if (QStandardPaths::findExecutable(
            QStringLiteral("ffmpeg")).isEmpty()) {
        QSKIP("ffmpeg is optional for the unit-test environment");
    }

    QTemporaryDir sourceDirectory;
    QVERIFY(sourceDirectory.isValid());
    const QString sourcePath =
        QDir(sourceDirectory.path()).filePath(
            QStringLiteral("source.png"));
    QImage source(320, 240, QImage::Format_RGB32);
    source.fill(QColor(QStringLiteral("#dd2211")));
    QVERIFY(source.save(sourcePath, "PNG"));

    QString stagedPath;
    QString firstPreviewPath;
    QString secondPreviewPath;
    {
        MediaPreviewController preview;
        preview.load(QUrl::fromLocalFile(sourcePath));
        QTRY_VERIFY_WITH_TIMEOUT(!preview.busy(), 30000);
        QVERIFY2(preview.ready(), qPrintable(preview.error()));
        stagedPath = preview.sourcePath();
        firstPreviewPath =
            preview.previewUrl().toLocalFile();
        QVERIFY(QFileInfo::exists(stagedPath));

        QImage firstPreview(firstPreviewPath);
        QVERIFY(!firstPreview.isNull());
        QCOMPARE(firstPreview.size(), QSize(1120, 540));
        const QColor firstCenter =
            firstPreview.pixelColor(
                firstPreview.width() / 2,
                firstPreview.height() / 2);
        QVERIFY(firstCenter.red() > 180);
        QVERIFY(firstCenter.blue() < 80);

        QImage replacement(320, 240, QImage::Format_RGB32);
        replacement.fill(QColor(QStringLiteral("#1144dd")));
        QVERIFY(replacement.save(sourcePath, "PNG"));

        TryxRuntimeMediaTransform fill;
        fill.mode = QStringLiteral("Fill");
        preview.setTransform(fill);
        QTRY_VERIFY_WITH_TIMEOUT(!preview.busy(), 30000);
        QVERIFY2(preview.ready(), qPrintable(preview.error()));
        secondPreviewPath =
            preview.previewUrl().toLocalFile();
        QVERIFY(firstPreviewPath != secondPreviewPath);

        QImage secondPreview(secondPreviewPath);
        QVERIFY(!secondPreview.isNull());
        QCOMPARE(secondPreview.size(), QSize(1120, 540));
        const QColor secondCenter =
            secondPreview.pixelColor(
                secondPreview.width() / 2,
                secondPreview.height() / 2);
        QVERIFY(secondCenter.red() > 180);
        QVERIFY(secondCenter.blue() < 80);
    }
    QVERIFY(!QFileInfo::exists(stagedPath));
    QVERIFY(!QFileInfo::exists(firstPreviewPath));
    QVERIFY(!QFileInfo::exists(secondPreviewPath));
}

void QuickClientTests::publicPreviewRejectsRawDeviceH264() {
    QVERIFY(!MediaPreviewController::isSupportedSuffix(
        QStringLiteral("h264")));
    QVERIFY(MediaPreviewController::isSupportedSuffix(
        QStringLiteral("mp4")));
}

void QuickClientTests::
    exportHelperIsAtomicAndDoesNotClobber() {
    const QString outbox =
        tryxRuntimeDeviceMediaOutboxPath();
    QVERIFY(!outbox.isEmpty());
    QVERIFY(QDir().mkpath(outbox));
    QVERIFY(QFile::setPermissions(
        outbox,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const QString sourcePath =
        QDir(outbox).filePath(
            QUuid::createUuid().toString(
                QUuid::WithoutBraces) +
            QStringLiteral(".h264"));
    const QByteArray payload(
        "validated recovered H264 test bytes");
    QFile source(sourcePath);
    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(source.write(payload),
             static_cast<qint64>(payload.size()));
    source.close();
    QVERIFY(QFile::setPermissions(
        sourcePath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner));

    QTemporaryDir destinationDirectory;
    QVERIFY(destinationDirectory.isValid());
    const QString destinationPath =
        QDir(destinationDirectory.path()).filePath(
            QStringLiteral("export.h264"));
    const QString expectedSha =
        QString::fromLatin1(
            QCryptographicHash::hash(
                payload, QCryptographicHash::Sha256)
                .toHex());
    const QStringList arguments{
        sourcePath,
        destinationPath,
        QString::number(payload.size()),
        expectedSha,
        QStringLiteral("0"),
    };

    QCOMPARE(
        DeviceMediaWorkflowController::runExportHelper(
            arguments),
        0);
    QFile exported(destinationPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), payload);
    exported.close();

    QVERIFY(exported.open(
        QIODevice::WriteOnly |
        QIODevice::Truncate));
    QCOMPARE(exported.write("keep"), qint64(4));
    exported.close();
    QCOMPARE(
        DeviceMediaWorkflowController::runExportHelper(
            arguments),
        3);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), QByteArray("keep"));
    exported.close();

    QStringList overwriteArguments = arguments;
    overwriteArguments[4] = QStringLiteral("1");
    QCOMPARE(
        DeviceMediaWorkflowController::runExportHelper(
            overwriteArguments),
        0);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QCOMPARE(exported.readAll(), payload);
    exported.close();
    QVERIFY(QFile::remove(sourcePath));
}

void QuickClientTests::supportBundleReportUsesHostAllowlist() {
    ScopedEnvironmentVariable home("HOME");
    ScopedEnvironmentVariable secret("AWS_SECRET_ACCESS_KEY");
    home.set(QByteArrayLiteral("/home/support-canary"));
    secret.set(QByteArrayLiteral("SUPPORT-SECRET-CANARY"));
    const qint64 generatedAt = QDateTime::fromString(
        QStringLiteral("2026-08-30T12:00:00.000Z"),
        Qt::ISODateWithMs).toMSecsSinceEpoch();

    QString error;
    const QByteArray hostOnly = tryx::support_bundle::buildReportV1(
        {}, tryx::support_bundle::RuntimeSnapshotStatus::Unsupported,
        generatedAt, &error);
    QVERIFY2(!hostOnly.isEmpty(), qPrintable(error));
    QVERIFY(!hostOnly.contains("/home/support-canary"));
    QVERIFY(!hostOnly.contains("SUPPORT-SECRET-CANARY"));
    const QJsonObject hostRoot =
        QJsonDocument::fromJson(hostOnly).object();
    QCOMPARE(hostRoot.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(hostRoot.value(QStringLiteral("runtime_snapshot"))
                 .toObject()
                 .value(QStringLiteral("status")).toString(),
             QStringLiteral("unsupported"));
    const QByteArray unavailable = tryx::support_bundle::buildReportV1(
        {}, tryx::support_bundle::RuntimeSnapshotStatus::Unavailable,
        generatedAt, &error);
    QVERIFY2(!unavailable.isEmpty(), qPrintable(error));
    QCOMPARE(QJsonDocument::fromJson(unavailable)
                 .object()
                 .value(QStringLiteral("runtime_snapshot"))
                 .toObject()
                 .value(QStringLiteral("status")).toString(),
             QStringLiteral("unavailable"));
    QCOMPARE(hostRoot.value(QStringLiteral("host")).toObject().keys(),
             QStringList({QStringLiteral("cpu_architecture"),
                          QStringLiteral("kernel_type"),
                          QStringLiteral("kernel_version"),
                          QStringLiteral("product_type"),
                          QStringLiteral("product_version")}));
    QVERIFY(QStringList({QStringLiteral("wayland"),
                         QStringLiteral("xcb"),
                         QStringLiteral("other")})
                .contains(hostRoot.value(QStringLiteral("application"))
                              .toObject()
                              .value(QStringLiteral("qt_platform"))
                              .toString()));

    tryx::SupportSnapshotSourceV1 runtimeSource;
    runtimeSource.generatedAtUtcMs = generatedAt;
    runtimeSource.runtimeVersion = QStringLiteral("2.2.0");
    runtimeSource.runtimeApiVersion = 8;
    const QString runtimeSnapshot =
        tryx::buildSupportSnapshotV1(runtimeSource);
    const QByteArray full = tryx::support_bundle::buildReportV1(
        runtimeSnapshot,
        tryx::support_bundle::RuntimeSnapshotStatus::Available,
        generatedAt, &error);
    QVERIFY2(!full.isEmpty(), qPrintable(error));
    const QJsonObject runtimeSection =
        QJsonDocument::fromJson(full)
            .object()
            .value(QStringLiteral("runtime_snapshot"))
            .toObject();
    QCOMPARE(runtimeSection.value(QStringLiteral("status")).toString(),
             QStringLiteral("available"));
    QCOMPARE(runtimeSection.value(QStringLiteral("data"))
                 .toObject()
                 .value(QStringLiteral("schema_version")).toInt(),
             1);

    const QByteArray malformed = tryx::support_bundle::buildReportV1(
        QStringLiteral("{\"secret\":true}"),
        tryx::support_bundle::RuntimeSnapshotStatus::Available,
        generatedAt, &error);
    QVERIFY(malformed.isEmpty());
    QVERIFY(!error.isEmpty());

    QJsonObject typeConfusedRoot = QJsonDocument::fromJson(
        runtimeSnapshot.toUtf8()).object();
    QJsonObject runtime =
        typeConfusedRoot.value(QStringLiteral("runtime")).toObject();
    runtime.insert(
        QStringLiteral("version"),
        QJsonObject{{QStringLiteral("secret"),
                     QStringLiteral("/home/alice/type-canary")}});
    typeConfusedRoot.insert(QStringLiteral("runtime"), runtime);
    const QByteArray typeConfused = tryx::support_bundle::buildReportV1(
        QString::fromUtf8(QJsonDocument(typeConfusedRoot).toJson(
            QJsonDocument::Compact)),
        tryx::support_bundle::RuntimeSnapshotStatus::Available,
        generatedAt, &error);
    QVERIFY(typeConfused.isEmpty());
    QVERIFY(!typeConfused.contains("type-canary"));
}

void QuickClientTests::
    supportBundleWriterCreatesPrivateFileWithoutClobbering() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QByteArray payload("{\"schema_version\":1}\n");
    const QString fileName = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-a1b2c3d4.json");
    const QUrl folder = QUrl::fromLocalFile(directory.path());

    const auto result = tryx::support_bundle::writeNewReport(
        folder, fileName, payload);
    QVERIFY2(result.ok(), qPrintable(result.message));
    const QString expectedPath =
        QDir(directory.path()).filePath(fileName);
    QCOMPARE(result.path, expectedPath);
    QFile file(expectedPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payload);
    file.close();
    struct stat status {};
    const QByteArray encoded = QFile::encodeName(expectedPath);
    QCOMPARE(::lstat(encoded.constData(), &status), 0);
    QVERIFY(S_ISREG(status.st_mode));
    QCOMPARE(status.st_uid, ::getuid());
    QCOMPARE(status.st_nlink, static_cast<nlink_t>(1));
    QCOMPARE(status.st_mode & 0777, static_cast<mode_t>(0600));

    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write("keep"), qint64(4));
    file.close();
    const auto collision = tryx::support_bundle::writeNewReport(
        folder, fileName, payload);
    QVERIFY(!collision.ok());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("keep"));
}

void QuickClientTests::
    supportBundleWriterRejectsBeforePublishGuardFails() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QByteArray payload("{\"schema_version\":1}\n");
    const QString fileName = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-b1c2d3e4.json");
    const QString targetPath =
        QDir(directory.path()).filePath(fileName);
    int guardCallCount = 0;
    bool preparedFileObserved = false;

    const auto result = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(directory.path()), fileName, payload,
        [&]() {
            ++guardCallCount;
            preparedFileObserved =
                QDir(directory.path())
                    .entryList(
                        QStringList{QStringLiteral(
                            ".tryx-panorama-support-*.tmp")},
                        QDir::Files | QDir::Hidden)
                    .size() == 1;
            return false;
        });

    QCOMPARE(guardCallCount, 1);
    QVERIFY(preparedFileObserved);
    QVERIFY(!result.ok());
    QCOMPARE(
        result.status,
        tryx::support_bundle::WriteStatus::PublishRejected);
    QVERIFY(!QFileInfo::exists(targetPath));
    QCOMPARE(
        QDir(directory.path())
            .entryList(
                QStringList{QStringLiteral(
                    ".tryx-panorama-support-*.tmp")},
                QDir::Files | QDir::Hidden)
            .size(),
        0);
}

void QuickClientTests::
    supportBundleWriterRollsBackAfterPublishGuardFails() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QByteArray payload("{\"schema_version\":1}\n");
    const QString fileName = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-c1d2e3f4.json");
    const QString targetPath =
        QDir(directory.path()).filePath(fileName);
    int guardCallCount = 0;
    bool publishedFileObserved = false;

    const auto result = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(directory.path()), fileName, payload,
        [&]() {
            ++guardCallCount;
            if (guardCallCount == 1) {
                return true;
            }
            publishedFileObserved = QFileInfo::exists(targetPath);
            return false;
        });

    QCOMPARE(guardCallCount, 2);
    QVERIFY(publishedFileObserved);
    QVERIFY(!result.ok());
    QCOMPARE(
        result.status,
        tryx::support_bundle::WriteStatus::PublishRejected);
    QVERIFY(!QFileInfo::exists(targetPath));
    QCOMPARE(
        QDir(directory.path())
            .entryList(
                QStringList{QStringLiteral(
                    ".tryx-panorama-support-*.tmp")},
                QDir::Files | QDir::Hidden)
            .size(),
        0);
}

void QuickClientTests::
    supportBundleWriterPinsIdentityThroughPublication_data() {
    QTest::addColumn<int>("rejectOnCall");
    QTest::newRow("success") << 0;
    QTest::newRow("reject-before-publication") << 1;
    QTest::newRow("reject-after-publication") << 2;
}

void QuickClientTests::
    supportBundleWriterPinsIdentityThroughPublication() {
    QFETCH(int, rejectOnCall);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray payload("{\"schema_version\":1}\n");
    const QString fileName = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-e1e2f3a4.json");
    const QString targetPath = QDir(directory.path()).filePath(fileName);
    const QStringList temporaryPattern{
        QStringLiteral(".tryx-panorama-support-*.tmp")};
    int guardCalls = 0;
    bool pinsHeld = true;
    struct stat identity {};
    const auto result = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(directory.path()), fileName, payload,
        [&]() {
            ++guardCalls;
            QString currentPath = targetPath;
            if (guardCalls == 1) {
                const auto names = QDir(directory.path()).entryList(
                    temporaryPattern, QDir::Files | QDir::Hidden);
                pinsHeld &= names.size() == 1;
                currentPath = QDir(directory.path()).filePath(names.value(0));
            }
            struct stat status {};
            if (::lstat(QFile::encodeName(currentPath).constData(), &status) != 0) {
                pinsHeld = false;
            } else {
                identity = status;
                const auto pins = descriptorsForIdentity(identity);
                pinsHeld &= pins.size() == 1;
                if (pins.size() == 1) {
                    pinsHeld &= (::fcntl(pins.first(), F_GETFD) & FD_CLOEXEC) != 0;
                    pinsHeld &= (::fcntl(pins.first(), F_GETFL) & O_ACCMODE) == O_WRONLY;
                }
            }
            return guardCalls != rejectOnCall;
        });
    QVERIFY(pinsHeld);
    QCOMPARE(guardCalls, rejectOnCall == 1 ? 1 : 2);
    QCOMPARE(result.status, rejectOnCall == 0
                 ? tryx::support_bundle::WriteStatus::Success
                 : tryx::support_bundle::WriteStatus::PublishRejected);
    QVERIFY(descriptorsForIdentity(identity).isEmpty());
    if (rejectOnCall == 0) {
        QFile file(targetPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), payload);
    } else {
        QVERIFY(!QFileInfo::exists(targetPath));
    }
    QVERIFY(QDir(directory.path()).entryList(
                temporaryPattern, QDir::Files | QDir::Hidden).isEmpty());
}

void QuickClientTests::
    supportBundleWriterPreservesReplacedRollbackTarget() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QFile::setPermissions(
        directory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QByteArray payload("{\"schema_version\":1}\n");
    const QByteArray foreignPayload("foreign");
    const QString fileName = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-d1e2f3a4.json");
    const QString targetPath =
        QDir(directory.path()).filePath(fileName);
    int guardCallCount = 0;
    bool targetReplaced = false;

    const auto result = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(directory.path()), fileName, payload,
        [&]() {
            ++guardCallCount;
            if (guardCallCount == 1) {
                return true;
            }
            if (!QFile::remove(targetPath)) {
                return false;
            }
            QFile replacement(targetPath);
            targetReplaced =
                replacement.open(
                    QIODevice::WriteOnly | QIODevice::NewOnly) &&
                replacement.write(foreignPayload) ==
                    foreignPayload.size();
            replacement.close();
            return false;
        });

    QCOMPARE(guardCallCount, 2);
    QVERIFY(targetReplaced);
    QVERIFY(!result.ok());
    QCOMPARE(result.status,
             tryx::support_bundle::WriteStatus::IoError);
    QFile replacement(targetPath);
    QVERIFY(replacement.open(QIODevice::ReadOnly));
    QCOMPARE(replacement.readAll(), foreignPayload);
    QCOMPARE(
        QDir(directory.path())
            .entryList(
                QStringList{QStringLiteral(
                    ".tryx-panorama-support-*.tmp")},
                QDir::Files | QDir::Hidden)
            .size(),
        0);
}

void QuickClientTests::supportBundleWriterRejectsUnsafeTargets() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray payload("{\"schema_version\":1}\n");
    const QUrl folder = QUrl::fromLocalFile(directory.path());
    const QString prefix = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-");
    const QString victimPath =
        QDir(directory.path()).filePath(QStringLiteral("victim"));
    QFile victim(victimPath);
    QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(victim.write("victim"), qint64(6));
    victim.close();

    const QString symlinkName = prefix +
        QStringLiteral("11111111.json");
    const QByteArray symlinkPath = QFile::encodeName(
        QDir(directory.path()).filePath(symlinkName));
    const QByteArray encodedVictim = QFile::encodeName(victimPath);
    QCOMPARE(::symlink(encodedVictim.constData(),
                       symlinkPath.constData()), 0);
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 folder, symlinkName, payload).ok());

    const QString hardlinkName = prefix +
        QStringLiteral("22222222.json");
    const QByteArray hardlinkPath = QFile::encodeName(
        QDir(directory.path()).filePath(hardlinkName));
    QCOMPARE(::link(encodedVictim.constData(),
                    hardlinkPath.constData()), 0);
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 folder, hardlinkName, payload).ok());

    const QString fifoName = prefix +
        QStringLiteral("33333333.json");
    const QByteArray fifoPath = QFile::encodeName(
        QDir(directory.path()).filePath(fifoName));
    QCOMPARE(::mkfifo(fifoPath.constData(), 0600), 0);
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 folder, fifoName, payload).ok());

    QVERIFY(!tryx::support_bundle::writeNewReport(
                 QUrl(QStringLiteral("https://example.invalid/export")),
                 prefix + QStringLiteral("44444444.json"),
                 payload).ok());
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 QUrl(QStringLiteral("file://remote.invalid/export")),
                 prefix + QStringLiteral("44444445.json"),
                 payload).ok());
    QString nulPath = directory.path();
    nulPath.append(QChar::Null);
    nulPath.append(QStringLiteral("-suffix"));
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 QUrl::fromLocalFile(nulPath),
                 prefix + QStringLiteral("44444446.json"),
                 payload).ok());
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 folder, QStringLiteral("../support.json"),
                 payload).ok());
    QVERIFY(!tryx::support_bundle::writeNewReport(
                 folder,
                 prefix + QStringLiteral("55555555.json"),
                 QByteArray(1024 * 1024 + 1, 'x')).ok());

    QVERIFY(victim.open(QIODevice::ReadOnly));
    QCOMPARE(victim.readAll(), QByteArray("victim"));
}

void QuickClientTests::
    supportBundleWriterRejectsPreparedTargetAndDirectoryRaces() {
    const QByteArray payload("{\"schema_version\":1}\n");
    const QString fileName = QStringLiteral(
        "tryx-panorama-support-20260830-120000-000-aabbccdd.json");

    QTemporaryDir ancestorRaceRoot;
    QVERIFY(ancestorRaceRoot.isValid());
    const QDir ancestorRoot(ancestorRaceRoot.path());
    const QString selectedParent = ancestorRoot.filePath(
        QStringLiteral("selected"));
    const QString selectedExport = QDir(selectedParent).filePath(
        QStringLiteral("export"));
    const QString movedParent = ancestorRoot.filePath(
        QStringLiteral("selected-moved"));
    const QString divertedParent = ancestorRoot.filePath(
        QStringLiteral("diverted"));
    const QString divertedExport = QDir(divertedParent).filePath(
        QStringLiteral("export"));
    QVERIFY(QDir().mkpath(selectedExport));
    QVERIFY(QDir().mkpath(divertedExport));
    bool ancestorSwapSucceeded = false;
    tryx::support_bundle::testing::setBeforeDirectoryOpenHook(
        [&](const QString &) {
            const QByteArray selected = QFile::encodeName(selectedParent);
            const QByteArray moved = QFile::encodeName(movedParent);
            const QByteArray diverted = QFile::encodeName(divertedParent);
            ancestorSwapSucceeded =
                ::rename(selected.constData(), moved.constData()) == 0 &&
                ::symlink(diverted.constData(), selected.constData()) == 0;
        });
    const auto ancestorRace = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(selectedExport), fileName, payload);
    tryx::support_bundle::testing::setBeforeDirectoryOpenHook({});
    QVERIFY(ancestorSwapSucceeded);
    QVERIFY(!ancestorRace.ok());
    QVERIFY(!QFileInfo::exists(
        QDir(divertedExport).filePath(fileName)));
    QVERIFY(!QFileInfo::exists(
        QDir(movedParent).filePath(
            QStringLiteral("export/") + fileName)));

    QTemporaryDir hardlinkDirectory;
    QVERIFY(hardlinkDirectory.isValid());
    const QString victimPath = QDir(hardlinkDirectory.path())
        .filePath(QStringLiteral("victim"));
    QFile victim(victimPath);
    QVERIFY(victim.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(victim.write("victim"), qint64(6));
    victim.close();
    tryx::support_bundle::testing::setBeforePublishHook(
        [victimPath](const QString &folder, const QString &targetName) {
            const QByteArray victimName = QFile::encodeName(victimPath);
            const QByteArray target = QFile::encodeName(
                QDir(folder).filePath(targetName));
            ::link(victimName.constData(), target.constData());
        });
    const auto hardlinkRace = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(hardlinkDirectory.path()),
        fileName, payload);
    tryx::support_bundle::testing::setBeforePublishHook({});
    QVERIFY(!hardlinkRace.ok());
    QVERIFY(victim.open(QIODevice::ReadOnly));
    QCOMPARE(victim.readAll(), QByteArray("victim"));
    victim.close();
    QCOMPARE(QDir(hardlinkDirectory.path()).entryList(
                 QStringList{QStringLiteral(
                     ".tryx-panorama-support-*.tmp")},
                 QDir::Files | QDir::Hidden).size(), 0);

    QTemporaryDir parentDirectory;
    QVERIFY(parentDirectory.isValid());
    const QString exportPath = QDir(parentDirectory.path())
        .filePath(QStringLiteral("export"));
    const QString movedPath = QDir(parentDirectory.path())
        .filePath(QStringLiteral("export-moved"));
    QVERIFY(QDir().mkdir(exportPath));
    QVERIFY(QFile::setPermissions(
        exportPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    bool replacementSucceeded = false;
    tryx::support_bundle::testing::setBeforePublishHook(
        [&replacementSucceeded, exportPath, movedPath](
            const QString &, const QString &) {
            const QByteArray source = QFile::encodeName(exportPath);
            const QByteArray moved = QFile::encodeName(movedPath);
            replacementSucceeded =
                ::rename(source.constData(), moved.constData()) == 0 &&
                ::mkdir(source.constData(), 0700) == 0;
        });
    const auto directoryRace = tryx::support_bundle::writeNewReport(
        QUrl::fromLocalFile(exportPath), fileName, payload);
    tryx::support_bundle::testing::setBeforePublishHook({});
    QVERIFY(replacementSucceeded);
    QVERIFY(!directoryRace.ok());
    QVERIFY(!QFileInfo::exists(QDir(exportPath).filePath(fileName)));
    QVERIFY(!QFileInfo::exists(QDir(movedPath).filePath(fileName)));
    QCOMPARE(QDir(movedPath).entryList(
                 QStringList{QStringLiteral(
                     ".tryx-panorama-support-*.tmp")},
                 QDir::Files | QDir::Hidden).size(), 0);

    const QByteArray replacement = QFile::encodeName(exportPath);
    const QByteArray moved = QFile::encodeName(movedPath);
    QCOMPARE(::rmdir(replacement.constData()), 0);
    QCOMPARE(::rename(moved.constData(), replacement.constData()), 0);
}

void QuickClientTests::qmlImportScannerFindsResolvedModules() {
    const QString scanner = QString::fromLocal8Bit(
        qgetenv("TRYX_QMLIMPORTSCANNER"));
    const QString qmlRoot = QString::fromLocal8Bit(
        qgetenv("TRYX_QML_ROOT"));
    const QString importPath = QString::fromLocal8Bit(
        qgetenv("TRYX_QML_IMPORT_PATH"));
    if (scanner.isEmpty() || qmlRoot.isEmpty() ||
        importPath.isEmpty()) {
        QFAIL(
            "qmlimportscanner paths are required; run make quick-check");
    }

    QProcess process;
    process.setProgram(scanner);
    process.setArguments({
        QStringLiteral("-rootPath"), qmlRoot,
        QStringLiteral("-importPath"), importPath,
    });
    process.start();
    QVERIFY2(
        process.waitForStarted(5000),
        qPrintable(process.errorString()));
    QVERIFY2(
        process.waitForFinished(30000),
        "qmlimportscanner did not finish within 30 seconds");
    const QByteArray standardError =
        process.readAllStandardError();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(
        process.exitCode() == 0,
        standardError.constData());

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            process.readAllStandardOutput(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    const QJsonArray imports = document.array();
    QVERIFY2(!imports.isEmpty(),
             "qmlimportscanner returned no imports");

    const QSet<QString> requiredModules{
        QStringLiteral("Qt.labs.folderlistmodel"),
        QStringLiteral("QtQuick"),
        QStringLiteral("QtQuick.Controls"),
        QStringLiteral("QtQuick.Controls.Material"),
        QStringLiteral("QtQuick.Dialogs"),
        QStringLiteral("QtQuick.Layouts"),
    };
    QSet<QString> resolvedRequiredModules;
    for (const QJsonValue &value : imports) {
        QVERIFY(value.isObject());
        const QJsonObject import = value.toObject();
        const QString type =
            import.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("module") &&
            type != QStringLiteral("directory")) {
            continue;
        }
        const QString name =
            import.value(QStringLiteral("name"))
                .toString()
                .trimmed();
        const QString path =
            import.value(QStringLiteral("path"))
                .toString()
                .trimmed();
        QVERIFY2(
            !name.isEmpty(),
            "qmlimportscanner returned an unnamed module or directory");
        if (type == QStringLiteral("module") &&
            requiredModules.contains(name)) {
            QVERIFY2(
                !path.isEmpty(),
                qPrintable(
                    QStringLiteral(
                        "qmlimportscanner did not resolve required module %1")
                        .arg(name)));
            resolvedRequiredModules.insert(name);
        }
    }
    for (const QString &module : requiredModules) {
        QVERIFY2(
            resolvedRequiredModules.contains(module),
            qPrintable(
                QStringLiteral(
                    "qmlimportscanner omitted required module %1")
                    .arg(module)));
    }
}

void QuickClientTests::catalogRejectsStaleRevision() {
    MediaCatalogModel model;
    TryxRuntimeMediaCatalogSnapshot newer;
    newer.revision = 5;
    newer.deviceIdentity = QStringLiteral("device-a");
    TryxRuntimeMediaEntry entry;
    entry.name = QStringLiteral("new.mp4.h264_2240x1080");
    entry.size = 1536;
    entry.source = 1;
    entry.readOnly = false;
    entry.managedOrigin = true;
    newer.entries.append(entry);
    model.applySnapshot(newer);

    TryxRuntimeMediaCatalogSnapshot stale;
    stale.revision = 4;
    stale.deviceIdentity = QStringLiteral("device-b");
    model.applySnapshot(stale);

    QCOMPARE(model.revision(), 5U);
    QCOMPARE(model.deviceIdentity(), QStringLiteral("device-a"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(
        model.data(model.index(0), MediaCatalogModel::NameRole)
            .toString(),
        entry.name);
    QCOMPARE(
        model.data(model.index(0), MediaCatalogModel::SizeRole)
            .toULongLong(),
        entry.size);
    QCOMPARE(
        model.data(model.index(0), MediaCatalogModel::SourceRole)
            .toUInt(),
        entry.source);
    QCOMPARE(
        model.data(model.index(0), MediaCatalogModel::ManagedOriginRole)
            .toBool(),
        entry.managedOrigin);
}

void QuickClientTests::
    catalogResolvesThumbnailFromConfiguredDataRoot() {
    QTemporaryDir temporaryData;
    QVERIFY(temporaryData.isValid());

    const QString thumbnailKey(64, QLatin1Char('a'));
    const QString thumbnailDirectory =
        QDir(temporaryData.path()).filePath(
            QStringLiteral("media-catalog/thumbnails"));
    QVERIFY(QDir().mkpath(thumbnailDirectory));
    const QString thumbnailPath =
        QDir(thumbnailDirectory).filePath(
            thumbnailKey + QStringLiteral(".jpg"));
    QFile thumbnail(thumbnailPath);
    QVERIFY(thumbnail.open(QIODevice::WriteOnly));
    QCOMPARE(thumbnail.write("thumbnail"), 9);
    thumbnail.close();

    TryxRuntimeMediaEntry entry;
    entry.name =
        QStringLiteral("user.mp4.h264_2240x1080");
    entry.thumbnailKey = thumbnailKey;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = QStringLiteral("device-a");
    snapshot.entries = {entry};

    MediaCatalogModel model(temporaryData.path());
    model.applySnapshot(snapshot);

    QCOMPARE(
        model.data(
            model.index(0),
            MediaCatalogModel::ThumbnailUrlRole).toUrl(),
        QUrl::fromLocalFile(thumbnailPath));
}

void QuickClientTests::catalogExposesOriginMetadataRoles() {
    MediaCatalogModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(roles.value(MediaCatalogModel::SizeRole),
             QByteArray("mediaSize"));
    QCOMPARE(roles.value(MediaCatalogModel::SourceRole),
             QByteArray("mediaSource"));
    QCOMPARE(roles.value(MediaCatalogModel::ReadOnlyRole),
             QByteArray("readOnly"));
    QCOMPARE(roles.value(MediaCatalogModel::ManagedOriginRole),
             QByteArray("managedOrigin"));

    TryxRuntimeMediaEntry userMedia;
    userMedia.name =
        QStringLiteral("user.mp4.h264_2240x1080");
    userMedia.size = 1536;
    userMedia.source = 1;
    userMedia.readOnly = false;
    userMedia.managedOrigin = true;

    TryxRuntimeMediaEntry presetMedia;
    presetMedia.name =
        QStringLiteral("preset.mp4.h264_2240x1080");
    presetMedia.size = 2U * 1024U * 1024U;
    presetMedia.source = 2;
    presetMedia.readOnly = true;
    presetMedia.managedOrigin = false;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = QStringLiteral("device-a");
    snapshot.entries = {userMedia, presetMedia};
    model.applySnapshot(snapshot);

    const QModelIndex userIndex = model.index(0);
    QCOMPARE(model.data(userIndex, MediaCatalogModel::SizeRole)
                 .toULongLong(),
             userMedia.size);
    QCOMPARE(model.data(userIndex, MediaCatalogModel::SourceRole)
                 .toUInt(),
             userMedia.source);
    QCOMPARE(model.data(userIndex, MediaCatalogModel::ReadOnlyRole)
                 .toBool(),
             userMedia.readOnly);
    QCOMPARE(model.data(userIndex, MediaCatalogModel::ManagedOriginRole)
                 .toBool(),
             userMedia.managedOrigin);

    const QModelIndex presetIndex = model.index(1);
    QCOMPARE(model.data(presetIndex, MediaCatalogModel::SizeRole)
                 .toULongLong(),
             presetMedia.size);
    QCOMPARE(model.data(presetIndex, MediaCatalogModel::SourceRole)
                 .toUInt(),
             presetMedia.source);
    QCOMPARE(model.data(presetIndex, MediaCatalogModel::ReadOnlyRole)
                 .toBool(),
             presetMedia.readOnly);
    QCOMPARE(model.data(presetIndex, MediaCatalogModel::ManagedOriginRole)
                 .toBool(),
             presetMedia.managedOrigin);
}

void QuickClientTests::catalogExposesDeviceCopyEligibility() {
    MediaCatalogModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(
        roles.value(MediaCatalogModel::MediaIdRole),
        QByteArray("mediaId"));
    QCOMPARE(
        roles.value(MediaCatalogModel::DeviceCopyAllowedRole),
        QByteArray("deviceCopyAllowed"));
    QCOMPARE(
        roles.value(MediaCatalogModel::DeviceCopyBlockReasonRole),
        QByteArray("deviceCopyBlockReason"));

    TryxRuntimeMediaEntry userMedia;
    userMedia.name =
        QStringLiteral("user.mp4.h264_2240x1080");
    userMedia.source = 1;
    userMedia.mediaId = QStringLiteral("media-user");

    TryxRuntimeMediaEntry presetMedia;
    presetMedia.name =
        QStringLiteral("preset.mp4.h264_2240x1080");
    presetMedia.source = 2;
    presetMedia.mediaId = QStringLiteral("media-preset");
    presetMedia.readOnly = true;

    TryxRuntimeMediaCatalogSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.deviceIdentity = QStringLiteral("device-a");
    snapshot.entries = {userMedia, presetMedia};
    model.applySnapshot(snapshot);

    QVERIFY(model.canStageDeviceCopy(userMedia.mediaId));
    QVERIFY(model.data(
        model.index(0),
        MediaCatalogModel::DeviceCopyAllowedRole).toBool());
    QVERIFY(!model.canStageDeviceCopy(presetMedia.mediaId));
    QVERIFY(!model.data(
        model.index(1),
        MediaCatalogModel::DeviceCopyAllowedRole).toBool());
    QVERIFY(!model.deviceCopyBlockReason(
        presetMedia.mediaId).isEmpty());
    QVERIFY(!model.canStageDeviceCopy(QStringLiteral("missing")));
}

void QuickClientTests::
    legacyConnectionPopulatesCurrentMediaModel() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;

    TryxRuntimeSnapshot snapshot;
    snapshot.revision = 4;
    snapshot.connected = true;
    snapshot.serial = QStringLiteral("LEGACY-1");
    snapshot.mediaFiles = {
        QStringLiteral("first.mp4"),
        QStringLiteral("first.mp4"),
        QString(),
        QStringLiteral("second.gif"),
    };
    runtime.applyConnectionSnapshot(snapshot);

    QVERIFY(runtime.ready());
    QVERIFY(runtime.legacyConnected());
    QVERIFY(runtime.connectionStatus().contains(
        QStringLiteral("Legacy")));
    QCOMPARE(runtime.mediaModel()->rowCount(), 2);
    QCOMPARE(
        runtime.mediaModel()->deviceIdentity(),
        QStringLiteral("legacy:LEGACY-1"));
    const QModelIndex first =
        runtime.mediaModel()->index(0, 0);
    QVERIFY(first.data(
        MediaCatalogModel::DeleteAllowedRole).toBool());
    QVERIFY(!first.data(
        MediaCatalogModel::DeviceCopyAllowedRole).toBool());
    QVERIFY(first.data(
        MediaCatalogModel::MediaIdRole).toString().isEmpty());
}

void QuickClientTests::
    legacyScreenConfigKeepsManager1Shape() {
    TryxRuntimeApplyRequest request;
    request.media = {
        QStringLiteral("left.mp4"),
        QStringLiteral("right.mp4"),
    };
    request.ratio = QStringLiteral("2:1");
    request.screenMode =
        QStringLiteral("Screen Splitting");
    request.playMode = QStringLiteral("Single");
    request.sysinfoLabels =
        {QStringLiteral("CPU Temperature")};
    request.settingsPosition = QStringLiteral("Top");
    request.settingsColor = QStringLiteral("#dcdcdc");
    request.settingsAlign = QStringLiteral("Left");
    request.settingsBadges =
        {QStringLiteral("CPU Badge")};
    request.filterOpacity = 12;
    request.presetId = QStringLiteral("custom");
    request.sysinfoLabels2 =
        {QStringLiteral("GPU Temperature")};
    request.settingsBadges2 =
        {QStringLiteral("GPU Badge")};
    request.waterfallMode = true;

    const QVariantList arguments =
        RuntimeClient::legacyScreenConfigArguments(request);
    QCOMPARE(arguments.size(), 14);
    QCOMPARE(arguments.at(0).toStringList(), request.media);
    QCOMPARE(arguments.at(2).toString(), request.screenMode);
    QCOMPARE(arguments.at(9).toInt(), request.filterOpacity);
    QCOMPARE(
        arguments.at(11).toStringList(),
        request.sysinfoLabels2);
    QCOMPARE(arguments.at(13).toBool(), true);
}

void QuickClientTests::legacyTransformBoundaryIsExplicit() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.connection_.revision = 1;
    runtime.connection_.connected = true;

    TryxRuntimeMediaTransform transform =
        tryxLegacyFitMediaTransform();
    transform.mode = QStringLiteral("Fill");
    const QString operationId =
        runtime.queueUploadWithTransform(
            QStringLiteral("/tmp/source.mp4"), transform);
    QVERIFY(operationId.isEmpty());
    QVERIFY(runtime.diagnostic().contains(
        QStringLiteral("default Fit")));
    QVERIFY(!runtime.operationBusy());
}

void QuickClientTests::
    legacyUploadRetainsSourceUntilTerminalSignal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath =
        QDir(directory.path()).filePath(
            QStringLiteral("source.mp4"));
    QFile source(sourcePath);
    QVERIFY(source.open(
        QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(source.write("legacy-upload"), qint64(13));
    source.close();
    QVERIFY(QFile::setPermissions(
        sourcePath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner));

    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.connection_.revision = 1;
    runtime.connection_.connected = true;

    QString claimError;
    const QString operationId =
        QStringLiteral("legacy-operation");
    QVERIFY(runtime.claimLegacyUploadSource(
        operationId, sourcePath, &claimError));
    QVERIFY(claimError.isEmpty());
    const QString claimedPath =
        runtime.legacyUpload_.claimedPath;
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(QFileInfo::exists(claimedPath));
    QVERIFY(runtime.operationBusy());

    QSignalSpy rejected(
        &runtime,
        &RuntimeClient::operationRequestRejected);
    QSignalSpy accepted(
        &runtime,
        &RuntimeClient::operationRequestAccepted);
    runtime.onLegacyUploadTimeout();
    QCOMPARE(rejected.count(), 1);
    QVERIFY(runtime.operationBusy());
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(QFileInfo::exists(claimedPath));

    QVERIFY(QFile::remove(sourcePath));
    QVERIFY(QFileInfo::exists(claimedPath));
    runtime.onLegacyMediaUploaded(
        QStringLiteral("uploaded.mp4"), 2);
    QVERIFY(!runtime.operationBusy());
    QVERIFY(!QFileInfo::exists(claimedPath));
    QCOMPARE(accepted.count(), 0);
}

void QuickClientTests::operationsExposeStableRoles() {
    OperationListModel model;
    const QHash<int, QByteArray> roles = model.roleNames();
    QCOMPARE(roles.value(OperationListModel::StateRole),
             QByteArray("operationState"));
    QCOMPARE(roles.value(OperationListModel::ProgressRole),
             QByteArray("progress"));

    TryxRuntimeOperationsSnapshot snapshot;
    snapshot.revision = 1;
    TryxRuntimeOperationInfo info;
    info.id = QStringLiteral("operation");
    info.state = QStringLiteral("Uploading");
    info.completed = 25;
    info.total = 100;
    snapshot.operations.append(info);
    model.applySnapshot(snapshot);

    QCOMPARE(
        model.data(model.index(0), OperationListModel::ProgressRole)
            .toDouble(),
        0.25);
    QVERIFY(!model.data(model.index(0),
                        OperationListModel::TerminalRole)
                 .toBool());
}

void QuickClientTests::operationsRejectStaleEvents() {
    OperationListModel model;
    TryxRuntimeOperationInfo current;
    current.id = QStringLiteral("current");
    current.state = QStringLiteral("Uploading");
    QVERIFY(model.upsert(current, 10));

    TryxRuntimeOperationInfo stale = current;
    stale.state = QStringLiteral("Failed");
    QVERIFY(!model.upsert(stale, 9));
    QCOMPARE(
        model.data(model.index(0), OperationListModel::StateRole)
            .toString(),
        QStringLiteral("Uploading"));

    QVERIFY(!model.remove(current.id, 9));
    QCOMPARE(model.rowCount(), 1);

    TryxRuntimeOperationsSnapshot staleSnapshot;
    staleSnapshot.revision = 8;
    QVERIFY(!model.applySnapshot(staleSnapshot));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.revision(), 10U);
}

void QuickClientTests::
    displayStateRequiresStrictlyIncreasingRevision() {
    RuntimeClient runtime(true);
    runtime.capabilitiesReady_ = true; // Negotiated Auto-only runtime.
    runtime.compatible_ = true;
    runtime.connection_.printerClassConnected = true;
    runtime.connection_.printerClassDevicePresent = true;
    runtime.connection_.serial = QStringLiteral("device-a");
    QSignalSpy displaySpy(
        &runtime, &RuntimeClient::displayChanged);

    TryxRuntimeDisplayState initial;
    initial.revision = 0;
    initial.deviceSerial = QStringLiteral("device-a");
    initial.valid = true;
    initial.brightness = 41;
    runtime.applyDisplayState(initial);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(runtime.brightness(), 41);

    TryxRuntimeDisplayState duplicate = initial;
    duplicate.brightness = 99;
    runtime.applyDisplayState(duplicate);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(runtime.brightness(), 41);

    TryxRuntimeDisplayState newer = initial;
    newer.revision = 1;
    newer.brightness = 73;
    runtime.applyDisplayState(newer);
    QCOMPARE(displaySpy.count(), 2);
    QCOMPARE(runtime.brightness(), 73);

    runtime.applyDisplayState(initial);
    QCOMPARE(displaySpy.count(), 2);
    QCOMPARE(runtime.brightness(), 73);

    runtime.clearRuntimeState();
    runtime.compatible_ = true;
    runtime.capabilitiesReady_ = true; // The restarted Auto-only runtime renegotiated.
    runtime.connection_.printerClassConnected = true;
    runtime.connection_.printerClassDevicePresent = true;
    runtime.connection_.serial = QStringLiteral("device-a");
    displaySpy.clear();
    TryxRuntimeDisplayState afterRestart = initial;
    afterRestart.brightness = 64;
    runtime.applyDisplayState(afterRestart);
    QCOMPARE(displaySpy.count(), 1);
    QCOMPARE(runtime.brightness(), 64);

    RuntimeClient legacyRuntime(true);
    legacyRuntime.compatible_ = true;
    legacyRuntime.connection_.connected = true;
    legacyRuntime.connection_.serial = QStringLiteral("legacy-a");
    legacyRuntime.onLegacyBrightnessChanged(55, 5);
    QCOMPARE(legacyRuntime.brightness(), 55);

    TryxRuntimeDisplayState staleAfterLegacy;
    staleAfterLegacy.revision = 4;
    staleAfterLegacy.valid = true;
    staleAfterLegacy.brightness = 99;
    legacyRuntime.applyDisplayState(staleAfterLegacy);
    QCOMPARE(legacyRuntime.brightness(), 55);
}

void QuickClientTests::
    untrustedDirectMetricsAndDisplayCallbacksAreDiscarded() {
    RuntimeClient runtime(true);
    runtime.compatible_ = true;
    runtime.metrics_.revision = 1;
    runtime.metrics_.availableMetrics = {
        QStringLiteral("CPU Temperature")};
    runtime.display_.revision = 1;
    runtime.display_.brightness = 41;

    TryxRuntimeMetricsState untrustedMetrics;
    untrustedMetrics.revision = 99;
    untrustedMetrics.availableMetrics = {
        QStringLiteral("GPU Power")};
    runtime.onMetricsStateUpdated(untrustedMetrics);
    QCOMPARE(
        runtime.availableMetrics(),
        QStringList{QStringLiteral("CPU Temperature")});

    TryxRuntimeDisplayState untrustedDisplay;
    untrustedDisplay.revision = 99;
    untrustedDisplay.valid = true;
    untrustedDisplay.brightness = 99;
    runtime.onDisplayStateUpdated(untrustedDisplay);
    QCOMPARE(runtime.brightness(), 41);
}

void QuickClientTests::
    operationAcknowledgementRequiresExactIdentity() {
    const QString expected =
        QStringLiteral(
            "11111111-1111-4111-8111-111111111111");
    TryxRuntimeOperationInfo observed;

    QVERIFY(RuntimeClient::operationAcknowledgementMatches(
        expected, expected, observed));
    QVERIFY(!RuntimeClient::operationAcknowledgementMatches(
        expected,
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222"),
        observed));
    QVERIFY(!RuntimeClient::operationAcknowledgementMatches(
        expected, QString(), observed));

    observed.id = expected;
    QVERIFY(RuntimeClient::operationAcknowledgementMatches(
        expected, QString(), observed));
    QVERIFY(RuntimeClient::operationAcknowledgementMatches(
        expected,
        QStringLiteral(
            "22222222-2222-4222-8222-222222222222"),
        observed));
    QVERIFY(!RuntimeClient::operationAcknowledgementMatches(
        QString(), QString(), observed));
}

void QuickClientTests::cacheCleanupRequestsAreExactAndTracked() {
    RuntimeClient unsupported(true);
    unsupported.serviceAvailable_ = true;
    unsupported.compatible_ = true;
    unsupported.capabilitiesReady_ = true;
    QVERIFY(unsupported.queueCacheCleanup().isEmpty());
    QVERIFY(unsupported.offlineRequests_.isEmpty());

    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeCacheCleanupV1Token(),
    };
    const QString operationId = runtime.queueCacheCleanup();
    QVERIFY(!operationId.isEmpty());
    QCOMPARE(
        QUuid(operationId).toString(QUuid::WithoutBraces),
        operationId);
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("QueueCacheCleanupV1"));
    QCOMPARE(runtime.offlineRequests_.constLast().arguments,
             QVariantList{operationId});
    QCOMPARE(runtime.offlineRequests_.constLast().operationId,
             operationId);
    QCOMPARE(runtime.offlineRequests_.constLast().kind,
             QStringLiteral("CacheCleanup"));

    QVERIFY(runtime.queueCacheCleanup().isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    QVERIFY(!runtime.cancelCacheCleanup(
        QStringLiteral("other-operation")));
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    QVERIFY(runtime.cancelCacheCleanup(operationId));
    QCOMPARE(runtime.offlineRequests_.size(), 2);
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("CancelOperation"));
    QCOMPARE(runtime.offlineRequests_.constLast().arguments,
             QVariantList{operationId});

    QVERIFY(runtime.refreshCacheCleanup(operationId));
    QCOMPARE(runtime.offlineRequests_.size(), 3);
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("GetOperation"));
    QCOMPARE(runtime.offlineRequests_.constLast().arguments,
             QVariantList{operationId});

    runtime.clearCacheCleanupTracking(operationId);
    const QString nextId = runtime.queueCacheCleanup();
    QVERIFY(!nextId.isEmpty());
    QVERIFY(nextId != operationId);
    QCOMPARE(runtime.offlineRequests_.size(), 4);
}

void QuickClientTests::
    cacheManagementRunsRuntimeBeforePreviewCleanup() {
    QTemporaryDir runtimeDirectory;
    QVERIFY(runtimeDirectory.isValid());
    QVERIFY(QFile::setPermissions(
        runtimeDirectory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    ScopedEnvironmentVariable runtimeRoot("XDG_RUNTIME_DIR");
    runtimeRoot.set(QFile::encodeName(runtimeDirectory.path()));
    const QString previewDirectory =
        QDir(runtimeDirectory.path()).filePath(
            QStringLiteral("tryx-panorama-manager/previews"));
    QString directoryError;
    QVERIFY(MediaPreviewController::ensurePrivateDirectoryTree(
        previewDirectory, &directoryError));
    const QByteArray localBytes("local-preview");
    const QString localPath = QDir(previewDirectory).filePath(
        QStringLiteral(
            "44444444-4444-4444-8444-444444444444-4.png"));
    QFile local(localPath);
    QVERIFY(local.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(local.write(localBytes), localBytes.size());
    local.close();
    QVERIFY(QFile::setPermissions(
        localPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeCacheCleanupV1Token(),
    };
    MediaEditorController editor(&runtime);
    CacheManagementController controller(&runtime, &editor);
    QCOMPARE(controller.state(), QStringLiteral("Ready"));
    QCOMPARE(controller.phase(), QStringLiteral("Idle"));
    QVERIFY(controller.canStart());
    const quint64 staleRevision = controller.eligibilityRevision();
    emit runtime.connectionChanged();
    QVERIFY(controller.eligibilityRevision() > staleRevision);
    QVERIFY(!controller.startCleanup(staleRevision));
    QVERIFY(runtime.offlineRequests_.isEmpty());
    QVERIFY(controller.startCleanup(
        controller.eligibilityRevision()));
    QCOMPARE(controller.state(), QStringLiteral("Running"));
    QCOMPARE(controller.phase(), QStringLiteral("Runtime"));
    QVERIFY(controller.canCancel());
    QVERIFY(editor.previewCleanupInterlockActive());
    QVERIFY(QFileInfo::exists(localPath));
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const QString operationId =
        runtime.offlineRequests_.constFirst().operationId;

    TryxRuntimeOperationInfo running;
    running.id = operationId;
    running.kind = QStringLiteral("CacheCleanup");
    running.state = QStringLiteral("Running");
    running.stage = QStringLiteral("CleaningCatalog");
    running.total = 2;
    emit runtime.operationUpdated(running);
    QVERIFY(QFileInfo::exists(localPath));

    TryxRuntimeOperationInfo succeeded = running;
    succeeded.state = QStringLiteral("Succeeded");
    succeeded.stage = QStringLiteral("Succeeded");
    succeeded.terminalOutcome = QStringLiteral("Succeeded");
    succeeded.completed = 2;
    succeeded.confirmedBytes = 21;
    emit runtime.operationUpdated(succeeded);

    QCOMPARE(controller.state(), QStringLiteral("Running"));
    QCOMPARE(controller.phase(), QStringLiteral("Quick"));
    QVERIFY(!controller.canCancel());
    QVERIFY(QFileInfo::exists(localPath));
    QTRY_COMPARE(controller.state(), QStringLiteral("Succeeded"));
    QCOMPARE(controller.phase(), QStringLiteral("Idle"));
    QCOMPARE(controller.runtimeRemovedFiles(), 2);
    QCOMPARE(controller.localRemovedFiles(), 1);
    QCOMPARE(controller.removedFiles(), 3);
    QCOMPARE(controller.removedBytes(),
             qint64(21 + localBytes.size()));
    QVERIFY(!QFileInfo::exists(localPath));
    QVERIFY(!editor.previewCleanupInterlockActive());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    QVERIFY(controller.canStart());
}

void QuickClientTests::
    cacheManagementPreservesPartialAndUnresolvedOutcomes() {
    QTemporaryDir runtimeDirectory;
    QVERIFY(runtimeDirectory.isValid());
    QVERIFY(QFile::setPermissions(
        runtimeDirectory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    ScopedEnvironmentVariable runtimeRoot("XDG_RUNTIME_DIR");
    runtimeRoot.set(QFile::encodeName(runtimeDirectory.path()));
    const QString previewDirectory =
        QDir(runtimeDirectory.path()).filePath(
            QStringLiteral("tryx-panorama-manager/previews"));
    QString directoryError;
    QVERIFY(MediaPreviewController::ensurePrivateDirectoryTree(
        previewDirectory, &directoryError));
    const QString localPath = QDir(previewDirectory).filePath(
        QStringLiteral(
            "55555555-5555-4555-8555-555555555555-5.png"));
    QFile local(localPath);
    QVERIFY(local.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(local.write("partial-local"), qint64(13));
    local.close();
    QVERIFY(QFile::setPermissions(
        localPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeCacheCleanupV1Token(),
    };
    MediaEditorController editor(&runtime);
    editor.preview_.cleanupFsyncFunctionForTesting_ =
        [](int) {
            errno = EIO;
            return -1;
        };
    CacheManagementController controller(&runtime, &editor);
    QVERIFY(controller.startCleanup(
        controller.eligibilityRevision()));
    const QString operationId =
        runtime.offlineRequests_.constFirst().operationId;
    TryxRuntimeOperationInfo succeeded;
    succeeded.id = operationId;
    succeeded.kind = QStringLiteral("CacheCleanup");
    succeeded.state = QStringLiteral("Succeeded");
    succeeded.stage = QStringLiteral("Succeeded");
    succeeded.terminalOutcome = QStringLiteral("Succeeded");
    succeeded.total = 1;
    succeeded.completed = 1;
    succeeded.confirmedBytes = 7;
    emit runtime.operationUpdated(succeeded);
    QCOMPARE(controller.state(), QStringLiteral("Running"));
    QCOMPARE(controller.phase(), QStringLiteral("Quick"));
    QTRY_COMPARE(controller.state(), QStringLiteral("Partial"));
    QCOMPARE(controller.phase(), QStringLiteral("Idle"));
    QCOMPARE(controller.runtimeRemovedFiles(), 1);
    QCOMPARE(controller.localRemovedFiles(), 1);
    QCOMPARE(controller.removedFiles(), 2);
    QCOMPARE(controller.removedBytes(), 20);
    QVERIFY(!QFileInfo::exists(localPath));
    QVERIFY(!editor.previewCleanupInterlockActive());
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    editor.preview_.cleanupFsyncFunctionForTesting_ = {};
    QVERIFY(controller.startCleanup(
        controller.eligibilityRevision()));
    const QString unresolvedId =
        runtime.offlineRequests_.constLast().operationId;
    emit runtime.runtimeInvalidated();
    QCOMPARE(controller.state(), QStringLiteral("Unresolved"));
    QVERIFY(controller.canRefresh());
    QVERIFY(controller.requiresAcknowledgment());
    QVERIFY(!editor.previewCleanupInterlockActive());
    QVERIFY(controller.refresh());
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("GetOperation"));

    TryxRuntimeOperationInfo cancelled;
    cancelled.id = unresolvedId;
    cancelled.kind = QStringLiteral("CacheCleanup");
    cancelled.state = QStringLiteral("Cancelled");
    cancelled.stage = QStringLiteral("Cancelled");
    cancelled.terminalOutcome = QStringLiteral("Cancelled");
    cancelled.errorCategory = QStringLiteral("UserCancelled");
    emit runtime.cacheCleanupRefreshResolved(cancelled);
    QCOMPARE(controller.state(), QStringLiteral("Cancelled"));
    QVERIFY(!controller.requiresAcknowledgment());

    QVERIFY(controller.startCleanup(
        controller.eligibilityRevision()));
    emit runtime.runtimeInvalidated();
    QCOMPARE(controller.state(), QStringLiteral("Unresolved"));
    controller.acknowledgeUnresolved();
    QVERIFY(!controller.requiresAcknowledgment());
    QCOMPARE(controller.state(), QStringLiteral("Ready"));

    {
        CacheManagementController scoped(&runtime, &editor);
        QVERIFY(scoped.startCleanup(
            scoped.eligibilityRevision()));
        QVERIFY(editor.previewCleanupInterlockActive());
    }
    QVERIFY(!editor.previewCleanupInterlockActive());
}

void QuickClientTests::cacheManagementTerminalMatrixIsExact() {
    QTemporaryDir runtimeDirectory;
    QVERIFY(runtimeDirectory.isValid());
    QVERIFY(QFile::setPermissions(
        runtimeDirectory.path(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    ScopedEnvironmentVariable runtimeRoot("XDG_RUNTIME_DIR");
    runtimeRoot.set(QFile::encodeName(runtimeDirectory.path()));

    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeCacheCleanupV1Token(),
    };
    MediaEditorController editor(&runtime);
    CacheManagementController controller(&runtime, &editor);

    const auto start = [&]() -> QString {
        if (!controller.startCleanup(
                controller.eligibilityRevision())) {
            return {};
        }
        return runtime.offlineRequests_.constLast().operationId;
    };
    const auto terminal = [](const QString &operationId) {
        TryxRuntimeOperationInfo info;
        info.id = operationId;
        info.kind = QStringLiteral("CacheCleanup");
        return info;
    };

    const QString blockedId = start();
    QVERIFY(!blockedId.isEmpty());
    TryxRuntimeOperationInfo blocked = terminal(blockedId);
    blocked.state = QStringLiteral("Failed");
    blocked.stage = QStringLiteral("Rejected");
    blocked.terminalOutcome = QStringLiteral("NotStarted");
    blocked.errorCategory = QStringLiteral("Busy");
    emit runtime.operationUpdated(blocked);
    QCOMPARE(controller.state(), QStringLiteral("Blocked"));
    QCOMPARE(controller.phase(), QStringLiteral("Idle"));
    QVERIFY(!controller.requiresAcknowledgment());
    QVERIFY(!editor.previewCleanupInterlockActive());

    const QString partialId = start();
    QVERIFY(!partialId.isEmpty());
    TryxRuntimeOperationInfo partial = terminal(partialId);
    partial.state = QStringLiteral("Failed");
    partial.stage = QStringLiteral("Failed");
    partial.terminalOutcome = QStringLiteral("PartialCleanup");
    partial.errorCategory = QStringLiteral("CacheCleanupFailed");
    partial.total = 3;
    partial.completed = 1;
    partial.confirmedBytes = 9;
    emit runtime.operationUpdated(partial);
    QCOMPARE(controller.state(), QStringLiteral("Partial"));
    QCOMPARE(controller.removedFiles(), 1);
    QCOMPARE(controller.removedBytes(), 9);
    QVERIFY(!controller.requiresAcknowledgment());

    const QString cancelledId = start();
    QVERIFY(!cancelledId.isEmpty());
    TryxRuntimeOperationInfo cancelled = terminal(cancelledId);
    cancelled.state = QStringLiteral("Cancelled");
    cancelled.stage = QStringLiteral("Cancelled");
    cancelled.terminalOutcome = QStringLiteral("Cancelled");
    cancelled.errorCategory = QStringLiteral("UserCancelled");
    emit runtime.operationUpdated(cancelled);
    QCOMPARE(controller.state(), QStringLiteral("Cancelled"));
    QVERIFY(!controller.requiresAcknowledgment());

    const QString unexpectedId = start();
    QVERIFY(!unexpectedId.isEmpty());
    TryxRuntimeOperationInfo unexpected = terminal(unexpectedId);
    unexpected.state = QStringLiteral("Failed");
    unexpected.stage = QStringLiteral("Rejected");
    unexpected.terminalOutcome = QStringLiteral("NotStarted");
    unexpected.errorCategory = QStringLiteral("UnknownBlocker");
    emit runtime.operationUpdated(unexpected);
    QCOMPARE(controller.state(), QStringLiteral("Unresolved"));
    QVERIFY(controller.canRefresh());
    QVERIFY(controller.requiresAcknowledgment());
    QVERIFY(!editor.previewCleanupInterlockActive());
    controller.acknowledgeUnresolved();

    const QString malformedId = start();
    QVERIFY(!malformedId.isEmpty());
    TryxRuntimeOperationInfo malformed = terminal(malformedId);
    malformed.state = QStringLiteral("Succeeded");
    malformed.stage = QStringLiteral("Succeeded");
    malformed.terminalOutcome = QStringLiteral("Succeeded");
    malformed.total = 1;
    malformed.completed = 2;
    emit runtime.operationUpdated(malformed);
    QCOMPARE(controller.state(), QStringLiteral("Unresolved"));
    QVERIFY(controller.requiresAcknowledgment());
    controller.acknowledgeUnresolved();

    const QString unknownStateId = start();
    QVERIFY(!unknownStateId.isEmpty());
    TryxRuntimeOperationInfo unknownState =
        terminal(unknownStateId);
    unknownState.state = QStringLiteral("Queued");
    emit runtime.operationUpdated(unknownState);
    QCOMPARE(controller.state(), QStringLiteral("Unresolved"));
    QVERIFY(controller.requiresAcknowledgment());
    QVERIFY(!editor.previewCleanupInterlockActive());
    controller.acknowledgeUnresolved();
}

void QuickClientTests::succeededOperationClearsBusyState() {
    RuntimeClient runtime(true);
    runtime.compatible_ = true;
    TryxRuntimeOperationInfo operation;
    operation.id = QStringLiteral("operation");
    operation.state = QStringLiteral("Uploading");

    QVERIFY(QMetaObject::invokeMethod(
        &runtime, "onOperationChanged", Qt::DirectConnection,
        Q_ARG(TryxRuntimeOperationInfo, operation),
        Q_ARG(quint64, 1)));
    QVERIFY(runtime.operationBusy());

    operation.state = QStringLiteral("Succeeded");
    QVERIFY(QMetaObject::invokeMethod(
        &runtime, "onOperationChanged", Qt::DirectConnection,
        Q_ARG(TryxRuntimeOperationInfo, operation),
        Q_ARG(quint64, 2)));
    QVERIFY(!runtime.operationBusy());
    QVERIFY(OperationListModel::isTerminal(operation));
}

void QuickClientTests::
    mediaEditorMetadataWaitsForCapabilityHandshake() {
    const TryxRuntimeDeviceMediaArtifact firstArtifact =
        deviceMediaArtifact(
            QStringLiteral(
                "51515151-5151-4151-8151-515151515151"),
            QStringLiteral(
                "52525252-5252-4252-8252-525252525252"),
            QString(64, QLatin1Char('c')),
            QStringLiteral("first-waiting-copy.h264"),
            QStringLiteral("device-a"));
    const TryxRuntimeDeviceMediaArtifact secondArtifact =
        deviceMediaArtifact(
            QStringLiteral(
                "53535353-5353-4353-8353-535353535353"),
            QStringLiteral(
                "54545454-5454-4454-8454-545454545454"),
            QString(64, QLatin1Char('d')),
            QStringLiteral("second-waiting-copy.h264"),
            QStringLiteral("device-a"));

    RuntimeClient delayedRuntime(true);
    MediaEditorController delayedEditor(&delayedRuntime);
    delayedEditor.beginRecoveredVideo(firstArtifact);
    QCOMPARE(delayedEditor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    QVERIFY(delayedRuntime.offlineRequests_.isEmpty());

    delayedEditor.beginRecoveredVideo(secondArtifact);
    QCOMPARE(delayedEditor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    QVERIFY(delayedRuntime.offlineRequests_.isEmpty());

    delayedRuntime.capabilitiesReady_ = true;
    delayedRuntime.runtimeCapabilities_ = {
        tryxRuntimeDeviceMediaMetadataV1Token()};
    emit delayedRuntime.capabilitiesChanged();
    QCOMPARE(delayedRuntime.offlineRequests_.size(), 1);
    QCOMPARE(delayedRuntime.offlineRequests_.constFirst().method,
             QStringLiteral("GetDeviceMediaMetadataV1"));
    QCOMPARE(delayedRuntime.offlineRequests_.constFirst().arguments,
             QVariantList({secondArtifact.artifactId,
                           secondArtifact.leaseId}));
    emit delayedRuntime.capabilitiesChanged();
    QCOMPARE(delayedRuntime.offlineRequests_.size(), 1);
    delayedEditor.cancel();

    RuntimeClient cancelledRuntime(true);
    MediaEditorController cancelledEditor(&cancelledRuntime);
    cancelledEditor.beginRecoveredVideo(firstArtifact);
    QCOMPARE(cancelledEditor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    cancelledEditor.cancel();
    cancelledRuntime.capabilitiesReady_ = true;
    cancelledRuntime.runtimeCapabilities_ = {
        tryxRuntimeDeviceMediaMetadataV1Token()};
    emit cancelledRuntime.capabilitiesChanged();
    QVERIFY(cancelledRuntime.offlineRequests_.isEmpty());
}

void QuickClientTests::
    mediaEditorDeviceCopyMetadataIsArtifactScoped() {
    const TryxRuntimeDeviceMediaArtifact firstArtifact =
        deviceMediaArtifact(
            QStringLiteral(
                "11111111-1111-4111-8111-111111111111"),
            QStringLiteral(
                "22222222-2222-4222-8222-222222222222"),
            QString(64, QLatin1Char('a')),
            QStringLiteral("first-device-copy.h264"),
            QStringLiteral("device-a"));
    const TryxRuntimeDeviceMediaArtifact secondArtifact =
        deviceMediaArtifact(
            QStringLiteral(
                "33333333-3333-4333-8333-333333333333"),
            QStringLiteral(
                "44444444-4444-4444-8444-444444444444"),
            QString(64, QLatin1Char('b')),
            QStringLiteral("second-device-copy.h264"),
            QStringLiteral("device-a"));
    const auto metadataFor = [](
        const TryxRuntimeDeviceMediaArtifact &artifact,
        const QString &status) {
        TryxRuntimeDeviceMediaMetadataV1 metadata;
        metadata.operationId = artifact.operationId;
        metadata.artifactId = artifact.artifactId;
        metadata.mediaId = artifact.mediaId;
        metadata.deviceIdentity = artifact.deviceIdentity;
        metadata.decodedSha256 = artifact.decodedSha256;
        metadata.deviceGeneration = 7;
        metadata.status = status;
        if (status == QStringLiteral("Ready")) {
            metadata.availableFields =
                kTryxDeviceMediaMetadataAllFields;
            metadata.width = 2240;
            metadata.height = 1080;
            metadata.durationMilliseconds = 60000;
            metadata.frameRateNumerator = 30;
            metadata.frameRateDenominator = 1;
        } else if (status == QStringLiteral("Partial")) {
            metadata.availableFields =
                kTryxDeviceMediaMetadataDimensions;
            metadata.width = 2240;
            metadata.height = 1080;
        }
        return metadata;
    };

    RuntimeClient unsupportedRuntime(true);
    unsupportedRuntime.capabilitiesReady_ = true;
    MediaEditorController unsupportedEditor(&unsupportedRuntime);
    unsupportedEditor.beginRecoveredVideo(firstArtifact);
    QCOMPARE(unsupportedEditor.deviceCopyMetadataStatus(),
             QStringLiteral("NotSupported"));
    QVERIFY(!unsupportedEditor.deviceCopyDimensionsAvailable());
    QVERIFY(unsupportedRuntime.offlineRequests_.isEmpty());
    unsupportedEditor.cancel();

    RuntimeClient runtime(true);
    runtime.capabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {
        tryxRuntimeDeviceMediaMetadataV1Token()};
    runtime.connection_.printerClassDevicePresent = true;
    runtime.display_.valid = true;
    runtime.display_.deviceSerial = QStringLiteral("device-a");
    runtime.display_.screenMode = QStringLiteral("Full Screen");
    runtime.display_.media = {
        firstArtifact.remoteName,
        secondArtifact.remoteName,
    };
    MediaEditorController editor(&runtime);

    editor.beginRecoveredVideo(firstArtifact);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("GetDeviceMediaMetadataV1"));
    QCOMPARE(runtime.offlineRequests_.constLast().arguments,
             QVariantList({firstArtifact.artifactId,
                           firstArtifact.leaseId}));
    QVERIFY(!editor.deviceCopyDimensionsAvailable());
    QVERIFY(!editor.deviceCopyDurationAvailable());
    QVERIFY(!editor.deviceCopyFrameRateAvailable());

    const TryxRuntimeDeviceMediaMetadataV1 firstReady =
        metadataFor(firstArtifact, QStringLiteral("Ready"));
    QVERIFY(tryxRuntimeDeviceMediaMetadataV1IsValid(firstReady));
    emit runtime.deviceMediaMetadataReady(firstReady);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Ready"));
    QVERIFY(editor.deviceCopyDimensionsAvailable());
    QCOMPARE(editor.deviceCopyWidth(), 2240);
    QCOMPARE(editor.deviceCopyHeight(), 1080);
    QVERIFY(editor.deviceCopyDurationAvailable());
    QCOMPARE(editor.deviceCopyDurationMilliseconds(), 60000.0);
    QVERIFY(editor.deviceCopyFrameRateAvailable());
    QCOMPARE(editor.deviceCopyFrameRateNumerator(), 30);
    QCOMPARE(editor.deviceCopyFrameRateDenominator(), 1);

    editor.beginRecoveredVideo(secondArtifact);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    QCOMPARE(runtime.offlineRequests_.size(), 2);
    emit runtime.deviceMediaMetadataReady(firstReady);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));

    TryxRuntimeDeviceMediaMetadataV1 malformed =
        metadataFor(secondArtifact, QStringLiteral("Ready"));
    malformed.availableFields =
        kTryxDeviceMediaMetadataDimensions;
    malformed.durationMilliseconds = 0;
    malformed.frameRateNumerator = 0;
    malformed.frameRateDenominator = 0;
    QVERIFY(!tryxRuntimeDeviceMediaMetadataV1IsValid(malformed));
    emit runtime.deviceMediaMetadataReady(malformed);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Unavailable"));
    QVERIFY(editor.recoveredDeviceCopy());
    QVERIFY(editor.replaceAllowed());
    QVERIFY(!editor.deviceCopyDimensionsAvailable());

    editor.beginRecoveredVideo(secondArtifact);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    emit runtime.deviceMediaMetadataFailed(
        firstArtifact.artifactId,
        QStringLiteral("stale metadata failure"));
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Loading"));
    emit runtime.deviceMediaMetadataFailed(
        secondArtifact.artifactId,
        QStringLiteral("metadata unavailable"));
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Unavailable"));
    QVERIFY(editor.recoveredDeviceCopy());
    QVERIFY(editor.replaceAllowed());

    editor.beginRecoveredVideo(secondArtifact);
    const TryxRuntimeDeviceMediaMetadataV1 probeFailed =
        metadataFor(secondArtifact, QStringLiteral("ProbeFailed"));
    QVERIFY(tryxRuntimeDeviceMediaMetadataV1IsValid(probeFailed));
    emit runtime.deviceMediaMetadataReady(probeFailed);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Unavailable"));
    QVERIFY(editor.recoveredDeviceCopy());

    editor.beginRecoveredVideo(secondArtifact);
    const TryxRuntimeDeviceMediaMetadataV1 secondPartial =
        metadataFor(secondArtifact, QStringLiteral("Partial"));
    QVERIFY(tryxRuntimeDeviceMediaMetadataV1IsValid(secondPartial));
    emit runtime.deviceMediaMetadataReady(secondPartial);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Partial"));
    QVERIFY(editor.deviceCopyDimensionsAvailable());
    QVERIFY(!editor.deviceCopyDurationAvailable());
    QVERIFY(!editor.deviceCopyFrameRateAvailable());

    editor.cancel();
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("NotSupported"));
    QVERIFY(!editor.recoveredDeviceCopy());
    QVERIFY(!editor.deviceCopyDimensionsAvailable());
    emit runtime.deviceMediaMetadataReady(secondPartial);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("NotSupported"));

    editor.beginRecoveredVideo(secondArtifact);
    emit runtime.deviceMediaMetadataReady(
        metadataFor(secondArtifact, QStringLiteral("Ready")));
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("Ready"));
    editor.begin(QUrl::fromLocalFile(QStringLiteral(
        "/nonexistent/tryx-c12-local-source.png")));
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("NotSupported"));
    QVERIFY(!editor.recoveredDeviceCopy());
    QVERIFY(!editor.deviceCopyDimensionsAvailable());
    emit runtime.deviceMediaMetadataReady(secondPartial);
    QCOMPARE(editor.deviceCopyMetadataStatus(),
             QStringLiteral("NotSupported"));
}

void QuickClientTests::
    deviceMediaWorkflowRejectsMismatchedClaimIdentity() {
    const QString mediaId = QStringLiteral("media-1");
    const QString mediaName =
        QStringLiteral("source.mp4.h264_2240x1080");
    const QString deviceIdentity =
        QStringLiteral("device-1");
    const QString artifactId =
        QStringLiteral("artifact-1");

    RuntimeClient runtime(true);
    preparePaseDeviceMedia(
        &runtime, mediaId, mediaName, deviceIdentity);
    MediaEditorController editor(&runtime);
    DeviceMediaWorkflowController workflow(
        &runtime, &editor);

    workflow.beginEdit(mediaId, mediaName);
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const QString stageOperationId =
        runtime.offlineRequests_.constFirst().operationId;
    QVERIFY(!stageOperationId.isEmpty());

    TryxRuntimeOperationInfo staged;
    staged.id = stageOperationId;
    staged.kind = QStringLiteral("StageDeviceMedia");
    staged.state = QStringLiteral("Succeeded");
    staged.resultName = artifactId;
    emit runtime.operationUpdated(staged);

    QCOMPARE(runtime.offlineRequests_.size(), 2);
    QCOMPARE(
        runtime.offlineRequests_.constLast().method,
        QStringLiteral("ClaimDeviceMediaArtifact"));

    TryxRuntimeDeviceMediaArtifact stale =
        deviceMediaArtifact(
            QStringLiteral("stale-operation"), artifactId,
            mediaId, mediaName, deviceIdentity);
    emit runtime.artifactClaimed(
        stageOperationId, stale);

    QCOMPARE(runtime.offlineRequests_.size(), 3);
    const RuntimeClient::OfflineRequest release =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        release.method,
        QStringLiteral("ReleaseDeviceMediaArtifact"));
    QCOMPARE(
        release.arguments,
        QVariantList({artifactId, stale.leaseId}));
    QVERIFY(!workflow.busy());
    QVERIFY(!workflow.error().isEmpty());
    QVERIFY(!editor.recoveredDeviceCopy());

    const int requestCount =
        runtime.offlineRequests_.size();
    emit runtime.artifactClaimed(
        stageOperationId,
        deviceMediaArtifact(
            stageOperationId, artifactId, mediaId,
            mediaName, deviceIdentity));
    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
}

void QuickClientTests::
    deviceMediaWorkflowSaveAsNewCompletesAndReleasesLease() {
    const QString mediaId = QStringLiteral("media-1");
    const QString mediaName =
        QStringLiteral("source.mp4.h264_2240x1080");
    const QString deviceIdentity =
        QStringLiteral("device-1");
    const QString artifactId =
        QStringLiteral("artifact-1");

    RuntimeClient runtime(true);
    preparePaseDeviceMedia(
        &runtime, mediaId, mediaName, deviceIdentity);
    MediaEditorController editor(&runtime);
    DeviceMediaWorkflowController workflow(
        &runtime, &editor);

    workflow.beginEdit(mediaId, mediaName);
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const RuntimeClient::OfflineRequest stageRequest =
        runtime.offlineRequests_.constFirst();
    QCOMPARE(
        stageRequest.method,
        QStringLiteral("QueueStageDeviceMedia"));
    QCOMPARE(
        stageRequest.arguments.at(1).toString(), mediaId);
    QVERIFY(workflow.busy());

    TryxRuntimeOperationInfo staleStage;
    staleStage.id = QStringLiteral("stale-stage");
    staleStage.kind = QStringLiteral("StageDeviceMedia");
    staleStage.state = QStringLiteral("Succeeded");
    staleStage.resultName = artifactId;
    emit runtime.operationUpdated(staleStage);
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    TryxRuntimeOperationInfo staged = staleStage;
    staged.id = stageRequest.operationId;
    emit runtime.operationUpdated(staged);
    QCOMPARE(runtime.offlineRequests_.size(), 2);
    const RuntimeClient::OfflineRequest claimRequest =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        claimRequest.method,
        QStringLiteral("ClaimDeviceMediaArtifact"));
    QCOMPARE(
        claimRequest.arguments,
        QVariantList(
            {stageRequest.operationId, artifactId}));

    const TryxRuntimeDeviceMediaArtifact artifact =
        deviceMediaArtifact(
            stageRequest.operationId, artifactId, mediaId,
            mediaName, deviceIdentity);
    emit runtime.artifactClaimed(
        QStringLiteral("stale-stage"), artifact);
    QCOMPARE(runtime.offlineRequests_.size(), 2);
    QVERIFY(workflow.claimPending_);

    emit runtime.artifactClaimed(
        stageRequest.operationId, artifact);
    QVERIFY(editor.recoveredDeviceCopy());
    QVERIFY(!workflow.claimPending_);

    QVERIFY(QMetaObject::invokeMethod(
        &workflow.renewTimer_, "timeout",
        Qt::DirectConnection));
    QCOMPARE(runtime.offlineRequests_.size(), 3);
    QCOMPARE(
        runtime.offlineRequests_.constLast().method,
        QStringLiteral("RenewDeviceMediaArtifactLease"));
    QCOMPARE(
        runtime.offlineRequests_.constLast().arguments,
        QVariantList({artifactId, artifact.leaseId}));

    emit editor.recoveredSaveAsNewRequested(
        editor.preparationProfile());
    QCOMPARE(runtime.offlineRequests_.size(), 4);
    const RuntimeClient::OfflineRequest mutationRequest =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        mutationRequest.method,
        QStringLiteral(
            "QueueRecoveredMediaUploadWithTransform"));
    QCOMPARE(
        mutationRequest.arguments.at(1).toString(),
        artifactId);
    QCOMPARE(
        mutationRequest.arguments.at(2).toString(),
        artifact.leaseId);
    QVERIFY(editor.submissionPending());

    TryxRuntimeOperationInfo staleMutation;
    staleMutation.id = QStringLiteral("stale-mutation");
    staleMutation.kind =
        QStringLiteral("RecoveredMediaUpload");
    staleMutation.state = QStringLiteral("Succeeded");
    emit runtime.operationUpdated(staleMutation);
    QVERIFY(editor.submissionPending());
    QCOMPARE(runtime.offlineRequests_.size(), 4);

    TryxRuntimeOperationInfo completed = staleMutation;
    completed.id = mutationRequest.operationId;
    emit runtime.operationUpdated(completed);

    QCOMPARE(runtime.offlineRequests_.size(), 5);
    QCOMPARE(
        runtime.offlineRequests_.constLast().method,
        QStringLiteral("ReleaseDeviceMediaArtifact"));
    QCOMPARE(
        runtime.offlineRequests_.constLast().arguments,
        QVariantList({artifactId, artifact.leaseId}));
    QVERIFY(!workflow.busy());
    QVERIFY(!editor.submissionPending());
    QVERIFY(!editor.recoveredDeviceCopy());

    const int requestCount =
        runtime.offlineRequests_.size();
    emit runtime.operationUpdated(completed);
    emit editor.recoveredSaveAsNewRequested(
        editor.preparationProfile());
    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
}

void QuickClientTests::
    deviceMediaWorkflowInvalidationStopsReplaceMutations() {
    const QString mediaId = QStringLiteral("media-1");
    const QString mediaName =
        QStringLiteral("source.mp4.h264_2240x1080");
    const QString deviceIdentity =
        QStringLiteral("device-1");
    const QString artifactId =
        QStringLiteral("artifact-1");

    RuntimeClient runtime(true);
    preparePaseDeviceMedia(
        &runtime, mediaId, mediaName, deviceIdentity);
    MediaEditorController editor(&runtime);
    DeviceMediaWorkflowController workflow(
        &runtime, &editor);

    workflow.beginEdit(mediaId, mediaName);
    const QString stageOperationId =
        runtime.offlineRequests_.constFirst().operationId;

    TryxRuntimeOperationInfo staged;
    staged.id = stageOperationId;
    staged.kind = QStringLiteral("StageDeviceMedia");
    staged.state = QStringLiteral("Succeeded");
    staged.resultName = artifactId;
    emit runtime.operationUpdated(staged);

    const TryxRuntimeDeviceMediaArtifact artifact =
        deviceMediaArtifact(
            stageOperationId, artifactId, mediaId,
            mediaName, deviceIdentity);
    emit runtime.artifactClaimed(
        stageOperationId, artifact);
    QVERIFY(editor.recoveredDeviceCopy());

    emit editor.recoveredReplaceRequested(
        editor.preparationProfile());
    QCOMPARE(runtime.offlineRequests_.size(), 3);
    const RuntimeClient::OfflineRequest replaceRequest =
        runtime.offlineRequests_.constLast();
    QCOMPARE(
        replaceRequest.method,
        QStringLiteral("QueueReplaceDeviceMedia"));
    QVERIFY(editor.submissionPending());

    const quint64 serviceEpoch = runtime.serviceEpoch_;
    const int requestCount =
        runtime.offlineRequests_.size();
    runtime.onServiceUnregistered(
        tryxRuntimeServiceName());

    QCOMPARE(runtime.serviceEpoch_, serviceEpoch + 1);
    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
    QVERIFY(!workflow.busy());
    QVERIFY(workflow.error().contains(
        QStringLiteral("runtime"),
        Qt::CaseInsensitive));
    QVERIFY(!editor.submissionPending());
    QVERIFY(!editor.recoveredDeviceCopy());

    TryxRuntimeOperationInfo completed;
    completed.id = replaceRequest.operationId;
    completed.kind = QStringLiteral("ReplaceDeviceMedia");
    completed.state = QStringLiteral("Succeeded");
    completed.terminalOutcome = QStringLiteral("Replaced");
    emit runtime.operationUpdated(completed);
    emit runtime.artifactClaimed(
        stageOperationId, artifact);
    emit editor.recoveredReplaceRequested(
        editor.preparationProfile());
    QVERIFY(QMetaObject::invokeMethod(
        &workflow.renewTimer_, "timeout",
        Qt::DirectConnection));
    workflow.beginEdit(mediaId, mediaName);

    QCOMPARE(
        runtime.offlineRequests_.size(), requestCount);
}

void QuickClientTests::
    applyRequestPreservesConfirmedOverlaySettings() {
    RuntimeClient runtime(true);
    runtime.capabilitiesReady_ = true; // Negotiated Auto-only runtime.
    runtime.connection_.printerClassConnected = true;
    runtime.connection_.printerClassDevicePresent = true;
    runtime.connection_.serial = QStringLiteral("device-a");
    TryxRuntimeDisplayState state;
    state.revision = 1;
    state.deviceSerial = QStringLiteral("device-a");
    state.valid = true;
    state.settingsPosition = QStringLiteral("Bottom");
    state.settingsColor = QStringLiteral("#123456");
    state.settingsAlign = QStringLiteral("Center");
    state.settingsBadges = {
        QStringLiteral("CPU Badge")};
    state.settingsPosition2 = QStringLiteral("Top");
    state.settingsColor2 = QStringLiteral("#654321");
    state.settingsAlign2 = QStringLiteral("Right");
    state.settingsBadges2 = {
        QStringLiteral("GPU Badge")};
    runtime.applyDisplayState(state);

    QCOMPARE(runtime.displayLeftPosition(),
             state.settingsPosition);
    QCOMPARE(runtime.displayLeftColor(),
             state.settingsColor);
    QCOMPARE(runtime.displayLeftAlignment(),
             state.settingsAlign);
    QCOMPARE(runtime.displayRightPosition(),
             state.settingsPosition2);
    QCOMPARE(runtime.displayRightColor(),
             state.settingsColor2);
    QCOMPARE(runtime.displayRightAlignment(),
             state.settingsAlign2);

    state.revision = 2;
    state.settingsColor = QStringLiteral("red");
    state.settingsColor2 = QStringLiteral(" #445566");
    runtime.applyDisplayState(state);
    QCOMPARE(runtime.displayLeftColor(), QStringLiteral("red"));
    QCOMPARE(runtime.displayRightColor(),
             QStringLiteral(" #445566"));

    const TryxRuntimeApplyRequest full =
        runtime.fullScreenApplyRequest(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"),
            {QStringLiteral("CPU Temperature")},
            {QStringLiteral("GPU Badge")},
            QStringLiteral("Top"),
            QStringLiteral("#abcdef"),
            QStringLiteral("Right"));
    QCOMPARE(
        full.settingsBadges,
        QStringList{QStringLiteral("GPU Badge")});
    QCOMPARE(full.settingsPosition, QStringLiteral("Top"));
    QCOMPARE(full.settingsColor, QStringLiteral("#abcdef"));
    QCOMPARE(full.settingsAlign, QStringLiteral("Right"));

    const TryxRuntimeApplyRequest split =
        runtime.splitScreenApplyRequest(
            QStringLiteral("left.mp4.h264_2240x1080"),
            QStringLiteral("right.mp4.h264_2240x1080"),
            {QStringLiteral("CPU Temperature")},
            {QStringLiteral("GPU Temperature")},
            {QStringLiteral("GPU Badge")},
            {QStringLiteral("CPU Badge")},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Center"),
            QStringLiteral("Bottom"),
            QStringLiteral("#445566"),
            QStringLiteral("Right"));
    QCOMPARE(
        split.settingsBadges,
        QStringList{QStringLiteral("GPU Badge")});
    QCOMPARE(
        split.settingsBadges2,
        QStringList{QStringLiteral("CPU Badge")});
    QCOMPARE(split.settingsPosition, QStringLiteral("Top"));
    QCOMPARE(split.settingsColor, QStringLiteral("#112233"));
    QCOMPARE(split.settingsAlign, QStringLiteral("Center"));
    QCOMPARE(split.settingsPosition2, QStringLiteral("Bottom"));
    QCOMPARE(split.settingsColor2, QStringLiteral("#445566"));
    QCOMPARE(split.settingsAlign2, QStringLiteral("Right"));

    QString badgeError;
    QVERIFY(runtime.badgesSelectionValid(
        {QStringLiteral("CPU Badge"),
         QStringLiteral("GPU Badge")},
        &badgeError));
    QVERIFY(!runtime.badgesSelectionValid(
        {QStringLiteral("Unsupported Badge")},
        &badgeError));
    QVERIFY(!badgeError.isEmpty());
}

void QuickClientTests::
    applyRequestsUseExplicitValidatedOverlayStyles() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.connection_.printerClassDevicePresent = true;
    runtime.connection_.displaySessionActive = true;

    runtime.applyFullScreen(
        {QStringLiteral("full.mp4.h264_2240x1080")},
        QStringLiteral("Loop"), {}, {},
        QStringLiteral("Bottom"),
        QStringLiteral("#A1B2C3"),
        QStringLiteral("Center"));
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("QueueApplyWithMetrics"));
    QCOMPARE(runtime.offlineRequests_.constLast().kind,
             QStringLiteral("Apply"));
    const TryxRuntimeApplyRequest full =
        runtime.offlineRequests_.constLast()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    QCOMPARE(full.settingsPosition, QStringLiteral("Bottom"));
    QCOMPARE(full.settingsColor, QStringLiteral("#a1b2c3"));
    QCOMPARE(full.settingsAlign, QStringLiteral("Center"));

    const int acceptedCount = runtime.offlineRequests_.size();
    runtime.applyFullScreen(
        {QStringLiteral("full.mp4.h264_2240x1080")},
        QStringLiteral("Loop"), {}, {},
        QStringLiteral(" bottom"),
        QStringLiteral("#a1b2c3"),
        QStringLiteral("Center"));
    runtime.applyFullScreen(
        {QStringLiteral("full.mp4.h264_2240x1080")},
        QStringLiteral("Loop"), {}, {},
        QStringLiteral("Bottom"),
        QStringLiteral("#12345"),
        QStringLiteral("Center"));
    runtime.applyFullScreen(
        {QStringLiteral("full.mp4.h264_2240x1080")},
        QStringLiteral("Loop"), {}, {},
        QStringLiteral("Bottom"),
        QStringLiteral("#a1b2c3"),
        QStringLiteral("center"));
    QCOMPARE(runtime.offlineRequests_.size(), acceptedCount);
    QVERIFY(!runtime.diagnostic().isEmpty());

    runtime.applySplitScreen(
        QStringLiteral("left.mp4.h264_2240x1080"),
        QStringLiteral("right.mp4.h264_2240x1080"),
        QStringLiteral("Single"), {}, {}, {}, {},
        QStringLiteral("Top"),
        QStringLiteral("#112233"),
        QStringLiteral("Left"),
        QStringLiteral("Bottom"),
        QStringLiteral("#445566"),
        QStringLiteral("Right"));
    QCOMPARE(runtime.offlineRequests_.size(), acceptedCount + 1);
    QCOMPARE(runtime.offlineRequests_.constLast().method,
             QStringLiteral("QueueApplyWithMetrics"));
    QCOMPARE(runtime.offlineRequests_.constLast().kind,
             QStringLiteral("Apply"));
    const TryxRuntimeApplyRequest split =
        runtime.offlineRequests_.constLast()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    QCOMPARE(split.settingsPosition, QStringLiteral("Top"));
    QCOMPARE(split.settingsColor, QStringLiteral("#112233"));
    QCOMPARE(split.settingsAlign, QStringLiteral("Left"));
    QCOMPARE(split.settingsPosition2, QStringLiteral("Bottom"));
    QCOMPARE(split.settingsColor2, QStringLiteral("#445566"));
    QCOMPARE(split.settingsAlign2, QStringLiteral("Right"));

    RuntimeClient acceptedLegacyRuntime(true);
    acceptedLegacyRuntime.serviceAvailable_ = true;
    acceptedLegacyRuntime.compatible_ = true;
    acceptedLegacyRuntime.connection_.connected = true;
    acceptedLegacyRuntime.applySplitScreen(
        QStringLiteral("left.mp4"),
        QStringLiteral("right.mp4"),
        QStringLiteral("Single"), {}, {}, {}, {},
        QStringLiteral("Top"),
        QStringLiteral("#AABBCC"),
        QStringLiteral("Center"),
        QStringLiteral("Top"),
        QStringLiteral("#aabbcc"),
        QStringLiteral("Center"));
    QVERIFY(acceptedLegacyRuntime.pendingLegacyScreenConfigValid_);
    QCOMPARE(
        acceptedLegacyRuntime.pendingLegacyScreenConfig_.settingsColor,
        QStringLiteral("#aabbcc"));
    QCOMPARE(
        acceptedLegacyRuntime.pendingLegacyScreenConfig_.settingsColor2,
        QStringLiteral("#aabbcc"));
    QCOMPARE(
        acceptedLegacyRuntime.pendingLegacyScreenConfig_.settingsAlign,
        QStringLiteral("Center"));

    RuntimeClient legacyRuntime(true);
    legacyRuntime.serviceAvailable_ = true;
    legacyRuntime.compatible_ = true;
    legacyRuntime.connection_.connected = true;
    legacyRuntime.applySplitScreen(
        QStringLiteral("left.mp4"),
        QStringLiteral("right.mp4"),
        QStringLiteral("Single"), {}, {}, {}, {},
        QStringLiteral("Top"),
        QStringLiteral("#112233"),
        QStringLiteral("Left"),
        QStringLiteral("Bottom"),
        QStringLiteral("#445566"),
        QStringLiteral("Right"));
    QVERIFY(!legacyRuntime.pendingLegacyScreenConfigValid_);
    QVERIFY(!legacyRuntime.diagnostic().isEmpty());
}

void QuickClientTests::
    displayApplySubmissionIdentityIsSynchronousAndUnique() {
    RuntimeClient fullRuntime(true);
    fullRuntime.serviceAvailable_ = true;
    fullRuntime.compatible_ = true;
    fullRuntime.connection_.printerClassConnected = true;
    fullRuntime.connection_.printerClassDevicePresent = true;
    fullRuntime.connection_.displaySessionActive = true;
    prepareModernDisplay(&fullRuntime);
    QSignalSpy fullStarted(
        &fullRuntime, &RuntimeClient::displayApplyStarted);

    const QString fullId =
        fullRuntime.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#A1B2C3"),
            QStringLiteral("Center"),
            true, false, 0, false, false, false);

    QVERIFY(!fullId.isEmpty());
    QCOMPARE(fullStarted.count(), 1);
    QCOMPARE(
        fullStarted.constFirst().at(0).toString(), fullId);
    QCOMPARE(fullRuntime.offlineRequests_.size(), 1);
    QCOMPARE(
        fullRuntime.offlineRequests_.constFirst().operationId,
        fullId);
    QVERIFY(fullRuntime.operationBusy());

    const QString duplicateId =
        fullRuntime.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#A1B2C3"),
            QStringLiteral("Center"),
            true, false, 0, false, false, false);
    QVERIFY(duplicateId.isEmpty());
    QCOMPARE(fullStarted.count(), 1);
    QCOMPARE(fullRuntime.offlineRequests_.size(), 1);

    RuntimeClient splitRuntime(true);
    splitRuntime.serviceAvailable_ = true;
    splitRuntime.compatible_ = true;
    splitRuntime.connection_.printerClassConnected = true;
    splitRuntime.connection_.printerClassDevicePresent = true;
    splitRuntime.connection_.displaySessionActive = true;
    prepareModernDisplay(&splitRuntime);
    QSignalSpy splitStarted(
        &splitRuntime, &RuntimeClient::displayApplyStarted);

    const QString splitId =
        splitRuntime.submitSplitDisplayDraft(
            QStringLiteral("left.mp4.h264_2240x1080"),
            QStringLiteral("right.mp4.h264_2240x1080"),
            QStringLiteral("Single"), {}, {}, {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            QStringLiteral("Bottom"),
            QStringLiteral("#445566"),
            QStringLiteral("Right"),
            true, false, 0, false, false, false);

    QVERIFY(!splitId.isEmpty());
    QVERIFY(splitId != fullId);
    QCOMPARE(splitStarted.count(), 1);
    QCOMPARE(
        splitStarted.constFirst().at(0).toString(), splitId);
    QCOMPARE(splitRuntime.offlineRequests_.size(), 1);
    QCOMPARE(
        splitRuntime.offlineRequests_.constFirst().operationId,
        splitId);
}

void QuickClientTests::badgeDisplaySubmissionRequiresCoherentSnapshot_data() {
    QTest::addColumn<QString>("scenario");
    for (const char *name : {"snapshot-first", "operation-first", "legacy-state", "other-operation", "wrong-generation", "mismatch", "failed", "capability-loss", "replace-custom"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void QuickClientTests::badgeDisplaySubmissionRequiresCoherentSnapshot() {
    QFETCH(QString, scenario);
    RuntimeClient runtime(true);
    prepareModernDisplay(&runtime);
    runtime.connection_.connected = true;
    runtime.connection_.revision = 17;
    runtime.connection_.productId = QStringLiteral("391a:1021");
    runtime.capabilitiesReady_ = runtime.deviceCapabilitiesReady_ = true;
    runtime.runtimeCapabilities_ = {tryxRuntimeApplyWithBadgesV1Token(), tryxRuntimeDisplaySnapshotV1Token(), tryxRuntimeSavedLayoutsV2Token()};
    runtime.deviceCapabilities_ = {tryxDeviceOverlayBadgeTextV1Token()};
    runtime.deviceCapabilitiesSnapshot_.deviceIdentity = runtime.connection_.serial;
    runtime.deviceCapabilitiesSnapshot_.connectionRevision = 17;
    runtime.deviceCapabilitiesSnapshot_.physicalGeneration = 3;
    runtime.metricsCatalogReady_ = true;
    runtime.metricsCatalog_ = tryxMetricsCatalog();
    TryxRuntimeApplyRequest baseline;
    baseline.screenMode = QStringLiteral("Full Screen");
    baseline.playMode = QStringLiteral("Single");
    baseline.media = {QStringLiteral("full.mp4.h264_2240x1080")};
    baseline.settingsPosition = QStringLiteral("Top");
    baseline.settingsColor = QStringLiteral("#dcdcdc");
    baseline.settingsAlign = QStringLiteral("Left");
    TryxRuntimeDisplaySnapshotV1 initial;
    initial.revision = 41;
    initial.connectionRevision = 17;
    initial.physicalGeneration = 3;
    initial.productId = runtime.connection_.productId;
    initial.status = QStringLiteral("HostAccepted");
    initial.display = confirmedDisplayState(baseline, initial.revision);
    if (scenario == QStringLiteral("replace-custom")) {
        initial.display.settingsBadges = {QStringLiteral("CPU Badge")};
        initial.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Retain on Replace")};
    }
    QVERIFY(runtime.applyDisplaySnapshotV1(initial));
    QVERIFY(runtime.customBadgeTextSupported());
    if (scenario == QStringLiteral("replace-custom")) {
        MediaEditorController editor(&runtime);
        editor.beginRecoveredVideo(deviceMediaArtifact(QStringLiteral("stage"), QStringLiteral("artifact"),
            QStringLiteral("media"), baseline.media.first(), runtime.connection_.serial));
        QVERIFY(!editor.replaceAllowed());
        QVERIFY(editor.replaceBlockReason().contains(QStringLiteral("badge"), Qt::CaseInsensitive));
        runtime.offlineRequests_.clear();
        QVERIFY(runtime.queueReplaceDeviceMedia(QStringLiteral("artifact"), QStringLiteral("lease"), QStringLiteral("media"),
            runtime.currentDisplayApplyRequest(), tryxLegacyFitMediaTransform()).isEmpty());
        QVERIFY(runtime.offlineRequests_.isEmpty());
        return;
    }
    TryxRuntimeOverlayBadgesV1 badges;
    badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("  Exact text  ")};
    auto draft = tryxOverlayBadgesV1ToJson(badges).toVariantMap();
    QSignalSpy finished(&runtime, &RuntimeClient::displayApplyFinished);
    const auto submit = [&]() {
        return runtime.submitFullDisplayDraft(baseline.media, baseline.playMode, {}, {QStringLiteral("CPU Badge")},
            baseline.settingsPosition, baseline.settingsColor, baseline.settingsAlign,
            true, true, 80, false, false, false, draft);
    };
    if (scenario == QStringLiteral("capability-loss")) {
        runtime.deviceCapabilities_.clear();
        QVERIFY(submit().isEmpty());
        QVERIFY(runtime.offlineRequests_.isEmpty());
        return;
    }
    const QString id = submit();
    QVERIFY(!id.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    QCOMPARE(runtime.offlineRequests_.first().method, QStringLiteral("QueueApplyWithBadgesV1"));
    const auto sent = runtime.offlineRequests_.first().arguments.at(1).value<TryxRuntimeApplyWithBadgesV1>();
    QCOMPARE(sent.badges.primaryCpu.text, QStringLiteral("Exact text"));
    draft.clear();
    auto accepted = initial;
    accepted.revision = 43;
    accepted.display = confirmedDisplayState(sent.request, 43);
    accepted.badges = sent.badges;
    accepted.acceptedOperationId = id;
    TryxRuntimeOperationInfo operation;
    operation.id = id;
    operation.deviceGeneration = 3;
    operation.state = QStringLiteral("Succeeded");
    if (scenario == QStringLiteral("operation-first")) {
        runtime.observeDisplaySubmissionOperation(operation);
        QCOMPARE(finished.count(), 0);
    }
    TryxRuntimeDisplaySnapshotV1 pending;
    pending.status = QStringLiteral("Pending");
    pending.revision = 42;
    pending.connectionRevision = 17;
    pending.physicalGeneration = 3;
    pending.productId = initial.productId;
    QVERIFY(runtime.applyDisplaySnapshotV1(pending));
    QCOMPARE(finished.count(), 0);
    QVERIFY(!runtime.displayStateValid());
    if (scenario == QStringLiteral("legacy-state")) {
        runtime.applyDisplayState(accepted.display);
        QCOMPARE(finished.count(), 0);
        QVERIFY(!runtime.displayStateValid());
    }
    if (scenario == QStringLiteral("other-operation") || scenario == QStringLiteral("wrong-generation")) {
        auto stale = accepted;
        if (scenario == QStringLiteral("other-operation")) stale.acceptedOperationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        else ++stale.physicalGeneration;
        runtime.applyDisplaySnapshotV1(stale);
        QCOMPARE(finished.count(), 0);
        ++accepted.revision;
        accepted.display.revision = accepted.revision;
    }
    if (scenario == QStringLiteral("mismatch")) accepted.badges.primaryCpu.text = QStringLiteral("Different");
    if (scenario == QStringLiteral("failed")) {
        operation.state = QStringLiteral("Failed");
        operation.errorCategory = QStringLiteral("PersistenceFailed");
        runtime.observeDisplaySubmissionOperation(operation);
    }
    QVERIFY(runtime.applyDisplaySnapshotV1(accepted));
    if (scenario == QStringLiteral("snapshot-first")) QCOMPARE(finished.count(), 0);
    runtime.observeDisplaySubmissionOperation(operation);
    QCOMPARE(finished.count(), 1);
    if (scenario == QStringLiteral("mismatch") || scenario == QStringLiteral("failed"))
        QVERIFY(finished.first().at(1).toString() != QStringLiteral("Succeeded"));
    else {
        QCOMPARE(finished.first().at(1).toString(), QStringLiteral("Succeeded"));
        QCOMPARE(runtime.displayBadgeChoices().value(QStringLiteral("primaryCpu")).toMap().value(QStringLiteral("text")).toString(), QStringLiteral("Exact text"));
    }
    runtime.observeDisplaySubmissionOperation(operation);
    runtime.applyDisplaySnapshotV1(accepted);
    QCOMPARE(finished.count(), 1);
}

void QuickClientTests::
    modernDisplayApplyRequiresTerminalAndMatchingState() {
    RuntimeClient terminalFirst(true);
    terminalFirst.serviceAvailable_ = true;
    terminalFirst.compatible_ = true;
    terminalFirst.connection_.printerClassConnected = true;
    terminalFirst.connection_.printerClassDevicePresent = true;
    terminalFirst.connection_.displaySessionActive = true;
    prepareModernDisplay(&terminalFirst);
    QSignalSpy terminalFirstFinished(
        &terminalFirst,
        &RuntimeClient::displayApplyFinished);

    const QString terminalFirstId =
        terminalFirst.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#A1B2C3"),
            QStringLiteral("Center"),
            true, true, 67, true, true, true);
    QVERIFY(!terminalFirstId.isEmpty());
    QCOMPARE(terminalFirst.offlineRequests_.size(), 1);
    const TryxRuntimeApplyRequest terminalFirstRequest =
        terminalFirst.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();

    TryxRuntimeOperationInfo succeeded;
    succeeded.id = terminalFirstId;
    succeeded.kind = QStringLiteral("Apply");
    succeeded.state = QStringLiteral("Succeeded");
    emit terminalFirst.operationRequestAccepted(
        terminalFirstId, QStringLiteral("Apply"));
    QCOMPARE(terminalFirstFinished.count(), 0);
    terminalFirst.onOperationChanged(succeeded, 1);
    QCOMPARE(terminalFirstFinished.count(), 0);
    QVERIFY(terminalFirst.operationBusy());

    terminalFirst.applyDisplayState(
        confirmedDisplayState(terminalFirstRequest, 1));
    QCOMPARE(terminalFirstFinished.count(), 1);
    QCOMPARE(
        terminalFirstFinished.constFirst().at(0).toString(),
        terminalFirstId);
    QCOMPARE(
        terminalFirstFinished.constFirst().at(1).toString(),
        QStringLiteral("Succeeded"));
    QVERIFY(!terminalFirst.operationBusy());

    RuntimeClient stateFirst(true);
    stateFirst.serviceAvailable_ = true;
    stateFirst.compatible_ = true;
    stateFirst.connection_.printerClassConnected = true;
    stateFirst.connection_.printerClassDevicePresent = true;
    stateFirst.connection_.displaySessionActive = true;
    prepareModernDisplay(&stateFirst);
    QSignalSpy stateFirstFinished(
        &stateFirst, &RuntimeClient::displayApplyFinished);

    const QString stateFirstId =
        stateFirst.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#102030"),
            QStringLiteral("Right"),
            true, true, 42, true, false, true);
    QVERIFY(!stateFirstId.isEmpty());
    const TryxRuntimeApplyRequest stateFirstRequest =
        stateFirst.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();

    stateFirst.applyDisplayState(
        confirmedDisplayState(stateFirstRequest, 1));
    QCOMPARE(stateFirstFinished.count(), 0);
    QVERIFY(stateFirst.operationBusy());

    succeeded.id = stateFirstId;
    stateFirst.onOperationChanged(succeeded, 1);
    QCOMPARE(stateFirstFinished.count(), 1);
    QCOMPARE(
        stateFirstFinished.constFirst().at(0).toString(),
        stateFirstId);
    QCOMPARE(
        stateFirstFinished.constFirst().at(1).toString(),
        QStringLiteral("Succeeded"));
    QVERIFY(!stateFirst.operationBusy());

    RuntimeClient mismatchingReadback(true);
    mismatchingReadback.serviceAvailable_ = true;
    mismatchingReadback.compatible_ = true;
    mismatchingReadback.connection_.printerClassConnected = true;
    mismatchingReadback.connection_.printerClassDevicePresent = true;
    mismatchingReadback.connection_.displaySessionActive = true;
    prepareModernDisplay(&mismatchingReadback);
    QSignalSpy mismatchFinished(
        &mismatchingReadback,
        &RuntimeClient::displayApplyFinished);
    const QString mismatchId =
        mismatchingReadback.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#203040"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    const TryxRuntimeApplyRequest mismatchRequest =
        mismatchingReadback.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    succeeded.id = mismatchId;
    mismatchingReadback.onOperationChanged(succeeded, 1);
    TryxRuntimeDisplayState mismatch =
        confirmedDisplayState(mismatchRequest, 1);
    mismatch.media = {QStringLiteral("other.mp4")};
    mismatchingReadback.applyDisplayState(mismatch);
    for (const QList<QVariant> &result : mismatchFinished) {
        QVERIFY(result.at(1).toString() !=
                QStringLiteral("Succeeded"));
    }
}

void QuickClientTests::
    modernDisplayApplyRequiresExactDeviceIdentity() {
    RuntimeClient missingIdentity(true);
    missingIdentity.serviceAvailable_ = true;
    missingIdentity.compatible_ = true;
    missingIdentity.connection_.printerClassConnected = true;
    missingIdentity.connection_.printerClassDevicePresent = true;
    missingIdentity.connection_.displaySessionActive = true;
    missingIdentity.display_.valid = true;
    QSignalSpy missingStarted(
        &missingIdentity,
        &RuntimeClient::displayApplyStarted);
    const QString missingId =
        missingIdentity.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(missingId.isEmpty());
    QCOMPARE(missingStarted.count(), 0);
    QVERIFY(missingIdentity.offlineRequests_.isEmpty());

    RuntimeClient mismatchedIdentity(true);
    prepareModernDisplay(
        &mismatchedIdentity,
        QStringLiteral("display-a"));
    mismatchedIdentity.connection_.serial =
        QStringLiteral("display-b");
    const QString mismatchedId =
        mismatchedIdentity.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(mismatchedId.isEmpty());
    QVERIFY(mismatchedIdentity.offlineRequests_.isEmpty());

    RuntimeClient readbackMismatch(true);
    prepareModernDisplay(&readbackMismatch);
    QSignalSpy finished(
        &readbackMismatch,
        &RuntimeClient::displayApplyFinished);
    const QString submissionId =
        readbackMismatch.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(!submissionId.isEmpty());
    const TryxRuntimeApplyRequest request =
        readbackMismatch.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    TryxRuntimeOperationInfo succeeded;
    succeeded.id = submissionId;
    succeeded.kind = QStringLiteral("Apply");
    succeeded.state = QStringLiteral("Succeeded");
    readbackMismatch.onOperationChanged(succeeded, 1);
    TryxRuntimeDisplayState wrongDevice =
        confirmedDisplayState(request, 1);
    wrongDevice.deviceSerial = QStringLiteral("device-b");
    readbackMismatch.applyDisplayState(wrongDevice);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.constFirst().at(1).toString(),
             QStringLiteral("Unresolved"));
    QVERIFY(readbackMismatch.operationBusy());
}

void QuickClientTests::modernDisplayApplyFailuresNeverSucceed() {
    const QStringList terminalOutcomes = {
        QStringLiteral("Failed"),
        QStringLiteral("Cancelled"),
        QStringLiteral("RetryAvailable"),
    };
    for (const QString &outcome : terminalOutcomes) {
        RuntimeClient runtime(true);
        runtime.serviceAvailable_ = true;
        runtime.compatible_ = true;
        runtime.connection_.printerClassConnected = true;
        runtime.connection_.printerClassDevicePresent = true;
        runtime.connection_.displaySessionActive = true;
        prepareModernDisplay(&runtime);
        QSignalSpy finished(
            &runtime, &RuntimeClient::displayApplyFinished);

        const QString submissionId =
            runtime.submitFullDisplayDraft(
                {QStringLiteral("full.mp4.h264_2240x1080")},
                QStringLiteral("Loop"), {}, {},
                QStringLiteral("Bottom"),
                QStringLiteral("#A1B2C3"),
                QStringLiteral("Center"),
                true, false, 0, false, false, false);
        QVERIFY(!submissionId.isEmpty());
        const TryxRuntimeApplyRequest request =
            runtime.offlineRequests_.constFirst()
                .arguments.at(1)
                .value<TryxRuntimeApplyRequest>();

        TryxRuntimeOperationInfo terminal;
        terminal.id = submissionId;
        terminal.kind = QStringLiteral("Apply");
        terminal.state = outcome;
        terminal.message = QStringLiteral("terminal result");
        runtime.onOperationChanged(terminal, 1);

        QCOMPARE(finished.count(), 1);
        QCOMPARE(
            finished.constFirst().at(0).toString(),
            submissionId);
        QCOMPARE(
            finished.constFirst().at(1).toString(), outcome);
        QVERIFY(finished.constFirst().at(1).toString() !=
                QStringLiteral("Succeeded"));
        runtime.applyDisplayState(
            confirmedDisplayState(request, 1));
        QCOMPARE(finished.count(), 1);
        QCOMPARE(runtime.offlineRequests_.size(), 1);
    }

    RuntimeClient partialOrUnknown(true);
    partialOrUnknown.serviceAvailable_ = true;
    partialOrUnknown.compatible_ = true;
    partialOrUnknown.connection_.printerClassConnected = true;
    partialOrUnknown.connection_.printerClassDevicePresent = true;
    partialOrUnknown.connection_.displaySessionActive = true;
    prepareModernDisplay(&partialOrUnknown);
    QSignalSpy partialFinished(
        &partialOrUnknown,
        &RuntimeClient::displayApplyFinished);
    const QString partialId =
        partialOrUnknown.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#556677"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(!partialId.isEmpty());
    TryxRuntimeOperationInfo partialTerminal;
    partialTerminal.id = partialId;
    partialTerminal.kind = QStringLiteral("Apply");
    // This is the actual non-Replace producer shape: an uncertain USB
    // mutation finishes Failed with PartialOrUnknown as its error category.
    partialTerminal.state = QStringLiteral("Failed");
    partialTerminal.errorCategory =
        QStringLiteral("PartialOrUnknown");
    partialTerminal.message =
        QStringLiteral("mutation outcome is unknown");
    partialOrUnknown.onOperationChanged(partialTerminal, 1);
    QCOMPARE(partialFinished.count(), 1);
    QCOMPARE(
        partialFinished.constFirst().at(1).toString(),
        QStringLiteral("Unresolved"));
    QVERIFY(partialOrUnknown.operationBusy());
    const QString blockedId =
        partialOrUnknown.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#556677"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(blockedId.isEmpty());
    QCOMPARE(partialOrUnknown.offlineRequests_.size(), 1);
    partialOrUnknown.abandonDisplaySubmission(partialId);
    QVERIFY(!partialOrUnknown.operationBusy());

    RuntimeClient sessionLost(true);
    sessionLost.serviceAvailable_ = true;
    sessionLost.compatible_ = true;
    sessionLost.connection_.printerClassConnected = true;
    sessionLost.connection_.printerClassDevicePresent = true;
    sessionLost.connection_.displaySessionActive = true;
    prepareModernDisplay(&sessionLost);
    QSignalSpy sessionLostFinished(
        &sessionLost, &RuntimeClient::displayApplyFinished);
    const QString sessionLostId =
        sessionLost.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#778899"),
            QStringLiteral("Center"),
            true, false, 0, false, false, false);
    QVERIFY(!sessionLostId.isEmpty());
    sessionLost.onDisplaySessionChanged(false, 1);
    QCOMPARE(sessionLostFinished.count(), 1);
    QCOMPARE(
        sessionLostFinished.constFirst().at(0).toString(),
        sessionLostId);
    QCOMPARE(
        sessionLostFinished.constFirst().at(1).toString(),
        QStringLiteral("Unresolved"));
    QVERIFY(sessionLost.operationBusy());

    RuntimeClient rejected(true);
    rejected.serviceAvailable_ = true;
    rejected.compatible_ = true;
    rejected.connection_.printerClassConnected = true;
    rejected.connection_.printerClassDevicePresent = true;
    rejected.connection_.displaySessionActive = true;
    prepareModernDisplay(&rejected);
    QSignalSpy rejectedFinished(
        &rejected, &RuntimeClient::displayApplyFinished);
    const QString rejectedId =
        rejected.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    emit rejected.operationRequestRejected(
        rejectedId, QStringLiteral("Apply"),
        QStringLiteral("request rejected"));
    QCOMPARE(rejectedFinished.count(), 1);
    QCOMPARE(
        rejectedFinished.constFirst().at(1).toString(),
        QStringLiteral("Rejected"));
    QCOMPARE(rejected.offlineRequests_.size(), 1);

    RuntimeClient invalidated(true);
    invalidated.serviceAvailable_ = true;
    invalidated.compatible_ = true;
    invalidated.connection_.printerClassConnected = true;
    invalidated.connection_.printerClassDevicePresent = true;
    invalidated.connection_.displaySessionActive = true;
    prepareModernDisplay(&invalidated);
    QSignalSpy invalidatedFinished(
        &invalidated, &RuntimeClient::displayApplyFinished);
    const QString invalidatedId =
        invalidated.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#334455"),
            QStringLiteral("Center"),
            true, false, 0, false, false, false);
    QVERIFY(!invalidatedId.isEmpty());
    invalidated.onServiceUnregistered(
        QStringLiteral("org.tryx.Runtime"));
    QCOMPARE(invalidatedFinished.count(), 1);
    QCOMPARE(
        invalidatedFinished.constFirst().at(0).toString(),
        invalidatedId);
    QCOMPARE(
        invalidatedFinished.constFirst().at(1).toString(),
        QStringLiteral("Unresolved"));
    QCOMPARE(invalidated.offlineRequests_.size(), 1);
}

void QuickClientTests::
    modernControlOnlyDisplayApplyPreservesLayoutAndOverlay() {
    RuntimeClient fullRuntime(true);
    fullRuntime.serviceAvailable_ = true;
    fullRuntime.compatible_ = true;
    fullRuntime.connection_.printerClassConnected = true;
    fullRuntime.connection_.printerClassDevicePresent = true;
    fullRuntime.connection_.displaySessionActive = true;
    prepareModernDisplay(&fullRuntime);
    fullRuntime.display_.settingsBadges = {
        QStringLiteral("CPU Badge")};
    fullRuntime.display_.sysinfoLabels = {
        QStringLiteral("CPU Temperature")};

    const QString fullId =
        fullRuntime.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            false, true, 64, true, true, false);
    QVERIFY(!fullId.isEmpty());
    QCOMPARE(fullRuntime.offlineRequests_.size(), 1);
    const TryxRuntimeApplyRequest fullRequest =
        fullRuntime.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    QVERIFY(fullRequest.media.isEmpty());
    QVERIFY(fullRequest.sysinfoLabels.isEmpty());
    QVERIFY(fullRequest.sysinfoLabels2.isEmpty());
    QVERIFY(fullRequest.settingsBadges.isEmpty());
    QVERIFY(fullRequest.settingsBadges2.isEmpty());
    QVERIFY(!fullRequest.replaceOverlay);
    QVERIFY(fullRequest.display.brightnessPresent);
    QCOMPARE(fullRequest.display.brightness, 64);
    QVERIFY(fullRequest.display.orientationPresent);
    QVERIFY(fullRequest.display.mirrorMode);

    RuntimeClient splitRuntime(true);
    splitRuntime.serviceAvailable_ = true;
    splitRuntime.compatible_ = true;
    splitRuntime.connection_.printerClassConnected = true;
    splitRuntime.connection_.printerClassDevicePresent = true;
    splitRuntime.connection_.displaySessionActive = true;
    prepareModernDisplay(&splitRuntime);
    splitRuntime.display_.settingsBadges = {
        QStringLiteral("CPU Badge")};
    splitRuntime.display_.settingsBadges2 = {
        QStringLiteral("GPU Badge")};

    const QString splitId =
        splitRuntime.submitSplitDisplayDraft(
            {}, {}, QStringLiteral("Single"),
            {}, {}, {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            QStringLiteral("Bottom"),
            QStringLiteral("#445566"),
            QStringLiteral("Right"),
            false, true, 41, false, false, false);
    QVERIFY(!splitId.isEmpty());
    QCOMPARE(splitRuntime.offlineRequests_.size(), 1);
    const TryxRuntimeApplyRequest splitRequest =
        splitRuntime.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    QVERIFY(splitRequest.media.isEmpty());
    QVERIFY(splitRequest.sysinfoLabels.isEmpty());
    QVERIFY(splitRequest.sysinfoLabels2.isEmpty());
    QVERIFY(splitRequest.settingsBadges.isEmpty());
    QVERIFY(splitRequest.settingsBadges2.isEmpty());
    QVERIFY(!splitRequest.replaceOverlay);
    QVERIFY(splitRequest.display.brightnessPresent);
    QCOMPARE(splitRequest.display.brightness, 41);
}

void QuickClientTests::
    modernDisplayApplyAmbiguousAcknowledgementStaysUnresolved() {
    RuntimeClient unknown(true);
    unknown.serviceAvailable_ = true;
    unknown.compatible_ = true;
    unknown.connection_.printerClassConnected = true;
    unknown.connection_.printerClassDevicePresent = true;
    unknown.connection_.displaySessionActive = true;
    prepareModernDisplay(&unknown);
    QSignalSpy unknownFinished(
        &unknown, &RuntimeClient::displayApplyFinished);

    const QString unknownId =
        unknown.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(!unknownId.isEmpty());
    unknown.reportOperationRequestFailure(
        unknownId, QStringLiteral("Apply"),
        QStringLiteral("The acknowledgement outcome is unknown"),
        true);
    QCOMPARE(unknownFinished.count(), 1);
    QCOMPARE(unknownFinished.constFirst().at(1).toString(),
             QStringLiteral("Unresolved"));
    QVERIFY(unknown.operationBusy());
    QVERIFY(unknown.displaySubmission_.unresolved);

    const QString duplicate =
        unknown.submitFullDisplayDraft(
            {QStringLiteral("other.mp4.h264_2240x1080")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#445566"),
            QStringLiteral("Center"),
            true, false, 0, false, false, false);
    QVERIFY(duplicate.isEmpty());
    QCOMPARE(unknown.offlineRequests_.size(), 1);

    RuntimeClient definite(true);
    definite.serviceAvailable_ = true;
    definite.compatible_ = true;
    definite.connection_.printerClassConnected = true;
    definite.connection_.printerClassDevicePresent = true;
    definite.connection_.displaySessionActive = true;
    prepareModernDisplay(&definite);
    QSignalSpy definiteFinished(
        &definite, &RuntimeClient::displayApplyFinished);
    const QString definiteId =
        definite.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(!definiteId.isEmpty());
    definite.reportOperationRequestFailure(
        definiteId, QStringLiteral("Apply"),
        QStringLiteral("The runtime rejected the request"),
        false);
    QCOMPARE(definiteFinished.count(), 1);
    QCOMPARE(definiteFinished.constFirst().at(1).toString(),
             QStringLiteral("Rejected"));
    QVERIFY(!definite.operationBusy());
}

void QuickClientTests::
    displayApplyTimeoutStaysUnresolvedUntilAbandoned() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;
    runtime.connection_.printerClassConnected = true;
    runtime.connection_.printerClassDevicePresent = true;
    runtime.connection_.displaySessionActive = true;
    prepareModernDisplay(&runtime);
    QSignalSpy finished(
        &runtime, &RuntimeClient::displayApplyFinished);

    const QString submissionId =
        runtime.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(!submissionId.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    runtime.onDisplayApplyTimeout();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.constFirst().at(0).toString(),
             submissionId);
    QCOMPARE(finished.constFirst().at(1).toString(),
             QStringLiteral("Unresolved"));
    QVERIFY(runtime.operationBusy());

    runtime.onDisplayApplyTimeout();
    QCOMPARE(finished.count(), 1);
    const QString blockedId =
        runtime.submitFullDisplayDraft(
            {QStringLiteral("other.mp4.h264_2240x1080")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#445566"),
            QStringLiteral("Center"),
            true, false, 0, false, false, false);
    QVERIFY(blockedId.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    runtime.abandonDisplaySubmission(
        QStringLiteral("different-submission"));
    QVERIFY(runtime.operationBusy());
    runtime.abandonDisplaySubmission(submissionId);
    QVERIFY(!runtime.operationBusy());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
}

void QuickClientTests::
    legacyDisplayApplyUsesOneMatchingManager1Mutation() {
    RuntimeClient layoutRuntime(true);
    layoutRuntime.serviceAvailable_ = true;
    layoutRuntime.compatible_ = true;
    layoutRuntime.connection_.connected = true;
    layoutRuntime.connection_.serial =
        QStringLiteral("legacy-a");
    layoutRuntime.connection_.revision = 5;
    QSignalSpy layoutStarted(
        &layoutRuntime, &RuntimeClient::displayApplyStarted);
    QSignalSpy layoutFinished(
        &layoutRuntime, &RuntimeClient::displayApplyFinished);

    const QString layoutId =
        layoutRuntime.submitFullDisplayDraft(
            {QStringLiteral("full.mp4")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#A1B2C3"),
            QStringLiteral("Center"),
            true, false, 0, true, false, true);
    QVERIFY(!layoutId.isEmpty());
    QCOMPARE(layoutStarted.count(), 1);
    QCOMPARE(layoutRuntime.offlineRequests_.size(), 1);
    QCOMPARE(
        layoutRuntime.offlineRequests_.constFirst().method,
        QStringLiteral("SetScreenConfig"));
    QCOMPARE(
        layoutRuntime.offlineRequests_.constFirst().operationId,
        layoutId);
    QCOMPARE(
        layoutRuntime.offlineRequests_.constFirst()
            .arguments.at(13).toBool(),
        true);
    QVERIFY(layoutRuntime.pendingLegacyScreenConfigValid_);

    layoutRuntime.displaySubmission_
        .legacyRequestAcknowledged = false;
    layoutRuntime.onLegacyScreenConfigChanged(5);
    QVERIFY(layoutRuntime.pendingLegacyScreenConfigValid_);
    QCOMPARE(layoutFinished.count(), 0);
    layoutRuntime.onLegacyScreenConfigChanged(6);
    QVERIFY(layoutRuntime.displaySubmission_
                .legacyCandidateObserved);
    QCOMPARE(layoutFinished.count(), 0);
    // GetSnapshot may advance the pre-dispatch barrier after an unrelated
    // early signal was buffered. Acknowledgement must discard that stale
    // candidate without clearing the pending request.
    layoutRuntime.displaySubmission_
        .startingDisplayRevision = 10;
    layoutRuntime.acknowledgeLegacyDisplaySubmission(
        layoutId);
    QVERIFY(!layoutRuntime.displaySubmission_
                 .legacyCandidateObserved);
    QVERIFY(layoutRuntime.pendingLegacyScreenConfigValid_);
    QCOMPARE(layoutFinished.count(), 0);
    layoutRuntime.onLegacyScreenConfigChanged(10);
    QCOMPARE(layoutFinished.count(), 0);
    layoutRuntime.onLegacyScreenConfigChanged(11);
    QCOMPARE(layoutFinished.count(), 1);
    QCOMPARE(
        layoutFinished.constFirst().at(0).toString(),
        layoutId);
    QCOMPARE(
        layoutFinished.constFirst().at(1).toString(),
        QStringLiteral("Succeeded"));

    RuntimeClient brightnessRuntime(true);
    brightnessRuntime.serviceAvailable_ = true;
    brightnessRuntime.compatible_ = true;
    brightnessRuntime.connection_.connected = true;
    brightnessRuntime.connection_.serial =
        QStringLiteral("legacy-a");
    brightnessRuntime.connection_.revision = 10;
    QSignalSpy brightnessFinished(
        &brightnessRuntime,
        &RuntimeClient::displayApplyFinished);

    const QString brightnessId =
        brightnessRuntime.submitFullDisplayDraft(
            {QStringLiteral("full.mp4")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            false, true, 63, false, false, false);
    QVERIFY(!brightnessId.isEmpty());
    QCOMPARE(brightnessRuntime.offlineRequests_.size(), 1);
    QCOMPARE(
        brightnessRuntime.offlineRequests_.constFirst().method,
        QStringLiteral("SetBrightness"));
    QCOMPARE(
        brightnessRuntime.offlineRequests_.constFirst()
            .arguments,
        QVariantList{63});
    QVERIFY(!brightnessRuntime.pendingLegacyScreenConfigValid_);

    brightnessRuntime.onLegacyBrightnessChanged(63, 10);
    QCOMPARE(brightnessFinished.count(), 0);
    brightnessRuntime.onLegacyBrightnessChanged(63, 11);
    QCOMPARE(brightnessFinished.count(), 1);
    QCOMPARE(
        brightnessFinished.constFirst().at(0).toString(),
        brightnessId);
    QCOMPARE(
        brightnessFinished.constFirst().at(1).toString(),
        QStringLiteral("Succeeded"));

    RuntimeClient mismatchedBrightness(true);
    mismatchedBrightness.serviceAvailable_ = true;
    mismatchedBrightness.compatible_ = true;
    mismatchedBrightness.connection_.connected = true;
    mismatchedBrightness.connection_.serial =
        QStringLiteral("legacy-a");
    mismatchedBrightness.connection_.revision = 20;
    QSignalSpy mismatchFinished(
        &mismatchedBrightness,
        &RuntimeClient::displayApplyFinished);
    const QString mismatchId =
        mismatchedBrightness.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            false, true, 63, false, false, false);
    QVERIFY(!mismatchId.isEmpty());
    mismatchedBrightness.onLegacyBrightnessChanged(62, 21);
    QCOMPARE(mismatchFinished.count(), 1);
    QCOMPARE(mismatchFinished.constFirst().at(1).toString(),
             QStringLiteral("Unresolved"));
    QVERIFY(mismatchedBrightness.operationBusy());
}

void QuickClientTests::
    legacyDisplayStateIsBoundToExactDeviceIdentity() {
    RuntimeClient runtime(true);
    runtime.serviceAvailable_ = true;
    runtime.compatible_ = true;

    TryxRuntimeSnapshot deviceA;
    deviceA.connected = true;
    deviceA.serial = QStringLiteral("legacy-a");
    deviceA.productId = QStringLiteral("391a:1011");
    deviceA.revision = 1;
    runtime.applyConnectionSnapshot(deviceA);
    QCOMPARE(runtime.displayDeviceIdentity(),
             QStringLiteral("legacy:legacy-a"));

    const QString layoutA =
        runtime.submitFullDisplayDraft(
            {QStringLiteral("device-a.mp4")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#A1B2C3"),
            QStringLiteral("Center"),
            true, false, 0, true, false, true);
    QVERIFY(!layoutA.isEmpty());
    runtime.onLegacyScreenConfigChanged(2);
    QVERIFY(runtime.displayStateValid());
    QVERIFY(runtime.legacyLayoutConfirmed_);
    QCOMPARE(runtime.legacyDisplayStateIdentity_,
             QStringLiteral("legacy:legacy-a"));
    QCOMPARE(runtime.displayedMedia(),
             QStringList{QStringLiteral("device-a.mp4")});

    TryxRuntimeSnapshot disconnected = deviceA;
    disconnected.connected = false;
    disconnected.serial.clear();
    disconnected.productId.clear();
    disconnected.revision = 3;
    runtime.applyConnectionSnapshot(disconnected);
    QVERIFY(!runtime.displayStateValid());
    QCOMPARE(runtime.legacyDisplayStateIdentity_,
             QStringLiteral("legacy:legacy-a"));
    QCOMPARE(runtime.displayedMedia(),
             QStringList{QStringLiteral("device-a.mp4")});

    TryxRuntimeSnapshot reconnectedA = deviceA;
    reconnectedA.revision = 4;
    runtime.applyConnectionSnapshot(reconnectedA);
    QVERIFY(!runtime.displayStateValid());
    QVERIFY(!runtime.legacyLayoutConfirmed_);
    QCOMPARE(runtime.legacyDisplayStateIdentity_,
             QStringLiteral("legacy:legacy-a"));
    QCOMPARE(runtime.displayedMedia(),
             QStringList{QStringLiteral("device-a.mp4")});
    const QString reconnectWaterfall =
        runtime.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            false, false, 0, true, false, false);
    QVERIFY(reconnectWaterfall.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    TryxRuntimeSnapshot deviceB = deviceA;
    deviceB.serial = QStringLiteral("legacy-b");
    deviceB.revision = 5;
    runtime.applyConnectionSnapshot(deviceB);
    QCOMPARE(runtime.displayDeviceIdentity(),
             QStringLiteral("legacy:legacy-b"));
    QVERIFY(!runtime.displayStateValid());
    QVERIFY(!runtime.legacyLayoutConfirmed_);
    QVERIFY(runtime.legacyDisplayStateIdentity_.isEmpty());
    QVERIFY(runtime.displayedMedia().isEmpty());

    const QString unsafeWaterfall =
        runtime.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            false, false, 0, true, false, true);
    QVERIFY(unsafeWaterfall.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);

    const QString brightnessB =
        runtime.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            false, true, 61, false, false, false);
    QVERIFY(!brightnessB.isEmpty());
    runtime.onLegacyBrightnessChanged(61, 6);
    QVERIFY(!runtime.displayStateValid());
    QVERIFY(!runtime.legacyLayoutConfirmed_);
    QCOMPARE(runtime.brightness(), 61);

    const QString stillUnsafeWaterfall =
        runtime.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            false, false, 0, true, false, true);
    QVERIFY(stillUnsafeWaterfall.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 2);

    const QString layoutB =
        runtime.submitFullDisplayDraft(
            {QStringLiteral("device-b.mp4")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(!layoutB.isEmpty());
    runtime.onLegacyScreenConfigChanged(7);
    QVERIFY(runtime.displayStateValid());
    QCOMPARE(runtime.legacyDisplayStateIdentity_,
             QStringLiteral("legacy:legacy-b"));
    QCOMPARE(runtime.displayedMedia(),
             QStringList{QStringLiteral("device-b.mp4")});

    TryxRuntimeSnapshot unidentified = deviceB;
    unidentified.serial.clear();
    unidentified.revision = 8;
    runtime.applyConnectionSnapshot(unidentified);
    QVERIFY(runtime.displayDeviceIdentity().isEmpty());
    QVERIFY(!runtime.displayStateValid());
    QCOMPARE(runtime.legacyDisplayStateIdentity_,
             QStringLiteral("legacy:legacy-b"));
    QCOMPARE(runtime.displayedMedia(),
             QStringList{QStringLiteral("device-b.mp4")});
    const qsizetype dispatches = runtime.offlineRequests_.size();
    const QString unidentifiedLayout =
        runtime.submitFullDisplayDraft(
            {QStringLiteral("unknown.mp4")},
            QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);
    QVERIFY(unidentifiedLayout.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), dispatches);

    RuntimeClient restarted(true);
    restarted.serviceAvailable_ = true;
    restarted.compatible_ = true;
    restarted.applyConnectionSnapshot(deviceA);
    const QString beforeRestart =
        restarted.submitFullDisplayDraft(
            {QStringLiteral("restart-safe.mp4")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#123456"),
            QStringLiteral("Center"),
            true, false, 0, true, false, true);
    QVERIFY(!beforeRestart.isEmpty());
    restarted.onLegacyScreenConfigChanged(2);
    QVERIFY(restarted.displayStateValid());

    restarted.clearRuntimeState();
    QVERIFY(!restarted.displayStateValid());
    QCOMPARE(restarted.legacyDisplayStateIdentity_,
             QStringLiteral("legacy:legacy-a"));
    QCOMPARE(restarted.displayedMedia(),
             QStringList{QStringLiteral("restart-safe.mp4")});
    restarted.serviceAvailable_ = true;
    restarted.compatible_ = true;
    restarted.applyConnectionSnapshot(deviceA);
    QVERIFY(!restarted.displayStateValid());
    QVERIFY(!restarted.legacyLayoutConfirmed_);
    QCOMPARE(restarted.displayedMedia(),
             QStringList{QStringLiteral("restart-safe.mp4")});
    const QString afterRestartWaterfall =
        restarted.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            false, false, 0, true, false, false);
    QVERIFY(afterRestartWaterfall.isEmpty());
    QCOMPARE(restarted.offlineRequests_.size(), 1);

    RuntimeClient unresolved(true);
    unresolved.serviceAvailable_ = true;
    unresolved.compatible_ = true;
    unresolved.applyConnectionSnapshot(deviceA);
    const QString unresolvedId =
        unresolved.submitFullDisplayDraft(
            {QStringLiteral("device-a-pending.mp4")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#ABCDEF"),
            QStringLiteral("Center"),
            true, false, 0, true, false, true);
    QVERIFY(!unresolvedId.isEmpty());
    QVERIFY(unresolved.pendingLegacyScreenConfigValid_);
    unresolved.onDisplayApplyTimeout();
    QVERIFY(unresolved.displaySubmission_.unresolved);
    QVERIFY(!unresolved.pendingLegacyScreenConfigValid_);
    QVERIFY(!unresolved.displaySubmission_
                 .legacyCandidateObserved);

    TryxRuntimeSnapshot replacement = deviceA;
    replacement.serial = QStringLiteral("legacy-b");
    replacement.revision = 2;
    unresolved.applyConnectionSnapshot(replacement);
    unresolved.onLegacyScreenConfigChanged(3);
    QVERIFY(!unresolved.displayStateValid());
    QVERIFY(unresolved.displayedMedia().isEmpty());
    unresolved.abandonDisplaySubmission(unresolvedId);
    QVERIFY(!unresolved.operationBusy());
    const QString poisonedWaterfall =
        unresolved.submitFullDisplayDraft(
            {}, QStringLiteral("Single"), {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#DCDCDC"),
            QStringLiteral("Left"),
            false, false, 0, true, false, true);
    QVERIFY(poisonedWaterfall.isEmpty());
    QCOMPARE(unresolved.offlineRequests_.size(), 1);
}

void QuickClientTests::
    legacyDisplayPreflightClassifiesBeforeDispatchFailure() {
    RuntimeClient rejected(true);
    rejected.serviceAvailable_ = true;
    rejected.compatible_ = true;
    rejected.connection_.connected = true;
    rejected.connection_.serial = QStringLiteral("legacy-a");
    rejected.displaySubmission_.id =
        QStringLiteral("legacy-preflight-rejected");
    rejected.displaySubmission_.legacyScreenConfig = true;
    rejected.displaySubmission_.deviceIdentity =
        QStringLiteral("legacy:legacy-a");
    rejected.pendingLegacyScreenConfigValid_ = true;
    QSignalSpy rejectedFinished(
        &rejected, &RuntimeClient::displayApplyFinished);

    rejected.finishLegacyDisplayPreflightFailure(
        rejected.displaySubmission_.id,
        QStringLiteral("The read-only preflight failed"),
        false);
    QCOMPARE(rejectedFinished.count(), 1);
    QCOMPARE(rejectedFinished.constFirst().at(1).toString(),
             QStringLiteral("Rejected"));
    QVERIFY(!rejected.operationBusy());
    QVERIFY(!rejected.pendingLegacyScreenConfigValid_);
    QVERIFY(rejected.offlineRequests_.isEmpty());

    RuntimeClient invalidated(true);
    invalidated.serviceAvailable_ = true;
    invalidated.compatible_ = true;
    invalidated.connection_.connected = true;
    invalidated.connection_.serial = QStringLiteral("legacy-a");
    invalidated.displaySubmission_.id =
        QStringLiteral("legacy-preflight-invalidated");
    invalidated.displaySubmission_.legacyScreenConfig = true;
    invalidated.displaySubmission_.deviceIdentity =
        QStringLiteral("legacy:legacy-a");
    invalidated.pendingLegacyScreenConfigValid_ = true;
    QSignalSpy invalidatedFinished(
        &invalidated, &RuntimeClient::displayApplyFinished);

    invalidated.finishLegacyDisplayPreflightFailure(
        invalidated.displaySubmission_.id,
        QStringLiteral("The runtime epoch changed"),
        true);
    QCOMPARE(invalidatedFinished.count(), 1);
    QCOMPARE(invalidatedFinished.constFirst().at(1).toString(),
             QStringLiteral("Unresolved"));
    QVERIFY(invalidated.operationBusy());
    QVERIFY(invalidated.displaySubmission_.unresolved);
    QVERIFY(!invalidated.pendingLegacyScreenConfigValid_);
    QVERIFY(invalidated.offlineRequests_.isEmpty());
}

void QuickClientTests::
    legacyDisplayApplyRejectsUnsupportedComposites() {
    RuntimeClient layoutAndBrightness(true);
    layoutAndBrightness.serviceAvailable_ = true;
    layoutAndBrightness.compatible_ = true;
    layoutAndBrightness.connection_.connected = true;
    QSignalSpy compositeStarted(
        &layoutAndBrightness,
        &RuntimeClient::displayApplyStarted);
    QSignalSpy compositeFinished(
        &layoutAndBrightness,
        &RuntimeClient::displayApplyFinished);

    const QString compositeId =
        layoutAndBrightness.submitFullDisplayDraft(
            {QStringLiteral("full.mp4")},
            QStringLiteral("Loop"), {}, {},
            QStringLiteral("Bottom"),
            QStringLiteral("#A1B2C3"),
            QStringLiteral("Center"),
            true, true, 50, false, false, false);
    QVERIFY(compositeId.isEmpty());
    QCOMPARE(compositeStarted.count(), 0);
    QCOMPARE(compositeFinished.count(), 0);
    QVERIFY(layoutAndBrightness.offlineRequests_.isEmpty());
    QVERIFY(!layoutAndBrightness.pendingLegacyScreenConfigValid_);
    QVERIFY(!layoutAndBrightness.diagnostic().isEmpty());

    RuntimeClient mirrorRuntime(true);
    mirrorRuntime.serviceAvailable_ = true;
    mirrorRuntime.compatible_ = true;
    mirrorRuntime.connection_.connected = true;
    QSignalSpy mirrorStarted(
        &mirrorRuntime, &RuntimeClient::displayApplyStarted);
    QSignalSpy mirrorFinished(
        &mirrorRuntime, &RuntimeClient::displayApplyFinished);

    const QString mirrorId =
        mirrorRuntime.submitSplitDisplayDraft(
            QStringLiteral("left.mp4"),
            QStringLiteral("right.mp4"),
            QStringLiteral("Single"), {}, {}, {}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, true, true, false);
    QVERIFY(mirrorId.isEmpty());
    QCOMPARE(mirrorStarted.count(), 0);
    QCOMPARE(mirrorFinished.count(), 0);
    QVERIFY(mirrorRuntime.offlineRequests_.isEmpty());
    QVERIFY(!mirrorRuntime.pendingLegacyScreenConfigValid_);
    QVERIFY(!mirrorRuntime.diagnostic().isEmpty());
}

void QuickClientTests::
    unavailableConfirmedMetricCanBeAppliedInSameArea() {
    RuntimeClient runtime(true);
    prepareModernDisplay(&runtime);
    runtime.display_.revision = 7;
    runtime.display_.screenMode = QStringLiteral("Full Screen");
    runtime.display_.sysinfoLabels = {
        QStringLiteral("CPU Temperature")};
    runtime.metrics_.revision = 4;
    runtime.metrics_.availableMetrics = {
        QStringLiteral("GPU Temperature")};
    runtime.metricsCatalogReady_ = true;
    runtime.metricsCatalog_ = tryxMetricsCatalog();

    const QString submissionId =
        runtime.submitFullDisplayDraft(
            {QStringLiteral("full.mp4.h264_2240x1080")},
            QStringLiteral("Single"),
            {QStringLiteral("CPU Temperature")}, {},
            QStringLiteral("Top"),
            QStringLiteral("#112233"),
            QStringLiteral("Left"),
            true, false, 0, false, false, false);

    QVERIFY(!submissionId.isEmpty());
    QCOMPARE(runtime.offlineRequests_.size(), 1);
    const TryxRuntimeApplyRequest request =
        runtime.offlineRequests_.constFirst()
            .arguments.at(1)
            .value<TryxRuntimeApplyRequest>();
    QCOMPARE(
        request.sysinfoLabels,
        QStringList{QStringLiteral("CPU Temperature")});
}

void QuickClientTests::
    unavailableMetricGrandfatheringIsContextBound() {
    RuntimeClient runtime(true);
    prepareModernDisplay(&runtime);
    runtime.display_.revision = 7;
    runtime.display_.screenMode = QStringLiteral("Full Screen");
    runtime.display_.sysinfoLabels = {
        QStringLiteral("CPU Temperature")};
    runtime.metrics_.revision = 4;
    runtime.metrics_.availableMetrics = {
        QStringLiteral("GPU Temperature")};
    runtime.metricsCatalogReady_ = true;
    runtime.metricsCatalog_ = tryxMetricsCatalog();
    QString error;

    QVERIFY(runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Full, &error));
    QVERIFY(!runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Right, &error));

    TryxRuntimeMetricsState newerMetrics = runtime.metrics_;
    newerMetrics.revision = 5;
    runtime.applyMetricsState(newerMetrics);
    QVERIFY(runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Full, &error));

    TryxRuntimeDisplayState replacement = runtime.display_;
    replacement.revision = 8;
    replacement.sysinfoLabels.clear();
    runtime.applyDisplayState(replacement);
    QVERIFY(!runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Full, &error));

    replacement.revision = 9;
    replacement.screenMode = QStringLiteral("Screen Splitting");
    replacement.sysinfoLabels = {
        QStringLiteral("CPU Temperature")};
    runtime.applyDisplayState(replacement);
    QVERIFY(runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Left, &error));
    QVERIFY(!runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Right, &error));

    TryxRuntimeSnapshot replacementDevice;
    replacementDevice.revision = 1;
    replacementDevice.connected = true;
    replacementDevice.printerClassConnected = true;
    replacementDevice.printerClassDevicePresent = true;
    replacementDevice.displaySessionActive = true;
    replacementDevice.serial = QStringLiteral("device-b");
    runtime.applyConnectionSnapshot(replacementDevice);
    QVERIFY(!runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Left, &error));
}

void QuickClientTests::
    legacyFallbackCatalogDropsStaleConfirmedDisplayTokens() {
    RuntimeClient runtime(true);
    prepareModernDisplay(&runtime);
    runtime.connection_.revision = 1;
    runtime.display_.revision = 7;
    runtime.display_.screenMode = QStringLiteral("Full Screen");
    runtime.display_.sysinfoLabels = {
        QStringLiteral("CPU Temperature")};
    runtime.metrics_.revision = 4;
    runtime.metrics_.availableMetrics = {
        QStringLiteral("GPU Temperature")};
    runtime.metricsCatalogReady_ = true;
    runtime.metricsCatalogLegacyFallback_ = true;
    runtime.refreshLegacyMetricsCatalog();
    QCOMPARE(
        runtime.metricsCatalog(),
        QStringList({QStringLiteral("GPU Temperature"),
                     QStringLiteral("CPU Temperature")}));

    TryxRuntimeSnapshot replacementDevice;
    replacementDevice.revision = 2;
    replacementDevice.connected = true;
    replacementDevice.printerClassConnected = true;
    replacementDevice.printerClassDevicePresent = true;
    replacementDevice.displaySessionActive = true;
    replacementDevice.serial = QStringLiteral("device-b");
    runtime.applyConnectionSnapshot(replacementDevice);

    QCOMPARE(
        runtime.metricsCatalog(),
        QStringList{QStringLiteral("GPU Temperature")});
}

void QuickClientTests::
    metricSelectionLimitIsThreePerAreaAtClient() {
    const QStringList leftThree = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("CPU Frequency"),
        QStringLiteral("CPU Usage")};
    const QStringList rightThree = {
        QStringLiteral("GPU Temperature"),
        QStringLiteral("GPU Frequency"),
        QStringLiteral("GPU Usage")};
    const auto prepare = [](RuntimeClient *runtime) {
        prepareModernDisplay(runtime);
        runtime->metrics_.availableMetrics = tryxMetricsCatalog();
        runtime->metricsCatalogReady_ = true;
        runtime->metricsCatalog_ = tryxMetricsCatalog();
    };

    RuntimeClient fullThree(true);
    prepare(&fullThree);
    QVERIFY(!fullThree.submitFullDisplayDraft(
        {QStringLiteral("full.mp4.h264_2240x1080")},
        QStringLiteral("Single"), leftThree, {},
        QStringLiteral("Top"), QStringLiteral("#112233"),
        QStringLiteral("Left"), true, false, 0,
        false, false, false).isEmpty());
    QCOMPARE(fullThree.offlineRequests_.size(), 1);

    RuntimeClient fullFour(true);
    prepare(&fullFour);
    QStringList leftFour = leftThree;
    leftFour.append(QStringLiteral("CPU Power"));
    QVERIFY(fullFour.submitFullDisplayDraft(
        {QStringLiteral("full.mp4.h264_2240x1080")},
        QStringLiteral("Single"), leftFour, {},
        QStringLiteral("Top"), QStringLiteral("#112233"),
        QStringLiteral("Left"), true, false, 0,
        false, false, false).isEmpty());
    QVERIFY(fullFour.offlineRequests_.isEmpty());

    const auto submitSplit = [&prepare](
                                 const QStringList &left,
                                 const QStringList &right) {
        RuntimeClient runtime(true);
        prepare(&runtime);
        const QString id = runtime.submitSplitDisplayDraft(
            QStringLiteral("left.mp4.h264_2240x1080"),
            QStringLiteral("right.mp4.h264_2240x1080"),
            QStringLiteral("Single"), left, right, {}, {},
            QStringLiteral("Top"), QStringLiteral("#112233"),
            QStringLiteral("Left"), QStringLiteral("Bottom"),
            QStringLiteral("#445566"), QStringLiteral("Right"),
            true, false, 0, false, false, false);
        return qMakePair(id, runtime.offlineRequests_.size());
    };

    const auto accepted = submitSplit(leftThree, rightThree);
    QVERIFY(!accepted.first.isEmpty());
    QCOMPARE(accepted.second, 1);

    QStringList rightFour = rightThree;
    rightFour.append(QStringLiteral("GPU Power"));
    const auto rejectedLeft = submitSplit(leftFour, rightThree);
    QVERIFY(rejectedLeft.first.isEmpty());
    QCOMPARE(rejectedLeft.second, 0);
    const auto rejectedRight = submitSplit(leftThree, rightFour);
    QVERIFY(rejectedRight.first.isEmpty());
    QCOMPARE(rejectedRight.second, 0);
}

void QuickClientTests::
    unconfirmedCatalogDoesNotAuthorizeNewMetric() {
    RuntimeClient runtime(true);
    runtime.metrics_.revision = 1;
    runtime.metrics_.availableMetrics = {
        QStringLiteral("CPU Temperature")};
    QString error;

    QVERIFY(!runtime.metricsSelectionValid(
        {QStringLiteral("CPU Temperature")}, true,
        RuntimeClient::MetricArea::Full, &error));
    QVERIFY(!error.isEmpty());
}

void QuickClientTests::
    metricsRequestUsesExplicitEnableDisableContract() {
    RuntimeClient runtime(true);
    TryxRuntimeMetricsState state;
    state.revision = 1;
    state.availableMetrics = {
        QStringLiteral("CPU Temperature"),
        QStringLiteral("GPU Temperature")};
    state.alignment = QStringLiteral("Right");
    state.textColor = 0x00123456;
    runtime.applyMetricsState(state);
    runtime.metricsCatalogReady_ = true;
    runtime.metricsCatalog_ = tryxMetricsCatalog();

    TryxRuntimeMetricsConfigRequest request;
    QString error;
    QVERIFY(runtime.metricsConfigRequest(
        false,
        {QStringLiteral("CPU Temperature")},
        QStringLiteral("invalid"),
        QStringLiteral("not-a-color"),
        &request, &error));
    QVERIFY(request.metrics.isEmpty());
    QVERIFY(!request.enabled);
    QCOMPARE(request.alignment, state.alignment);
    QCOMPARE(request.textColor, state.textColor);

    QVERIFY(!runtime.metricsConfigRequest(
        true, {}, QStringLiteral("Left"),
        QStringLiteral("#dcdcdc"), &request, &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(runtime.metricsConfigRequest(
        true,
        {QStringLiteral("CPU Temperature")},
        QStringLiteral("Center"),
        QStringLiteral("#abcdef"),
        &request, &error));
    QVERIFY(request.enabled);
    QCOMPARE(
        request.metrics,
        QStringList{QStringLiteral("CPU Temperature")});
    QCOMPARE(request.alignment, QStringLiteral("Center"));
    QCOMPARE(request.textColor, 0x00abcdefU);
}

void QuickClientTests::systemMetricsModelMapsAvailability() {
    SystemMetricsModel model(false, nullptr);
    QSignalSpy changed(
        &model, &SystemMetricsModel::metricsChanged);

    SystemMetrics sample;
    sample.cpu.usagePercent = 17.5;
    sample.cpu.usageAvailable = true;
    sample.cpu.temperature = 54.0;
    sample.cpu.temperatureAvailable = true;
    sample.cpu.frequencyMHz = 4250.0;
    sample.cpu.frequencyAvailable = true;

    GpuMetrics gpu;
    gpu.name = QStringLiteral("Primary GPU");
    gpu.usagePercent = 42.0;
    gpu.usageAvailable = true;
    gpu.temperature = 61.0;
    gpu.temperatureAvailable = true;
    gpu.frequencyMHz = 2300.0;
    gpu.frequencyAvailable = true;
    gpu.powerWatts = 0.0;
    gpu.powerAvailable = true;
    gpu.vramUsedMB = 0;
    gpu.vramTotalMB = 8192;
    gpu.vramAvailable = true;
    sample.gpus.append(gpu);

    sample.ram.usagePercent = 33.0;
    sample.ram.usageAvailable = true;
    sample.ram.usedMB = 10240;
    sample.ram.totalMB = 32768;
    sample.disk.usagePercent = 58.0;
    sample.disk.usageAvailable = true;
    sample.disk.usedGB = 580;
    sample.disk.totalGB = 1000;
    sample.net.available = true;
    sample.net.rxSpeedKBs = 125.5;
    sample.net.txSpeedKBs = 12.25;

    model.applyMetrics(sample);

    QCOMPARE(changed.count(), 1);
    QVERIFY(model.sampled());
    QVERIFY(model.cpuUsageAvailable());
    QCOMPARE(model.cpuUsage(), 17.5);
    QCOMPARE(model.cpuTemperature(), 54.0);
    QVERIFY(model.gpuPresent());
    QCOMPARE(model.gpuName(), QStringLiteral("Primary GPU"));
    QVERIFY(model.gpuPowerAvailable());
    QCOMPARE(model.gpuPowerWatts(), 0.0);
    QCOMPARE(model.gpuVramUsedMB(), 0);
    QVERIFY(model.gpuVramAvailable());
    QCOMPARE(model.ramTotalMB(), 32768);
    QVERIFY(model.diskUsageAvailable());
    QCOMPARE(model.diskTotalGB(), 1000);
    QVERIFY(model.networkAvailable());
    QCOMPARE(model.rxSpeedKBs(), 125.5);

    sample.gpus[0].powerWatts = 125.0;
    sample.gpus[0].powerAvailable = false;
    sample.gpus[0].vramUsedMB = 4096;
    sample.gpus[0].vramAvailable = false;
    model.applyMetrics(sample);
    QVERIFY(!model.gpuPowerAvailable());
    QVERIFY(!model.gpuVramAvailable());

    sample.gpus.clear();
    model.applyMetrics(sample);
    QVERIFY(!model.gpuPresent());
    QVERIFY(!model.gpuUsageAvailable());
    QCOMPARE(model.gpuName(), QString());
}

void QuickClientTests::
    systemMetricsModelPollsOnlyWhileDashboardActive() {
    SystemMetricsModel model(true, nullptr);
    QSignalSpy activeChanged(
        &model, &SystemMetricsModel::dashboardActiveChanged);

    QVERIFY(!model.dashboardActive());
    QVERIFY(!model.updateTimer_->isActive());

    model.setDashboardActive(true);
    QVERIFY(model.dashboardActive());
    QVERIFY(model.updateTimer_->isActive());
    QCOMPARE(activeChanged.count(), 1);

    model.setDashboardActive(false);
    QVERIFY(!model.dashboardActive());
    QVERIFY(!model.updateTimer_->isActive());
    QCOMPARE(activeChanged.count(), 2);
}

void QuickClientTests::
    appSettingsDefaultToEnglishAndPreserveConfig() {
    QTemporaryDir configRoot;
    QVERIFY(configRoot.isValid());

    const bool hadConfigHome =
        qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    const QByteArray previousConfigHome =
        qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME",
            QFile::encodeName(configRoot.path()));

    {
        AppSettingsController settings(true);
        QCOMPARE(settings.language(), QStringLiteral("en"));
        QVERIFY(settings.hideToTrayOnClose());
        QVERIFY(settings.errorMessage().isEmpty());
    }

    const QString configDirectory = QString::fromStdString(
        panorama::ConfigManager::get_config_dir());
    QVERIFY(QDir().mkpath(configDirectory));
    QFile legacyConfig(QString::fromStdString(
        panorama::ConfigManager::get_config_path()));
    QVERIFY(legacyConfig.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray legacyJson = QByteArrayLiteral(
        "{\"port\":\"/dev/ttyACM7\",\"brightness\":61,"
        "\"keepalive_interval\":23,\"language\":\"ru\","
        "\"pase_overlay_lease_mode\":\"ping-only\"}");
    QCOMPARE(legacyConfig.write(legacyJson), legacyJson.size());
    legacyConfig.close();

    const auto legacyLoaded =
        panorama::ConfigManager::load_config();
    QVERIFY(legacyLoaded.has_value());
    QCOMPARE(legacyLoaded->port, std::string("/dev/ttyACM7"));
    QCOMPARE(legacyLoaded->keepalive_interval, 23);
    QCOMPARE(legacyLoaded->close_behavior,
             std::string("hide-to-tray"));

    QFile invalidGuiConfig(QString::fromStdString(
        panorama::ConfigManager::get_config_path()));
    QVERIFY(invalidGuiConfig.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray invalidGuiJson = QByteArrayLiteral(
        "{\"port\":\"/dev/ttyACM8\",\"brightness\":62,"
        "\"keepalive_interval\":24,\"language\":\"ru\","
        "\"close_behavior\":42,"
        "\"pase_overlay_lease_mode\":\"ping-only\"}");
    QCOMPARE(invalidGuiConfig.write(invalidGuiJson),
             invalidGuiJson.size());
    invalidGuiConfig.close();

    const auto invalidGuiLoaded =
        panorama::ConfigManager::load_config();
    QVERIFY(invalidGuiLoaded.has_value());
    QCOMPARE(invalidGuiLoaded->port, std::string("/dev/ttyACM8"));
    QCOMPARE(invalidGuiLoaded->keepalive_interval, 24);
    QCOMPARE(invalidGuiLoaded->close_behavior,
             std::string("hide-to-tray"));

    panorama::Config config;
    config.port = "ttyACM-test";
    config.brightness = 61;
    config.keepalive_interval = 23;
    config.language = "ru";
    config.close_behavior = "quit-gui";
    config.pase_overlay_lease_mode =
        "ping-and-overlay-lease";
    QVERIFY(panorama::ConfigManager::save_config(config));

    {
        AppSettingsController settings(true);
        QCOMPARE(settings.language(), QStringLiteral("ru"));
        QVERIFY(!settings.hideToTrayOnClose());
        QCOMPARE(
            settings.devicePort(),
            QString::fromStdString(config.port));
        QCOMPARE(
            settings.keepaliveInterval(),
            config.keepalive_interval);
        QSignalSpy languageChanged(
            &settings,
            &AppSettingsController::languageChanged);
        QSignalSpy deviceSettingsChanged(
            &settings,
            &AppSettingsController::deviceSettingsChanged);
        QSignalSpy closeBehaviorChanged(
            &settings,
            &AppSettingsController::closeBehaviorChanged);
        settings.setLanguage(QStringLiteral("en"));
        settings.setDevicePort(
            QStringLiteral("/dev/ttyACM9"));
        settings.setKeepaliveInterval(41);
        settings.setHideToTrayOnClose(true);
        settings.setHideToTrayOnClose(true);
        QCOMPARE(settings.language(), QStringLiteral("en"));
        QVERIFY(settings.hideToTrayOnClose());
        QCOMPARE(
            settings.devicePort(),
            QStringLiteral("/dev/ttyACM9"));
        QCOMPARE(settings.keepaliveInterval(), 41);
        QCOMPARE(languageChanged.count(), 1);
        QCOMPARE(deviceSettingsChanged.count(), 2);
        QCOMPARE(closeBehaviorChanged.count(), 1);
        QVERIFY(settings.errorMessage().isEmpty());
    }

    const auto saved =
        panorama::ConfigManager::load_config();
    QVERIFY(saved.has_value());
    QCOMPARE(saved->language, std::string("en"));
    QCOMPARE(saved->port, std::string("/dev/ttyACM9"));
    QCOMPARE(saved->brightness, config.brightness);
    QCOMPARE(saved->keepalive_interval, 41);
    QCOMPARE(saved->close_behavior, std::string("hide-to-tray"));
    QCOMPARE(saved->pase_overlay_lease_mode,
             config.pase_overlay_lease_mode);

    if (hadConfigHome) {
        qputenv("XDG_CONFIG_HOME", previousConfigHome);
    } else {
        qunsetenv("XDG_CONFIG_HOME");
    }
}

void QuickClientTests::
    guiAutostartRoundTripsWithoutRuntimeService() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString fakeBin =
        QDir(testRoot.path()).filePath(QStringLiteral("fake-bin"));
    const QString systemctlLog =
        QDir(testRoot.path()).filePath(QStringLiteral("systemctl.log"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(systemConfig));
    QVERIFY(QDir().mkpath(fakeBin));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    ScopedEnvironmentVariable pathEnvironment("PATH");
    ScopedEnvironmentVariable logEnvironment(
        "TRYX_GUI_AUTOSTART_TEST_SYSTEMCTL_LOG");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));
    pathEnvironment.set(QFile::encodeName(fakeBin));
    QVERIFY(createFakeSystemctl(fakeBin, systemctlLog));

    const QString entryPath = guiAutostartEntryPath(configHome);
    {
        AppSettingsController settings(false);
        QTRY_VERIFY_WITH_TIMEOUT(!settings.busy(), 3000);
        QVERIFY(settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(settings.errorMessage().isEmpty());

        settings.setAutostartEnabled(true);
        QTRY_VERIFY_WITH_TIMEOUT(!settings.busy(), 3000);
        QVERIFY(settings.autostartAvailable());
        QVERIFY(settings.autostartEnabled());
        QVERIFY(settings.errorMessage().isEmpty());
        QVERIFY(QFileInfo::exists(entryPath));
    }

    const QFileInfo autostartDirectory(
        QFileInfo(entryPath).absolutePath());
    QVERIFY(autostartDirectory.isDir());
    QCOMPARE(
        autostartDirectory.permissions() &
            (QFileDevice::ReadGroup |
             QFileDevice::WriteGroup |
             QFileDevice::ExeGroup |
             QFileDevice::ReadOther |
             QFileDevice::WriteOther |
             QFileDevice::ExeOther),
        QFileDevice::Permissions{});

    const QFileInfo entryInfo(entryPath);
    QVERIFY(entryInfo.isFile());
    QVERIFY(!entryInfo.isSymLink());
    QCOMPARE(
        entryInfo.permissions() &
            (QFileDevice::ReadGroup |
             QFileDevice::WriteGroup |
             QFileDevice::ExeGroup |
             QFileDevice::ReadOther |
             QFileDevice::WriteOther |
             QFileDevice::ExeOther),
        QFileDevice::Permissions{});

    QSettings desktopEntry(entryPath, QSettings::IniFormat);
    desktopEntry.beginGroup(QStringLiteral("Desktop Entry"));
    QCOMPARE(
        desktopEntry.value(QStringLiteral("Type")).toString(),
        QStringLiteral("Application"));
    QCOMPARE(
        desktopEntry.value(QStringLiteral("TryExec")).toString(),
        QCoreApplication::applicationFilePath());
    const QString exec =
        desktopEntry.value(QStringLiteral("Exec")).toString();
    QVERIFY(exec.contains(QCoreApplication::applicationFilePath()));
    QFile rawDesktopEntry(entryPath);
    QVERIFY(rawDesktopEntry.open(QIODevice::ReadOnly));
    const QByteArray rawDesktopEntryContents =
        rawDesktopEntry.readAll();
    QVERIFY(rawDesktopEntryContents.contains(
        QByteArrayLiteral(" --autostart\n")));
    QSet<QByteArray> managedKeys;
    for (const QByteArray &line :
         rawDesktopEntryContents.split('\n')) {
        const qsizetype separator = line.indexOf('=');
        if (separator > 0) {
            managedKeys.insert(line.left(separator));
        }
    }
    const QSet<QByteArray> expectedManagedKeys{
        QByteArrayLiteral("Type"),
        QByteArrayLiteral("Name"),
        QByteArrayLiteral("Name[ru]"),
        QByteArrayLiteral("Exec"),
        QByteArrayLiteral("TryExec"),
        QByteArrayLiteral("Icon"),
        QByteArrayLiteral("Terminal"),
        QByteArrayLiteral("StartupNotify"),
        QByteArrayLiteral("Hidden"),
        QByteArrayLiteral("X-TRYX-Panorama-Managed")};
    QCOMPARE(managedKeys, expectedManagedKeys);
    QCOMPARE(
        desktopEntry.value(QStringLiteral("Icon")).toString(),
        QStringLiteral("tryx-panorama"));
    QVERIFY(!desktopEntry.value(
        QStringLiteral("Terminal"), true).toBool());
    QVERIFY(!desktopEntry.value(
        QStringLiteral("StartupNotify"), true).toBool());
    QVERIFY(!desktopEntry.value(
        QStringLiteral("Hidden"), true).toBool());
    QVERIFY(desktopEntry.value(
        QStringLiteral("X-TRYX-Panorama-Managed"), false).toBool());
    desktopEntry.endGroup();
    QCOMPARE(desktopEntry.status(), QSettings::NoError);

    {
        AppSettingsController reloaded(false);
        QTRY_VERIFY_WITH_TIMEOUT(!reloaded.busy(), 3000);
        QVERIFY(reloaded.autostartAvailable());
        QVERIFY(reloaded.autostartEnabled());
        reloaded.setAutostartEnabled(false);
        QTRY_VERIFY_WITH_TIMEOUT(!reloaded.busy(), 3000);
        QVERIFY(reloaded.autostartAvailable());
        QVERIFY(!reloaded.autostartEnabled());
        QVERIFY(reloaded.errorMessage().isEmpty());
    }
    QVERIFY(!QFileInfo::exists(entryPath));

    {
        AppSettingsController reloaded(false);
        QTRY_VERIFY_WITH_TIMEOUT(!reloaded.busy(), 3000);
        QVERIFY(reloaded.autostartAvailable());
        QVERIFY(!reloaded.autostartEnabled());
    }

    QFile log(systemctlLog);
    if (log.exists()) {
        QVERIFY(log.open(QIODevice::ReadOnly));
        const QByteArray calls = log.readAll();
        QVERIFY2(
            calls.isEmpty(),
            qPrintable(QStringLiteral(
                "GUI autostart invoked systemctl: %1")
                           .arg(QString::fromLocal8Bit(calls))));
    }
}

void QuickClientTests::
    guiAutostartCreatesPrivateDirectoryWithPermissiveUmask_data() {
    QTest::addColumn<uint>("mask");

    QTest::newRow("group-writable-default") << uint{0002};
    QTest::newRow("all-permissions-masked") << uint{0777};
}

void QuickClientTests::
    guiAutostartCreatesPrivateDirectoryWithPermissiveUmask() {
    QFETCH(uint, mask);

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    QVERIFY(QDir().mkpath(systemConfig));
    QVERIFY(!QFileInfo::exists(configHome));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    {
        ScopedUmask permissiveUmask(static_cast<mode_t>(mask));
        AppSettingsController settings(false);
        QVERIFY(settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(settings.autostartAvailable());
        QVERIFY(settings.autostartEnabled());
        QVERIFY(settings.errorMessage().isEmpty());
    }

    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QFileInfo::exists(entryPath));
    const QByteArray encodedConfigHome = QFile::encodeName(configHome);
    struct stat configStatus {};
    QCOMPARE(::lstat(encodedConfigHome.constData(), &configStatus), 0);
    QVERIFY(S_ISDIR(configStatus.st_mode));
    QVERIFY(!S_ISLNK(configStatus.st_mode));
    QCOMPARE(configStatus.st_uid, ::getuid());
    QCOMPARE(
        static_cast<mode_t>(configStatus.st_mode & 0777),
        static_cast<mode_t>(0700));

    const QByteArray encodedDirectory = QFile::encodeName(
        QFileInfo(entryPath).absolutePath());
    struct stat directoryStatus {};
    QCOMPARE(
        ::lstat(encodedDirectory.constData(), &directoryStatus), 0);
    QCOMPARE(
        static_cast<mode_t>(directoryStatus.st_mode & 0777),
        static_cast<mode_t>(0700));
}

void QuickClientTests::
    guiAutostartRejectsUnsafeConfigHierarchy_data() {
    QTest::addColumn<bool>("unsafeParent");

    QTest::newRow("group-writable-config-home") << false;
    QTest::newRow("group-writable-immediate-parent") << true;
}

void QuickClientTests::
    guiAutostartRejectsUnsafeConfigHierarchy() {
    QFETCH(bool, unsafeParent);

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configParent =
        QDir(testRoot.path()).filePath(QStringLiteral("config-parent"));
    const QString configHome =
        QDir(configParent).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(systemConfig));

    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner |
        QFileDevice::WriteOwner |
        QFileDevice::ExeOwner;
    const QFileDevice::Permissions groupWritable =
        ownerOnly |
        QFileDevice::ReadGroup |
        QFileDevice::WriteGroup |
        QFileDevice::ExeGroup;
    QVERIFY(QFile::setPermissions(configParent,
                                  unsafeParent
                                      ? groupWritable
                                      : ownerOnly));
    QVERIFY(QFile::setPermissions(configHome,
                                  unsafeParent
                                      ? ownerOnly
                                      : groupWritable));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    const gui_autostart::State state = gui_autostart::query();
    QVERIFY(!state.available);
    QVERIFY(!state.enabled);
    QVERIFY(!state.error.isEmpty());

    QString enableError;
    QVERIFY(!gui_autostart::setEnabled(true, &enableError));
    QVERIFY(!enableError.isEmpty());
    QVERIFY(!QFileInfo::exists(guiAutostartEntryPath(configHome)));
}

void QuickClientTests::guiAutostartRejectsEmptyConfigDirectory() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    ScopedCurrentDirectory currentDirectory(testRoot.path());
    QVERIFY(currentDirectory.changed());

    const QString relativeEntryPath =
        guiAutostartEntryPath(testRoot.path());
    QVERIFY(QDir().mkpath(QFileInfo(relativeEntryPath).absolutePath()));
    QVERIFY(QFile::setPermissions(
        QFileInfo(relativeEntryPath).absolutePath(),
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QByteArray sentinel = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false);
    QFile relativeEntry(relativeEntryPath);
    QVERIFY(relativeEntry.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(relativeEntry.write(sentinel), sentinel.size());
    relativeEntry.close();
    QVERIFY(QFile::setPermissions(
        relativeEntryPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    ScopedGuiAutostartConfigOverride emptyConfig(QString{});
    const gui_autostart::State state = gui_autostart::query();
    QVERIFY(!state.available);
    QVERIFY(!state.enabled);
    QVERIFY(!state.error.isEmpty());

    QString enableError;
    QVERIFY(!gui_autostart::setEnabled(true, &enableError));
    QVERIFY(!enableError.isEmpty());
    QString disableError;
    QVERIFY(!gui_autostart::setEnabled(false, &disableError));
    QVERIFY(!disableError.isEmpty());

    QFile unchanged(relativeEntryPath);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), sentinel);
}

void QuickClientTests::
    guiAutostartRejectsUnsafeAndForeignEntries() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString fakeBin =
        QDir(testRoot.path()).filePath(QStringLiteral("fake-bin"));
    const QString systemctlLog =
        QDir(testRoot.path()).filePath(QStringLiteral("systemctl.log"));
    const QString autostartDirectory =
        QDir(configHome).filePath(QStringLiteral("autostart"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(autostartDirectory));
    QVERIFY(QDir().mkpath(systemConfig));
    QVERIFY(QDir().mkpath(fakeBin));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    ScopedEnvironmentVariable pathEnvironment("PATH");
    ScopedEnvironmentVariable logEnvironment(
        "TRYX_GUI_AUTOSTART_TEST_SYSTEMCTL_LOG");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));
    pathEnvironment.set(QFile::encodeName(fakeBin));
    QVERIFY(createFakeSystemctl(fakeBin, systemctlLog));

    const QString externalPath =
        QDir(testRoot.path()).filePath(QStringLiteral("external.desktop"));
    const QByteArray externalSentinel =
        QByteArrayLiteral("external-sentinel\n");
    QFile external(externalPath);
    QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        external.write(externalSentinel),
        externalSentinel.size());
    external.close();
    QVERIFY(QFile::link(externalPath, entryPath));

    {
        AppSettingsController settings(false);
        QTRY_VERIFY_WITH_TIMEOUT(!settings.busy(), 3000);
        QVERIFY(!settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
    }
    QFile unchangedExternal(externalPath);
    QVERIFY(unchangedExternal.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedExternal.readAll(), externalSentinel);
    unchangedExternal.close();
    QVERIFY(QFileInfo(entryPath).isSymLink());
    QVERIFY(QFile::remove(entryPath));

    const QByteArray externalEncoded =
        QFile::encodeName(externalPath);
    const QByteArray entryEncoded =
        QFile::encodeName(entryPath);
    QCOMPARE(
        ::link(externalEncoded.constData(),
               entryEncoded.constData()),
        0);
    {
        AppSettingsController settings(false);
        QVERIFY(!settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
    }
    QFile unchangedHardLink(externalPath);
    QVERIFY(unchangedHardLink.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedHardLink.readAll(), externalSentinel);
    unchangedHardLink.close();
    QVERIFY(QFile::remove(entryPath));

    QVERIFY(QDir().mkdir(entryPath));
    {
        AppSettingsController settings(false);
        QTRY_VERIFY_WITH_TIMEOUT(!settings.busy(), 3000);
        QVERIFY(!settings.autostartAvailable());
        settings.setAutostartEnabled(false);
        QVERIFY(QFileInfo(entryPath).isDir());
    }
    QVERIFY(QDir().rmdir(entryPath));

    const QByteArray foreignEntry = QByteArrayLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Custom TRYX launcher\n"
        "Exec=/usr/bin/true\n");
    QFile foreign(entryPath);
    QVERIFY(foreign.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(foreign.write(foreignEntry), foreignEntry.size());
    foreign.close();
    {
        AppSettingsController settings(false);
        QTRY_VERIFY_WITH_TIMEOUT(!settings.busy(), 3000);
        QVERIFY(!settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
    }
    QFile unchangedForeign(entryPath);
    QVERIFY(unchangedForeign.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedForeign.readAll(), foreignEntry);
    unchangedForeign.close();
    QVERIFY(QFile::remove(entryPath));

    const QByteArray incompatibleManagedEntry = QByteArrayLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=TRYX Panorama Manager\n"
        "Exec=/usr/bin/true --autostart\n"
        "TryExec=/usr/bin/true\n"
        "Icon=tryx-panorama\n"
        "Terminal=false\n"
        "StartupNotify=false\n"
        "Hidden=false\n"
        "X-TRYX-Panorama-Managed=true\n");
    QFile incompatible(entryPath);
    QVERIFY(incompatible.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        incompatible.write(incompatibleManagedEntry),
        incompatibleManagedEntry.size());
    incompatible.close();
    {
        AppSettingsController settings(false);
        QVERIFY(!settings.autostartAvailable());
        settings.setAutostartEnabled(false);
        QVERIFY(QFileInfo::exists(entryPath));
    }
    QFile unchangedIncompatible(entryPath);
    QVERIFY(unchangedIncompatible.open(QIODevice::ReadOnly));
    QCOMPARE(
        unchangedIncompatible.readAll(),
        incompatibleManagedEntry);
    unchangedIncompatible.close();
    QVERIFY(QFile::remove(entryPath));

    QVERIFY(QDir(configHome).rmdir(QStringLiteral("autostart")));
    const QString externalDirectory =
        QDir(testRoot.path()).filePath(
            QStringLiteral("external-autostart"));
    QVERIFY(QDir().mkpath(externalDirectory));
    QVERIFY(QFile::link(
        externalDirectory, autostartDirectory));
    {
        AppSettingsController settings(false);
        QVERIFY(!settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!QFileInfo::exists(
            QDir(externalDirectory).filePath(
                QStringLiteral(
                    "tryx-panorama-manager.desktop"))));
    }
}

void QuickClientTests::
    guiAutostartPreservesMalformedManagedEntries_data() {
    QTest::addColumn<QByteArray>("contents");

    const QByteArray managed = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false);

    QByteArray duplicateKey = managed;
    duplicateKey.append(QByteArrayLiteral("Hidden=false\n"));
    QTest::newRow("duplicate-key") << duplicateKey;

    QByteArray badEscape = managed;
    QVERIFY(badEscape.contains(
        QByteArrayLiteral("Name=TRYX Panorama Manager")));
    badEscape.replace(
        QByteArrayLiteral("Name=TRYX Panorama Manager"),
        QByteArrayLiteral("Name=TRYX\\q Panorama Manager"));
    QTest::newRow("bad-escape") << badEscape;

    QByteArray invalidUtf8 = managed;
    invalidUtf8.append('#');
    invalidUtf8.append(static_cast<char>(0xff));
    invalidUtf8.append('\n');
    QTest::newRow("invalid-utf8") << invalidUtf8;

    QByteArray oversized = managed;
    oversized.append(QByteArray(
        64 * 1024 + 1 - oversized.size(), '#'));
    QCOMPARE(oversized.size(), 64 * 1024 + 1);
    QTest::newRow("oversized") << oversized;

    QByteArray actionGroup = managed;
    actionGroup.append(QByteArrayLiteral(
        "[Desktop Action Foo]\n"
        "Name=Foreign action\n"
        "Exec=/usr/bin/true\n"));
    QTest::newRow("extra-action-group") << actionGroup;

    QByteArray comment = managed;
    comment.append(QByteArrayLiteral("# preserve this comment\n"));
    QTest::newRow("comment") << comment;

    QByteArray repeatedSection = managed;
    repeatedSection.append(QByteArrayLiteral("[Desktop Entry]\n"));
    QTest::newRow("repeated-desktop-entry-section")
        << repeatedSection;
}

void QuickClientTests::
    guiAutostartPreservesMalformedManagedEntries() {
    QFETCH(QByteArray, contents);

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(QFileInfo(entryPath).absolutePath()));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(entry.write(contents), contents.size());
    entry.close();
    QVERIFY(QFile::setPermissions(
        entryPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    AppSettingsController settings(false);
    QVERIFY(!settings.autostartAvailable());
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(!settings.errorMessage().isEmpty());
    settings.setAutostartEnabled(true);
    QVERIFY(!settings.autostartEnabled());
    settings.setAutostartEnabled(false);
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(!settings.errorMessage().isEmpty());

    QFile unchanged(entryPath);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), contents);
}

void QuickClientTests::guiAutostartRejectsWritableUserPaths() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString autostartDirectory =
        QDir(configHome).filePath(QStringLiteral("autostart"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(autostartDirectory));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    QVERIFY(QFile::setPermissions(
        autostartDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner |
            QFileDevice::ReadGroup |
            QFileDevice::WriteGroup |
            QFileDevice::ExeGroup));
    {
        AppSettingsController settings(false);
        QVERIFY(!settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        settings.setAutostartEnabled(true);
        QVERIFY(!QFileInfo::exists(entryPath));
    }

    QVERIFY(QFile::setPermissions(
        autostartDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));
    const QByteArray managed = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false);
    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(entry.write(managed), managed.size());
    entry.close();
    QVERIFY(QFile::setPermissions(
        entryPath,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::WriteGroup));

    {
        AppSettingsController settings(false);
        QVERIFY(!settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        settings.setAutostartEnabled(false);
        QVERIFY(QFileInfo::exists(entryPath));
    }
    QFile unchanged(entryPath);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), managed);
}

void QuickClientTests::guiAutostartRequiresExactManagedKeys() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(QFileInfo(entryPath).absolutePath()));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    const QByteArray managedWithExtraKey = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false,
        QByteArrayLiteral("X-Foreign-Key=true\n"));
    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        entry.write(managedWithExtraKey),
        managedWithExtraKey.size());
    entry.close();
    QVERIFY(QFile::setPermissions(
        entryPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    AppSettingsController settings(false);
    QVERIFY(!settings.autostartAvailable());
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(!settings.errorMessage().isEmpty());
    settings.setAutostartEnabled(false);
    QVERIFY(QFileInfo::exists(entryPath));

    QFile unchanged(entryPath);
    QVERIFY(unchanged.open(QIODevice::ReadOnly));
    QCOMPARE(unchanged.readAll(), managedWithExtraKey);
    unchanged.close();

    const QByteArray managedWithOnlyShowIn = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false,
        QByteArrayLiteral("OnlyShowIn=KDE;\n"));
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(
        entry.write(managedWithOnlyShowIn),
        managedWithOnlyShowIn.size());
    entry.close();
    AppSettingsController visibilityExtra(false);
    QVERIFY(!visibilityExtra.autostartAvailable());
    QVERIFY(!visibilityExtra.autostartEnabled());
    QVERIFY(!visibilityExtra.errorMessage().isEmpty());
    visibilityExtra.setAutostartEnabled(false);
    QVERIFY(QFileInfo::exists(entryPath));

    QFile unchangedVisibilityExtra(entryPath);
    QVERIFY(unchangedVisibilityExtra.open(QIODevice::ReadOnly));
    QCOMPARE(
        unchangedVisibilityExtra.readAll(),
        managedWithOnlyShowIn);
}

void QuickClientTests::guiAutostartMasksSystemEntry() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(writeSystemAutostartEntry(
        systemConfig, false, false));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    const QString userEntryPath =
        guiAutostartEntryPath(configHome);
    {
        AppSettingsController settings(false);
        QVERIFY(settings.autostartAvailable());
        QVERIFY(settings.autostartEnabled());
        settings.setAutostartEnabled(false);
        QVERIFY(settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(settings.errorMessage().isEmpty());
    }

    QVERIFY(QFileInfo::exists(userEntryPath));
    QSettings overrideEntry(
        userEntryPath, QSettings::IniFormat);
    overrideEntry.beginGroup(QStringLiteral("Desktop Entry"));
    QVERIFY(overrideEntry.value(
        QStringLiteral("Hidden"), false).toBool());
    QVERIFY(overrideEntry.value(
        QStringLiteral("X-TRYX-Panorama-Managed"),
        false).toBool());
    overrideEntry.endGroup();

    {
        AppSettingsController reloaded(false);
        QVERIFY(reloaded.autostartAvailable());
        QVERIFY(!reloaded.autostartEnabled());
        reloaded.setAutostartEnabled(true);
        QVERIFY(reloaded.autostartAvailable());
        QVERIFY(reloaded.autostartEnabled());
        QVERIFY(reloaded.errorMessage().isEmpty());
    }

    QSettings enabledEntry(
        userEntryPath, QSettings::IniFormat);
    enabledEntry.beginGroup(QStringLiteral("Desktop Entry"));
    QVERIFY(!enabledEntry.value(
        QStringLiteral("Hidden"), true).toBool());
    enabledEntry.endGroup();
}

void QuickClientTests::
    guiAutostartHonorsMinimalSystemHiddenOverride() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString higherPriority =
        QDir(testRoot.path()).filePath(QStringLiteral("system-high"));
    const QString lowerPriority =
        QDir(testRoot.path()).filePath(QStringLiteral("system-low"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(writeSystemAutostartEntry(lowerPriority));

    const QString hiddenEntryPath =
        guiAutostartEntryPath(higherPriority);
    QVERIFY(QDir().mkpath(QFileInfo(hiddenEntryPath).absolutePath()));
    QFile hiddenEntry(hiddenEntryPath);
    QVERIFY(hiddenEntry.open(
        QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray minimalOverride = QByteArrayLiteral(
        "[Desktop Entry]\n"
        "Hidden=true\n");
    QCOMPARE(
        hiddenEntry.write(minimalOverride),
        minimalOverride.size());
    hiddenEntry.close();

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(
        higherPriority + QDir::listSeparator() + lowerPriority));

    AppSettingsController settings(false);
    QVERIFY(settings.autostartAvailable());
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(settings.errorMessage().isEmpty());
    QVERIFY(!QFileInfo::exists(guiAutostartEntryPath(configHome)));
}

void QuickClientTests::
    guiAutostartHonorsCurrentDesktopForSystemEntries_data() {
    QTest::addColumn<QByteArray>("currentDesktop");
    QTest::addColumn<QByteArray>("visibilityKeys");
    QTest::addColumn<bool>("hidden");
    QTest::addColumn<bool>("expectedEnabled");

    QTest::newRow("only-show-in-matches-colon-list")
        << QByteArrayLiteral("GNOME: KDE ")
        << QByteArrayLiteral("OnlyShowIn=XFCE; KDE ;\n")
        << false << true;
    QTest::newRow("only-show-in-does-not-match")
        << QByteArrayLiteral("GNOME:KDE")
        << QByteArrayLiteral("OnlyShowIn=XFCE;LXQt;\n")
        << false << false;
    QTest::newRow("only-show-in-empty-current-desktop")
        << QByteArray{}
        << QByteArrayLiteral("OnlyShowIn=KDE;\n")
        << false << false;
    QTest::newRow("not-show-in-matches-whitespace-list")
        << QByteArrayLiteral("GNOME: KDE ")
        << QByteArrayLiteral("NotShowIn=XFCE; KDE ;\n")
        << false << false;
    QTest::newRow("not-show-in-does-not-match")
        << QByteArrayLiteral("GNOME:KDE")
        << QByteArrayLiteral("NotShowIn=XFCE;LXQt;\n")
        << false << true;
    QTest::newRow("not-show-in-empty-current-desktop")
        << QByteArray{}
        << QByteArrayLiteral("NotShowIn=KDE;\n")
        << false << true;
    QTest::newRow("hidden-masks-before-visibility-validation")
        << QByteArrayLiteral("KDE")
        << QByteArrayLiteral(
               "OnlyShowIn=KDE;\n"
               "NotShowIn=KDE;\n")
        << true << false;
}

void QuickClientTests::
    guiAutostartHonorsCurrentDesktopForSystemEntries() {
    QFETCH(QByteArray, currentDesktop);
    QFETCH(QByteArray, visibilityKeys);
    QFETCH(bool, hidden);
    QFETCH(bool, expectedEnabled);

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(writeSystemAutostartEntry(
        systemConfig, hidden, true, visibilityKeys));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    ScopedEnvironmentVariable currentDesktopEnvironment(
        "XDG_CURRENT_DESKTOP");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));
    currentDesktopEnvironment.set(currentDesktop);

    AppSettingsController settings(false);
    QVERIFY(settings.autostartAvailable());
    QCOMPARE(settings.autostartEnabled(), expectedEnabled);
    QVERIFY(settings.errorMessage().isEmpty());
}

void QuickClientTests::
    guiAutostartRejectsRelativeTryExecWithPathComponent() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString fakeBin =
        QDir(testRoot.path()).filePath(QStringLiteral("bin"));
    const QString nestedExecutable =
        QDir(fakeBin).filePath(QStringLiteral("subdir/tool"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(QFileInfo(nestedExecutable).absolutePath()));

    QFile executable(nestedExecutable);
    QVERIFY(executable.open(QIODevice::WriteOnly));
    QCOMPARE(executable.write("#!/bin/sh\nexit 0\n"), 17);
    executable.close();
    QVERIFY(QFile::setPermissions(
        nestedExecutable,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const QString entryPath = guiAutostartEntryPath(systemConfig);
    QVERIFY(QDir().mkpath(QFileInfo(entryPath).absolutePath()));
    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::WriteOnly));
    const QByteArray contents = QByteArrayLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=System TRYX Panorama Manager\n"
        "Exec=subdir/tool --autostart\n"
        "TryExec=subdir/tool\n"
        "Icon=tryx-panorama\n"
        "Terminal=false\n"
        "StartupNotify=false\n"
        "Hidden=false\n");
    QCOMPARE(entry.write(contents), contents.size());
    entry.close();

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    ScopedEnvironmentVariable pathEnvironment("PATH");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));
    pathEnvironment.set(QFile::encodeName(fakeBin));

    AppSettingsController settings(false);
    QVERIFY(settings.autostartAvailable());
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(settings.errorMessage().isEmpty());
}

void QuickClientTests::
    guiAutostartRejectsUnsafeSystemEntryWithoutBlocking_data() {
    QTest::addColumn<QString>("kind");

    QTest::newRow("fifo") << QStringLiteral("fifo");
    QTest::newRow("symlink") << QStringLiteral("symlink");
    QTest::newRow("oversized") << QStringLiteral("oversized");
}

void QuickClientTests::
    guiAutostartRejectsUnsafeSystemEntryWithoutBlocking() {
    QFETCH(QString, kind);

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString systemEntryPath =
        guiAutostartEntryPath(systemConfig);
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(QFileInfo(systemEntryPath).absolutePath()));
    const QByteArray encodedEntry = QFile::encodeName(systemEntryPath);
    if (kind == QStringLiteral("fifo")) {
        QCOMPARE(::mkfifo(encodedEntry.constData(), 0600), 0);
    } else if (kind == QStringLiteral("symlink")) {
        const QString externalPath =
            QDir(testRoot.path()).filePath(QStringLiteral("external.desktop"));
        QFile external(externalPath);
        QVERIFY(external.open(
            QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray sentinel = QByteArrayLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=External\n"
            "Exec=/usr/bin/true\n");
        QCOMPARE(external.write(sentinel), sentinel.size());
        external.close();
        QVERIFY(QFile::link(externalPath, systemEntryPath));
        QVERIFY(QFileInfo(systemEntryPath).isSymLink());
    } else {
        QFile oversized(systemEntryPath);
        QVERIFY(oversized.open(
            QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray contents(64 * 1024 + 1, '#');
        QCOMPARE(oversized.write(contents), contents.size());
        oversized.close();
    }

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    AppSettingsController settings(false);
    QVERIFY(!settings.autostartAvailable());
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(!settings.errorMessage().isEmpty());
    const QString userEntryPath = guiAutostartEntryPath(configHome);
    settings.setAutostartEnabled(true);
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(!QFileInfo::exists(userEntryPath));
    settings.setAutostartEnabled(false);
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(!QFileInfo::exists(userEntryPath));
    QVERIFY(QFile::remove(systemEntryPath));
}

void QuickClientTests::
    guiAutostartWriteFailureAndOfflineStayBounded() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString autostartDirectory =
        QDir(configHome).filePath(QStringLiteral("autostart"));
    const QString entryPath =
        guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(autostartDirectory));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    QVERIFY(QFile::setPermissions(
        autostartDirectory,
        QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    {
        AppSettingsController settings(false);
        QVERIFY(settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        QVERIFY(!QFileInfo::exists(entryPath));
    }
    QVERIFY(QFile::setPermissions(
        autostartDirectory,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    {
        AppSettingsController offlineSettings(true);
        QVERIFY(!offlineSettings.autostartAvailable());
        QVERIFY(!offlineSettings.autostartEnabled());
        offlineSettings.setAutostartEnabled(true);
        QVERIFY(!offlineSettings.autostartEnabled());
        QVERIFY(!offlineSettings.errorMessage().isEmpty());
        QVERIFY(!QFileInfo::exists(entryPath));
    }
}

void QuickClientTests::
    guiAutostartRejectsConcurrentInPlaceChanges() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(QFileInfo(entryPath).absolutePath()));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    const auto writeEntry = [&entryPath](const QByteArray &contents) {
        QFile entry(entryPath);
        if (!entry.open(
                QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return entry.write(contents) == contents.size();
    };

    const QByteArray removable =
        managedEntryWithSlowBlankLineParsing(false);
    QVERIFY(writeEntry(removable));
    const GuardedAutostartChange remove =
        changeAfterFirstEntryRead(entryPath, false);
    QVERIFY(remove.watcherInstalled);
    QVERIFY(remove.contentsMutated);
    QVERIFY(!remove.operationSucceeded);
    QVERIFY(!remove.error.isEmpty());
    QVERIFY(QFileInfo::exists(entryPath));
    QByteArray expectedRemovable = removable;
    expectedRemovable[0] = '\t';
    QFile unchangedAfterRemove(entryPath);
    QVERIFY(unchangedAfterRemove.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedAfterRemove.readAll(), expectedRemovable);
    unchangedAfterRemove.close();

    const QByteArray replaceable =
        managedEntryWithSlowBlankLineParsing(true);
    QVERIFY(writeEntry(replaceable));
    const GuardedAutostartChange replace =
        changeAfterFirstEntryRead(entryPath, true);
    QVERIFY(replace.watcherInstalled);
    QVERIFY(replace.contentsMutated);
    QVERIFY(!replace.operationSucceeded);
    QVERIFY(!replace.error.isEmpty());
    QByteArray expectedReplaceable = replaceable;
    expectedReplaceable[0] = '\t';
    QFile unchangedAfterReplace(entryPath);
    QVERIFY(unchangedAfterReplace.open(QIODevice::ReadOnly));
    QCOMPARE(unchangedAfterReplace.readAll(), expectedReplaceable);
}

void QuickClientTests::
    guiAutostartPreservesPostCheckRacedEntries() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    const QString autostartDirectory =
        QFileInfo(entryPath).absolutePath();
    QVERIFY(QDir().mkpath(autostartDirectory));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));
    ScopedGuiAutostartConfigOverride configOverride(configHome);

    const QByteArray managed = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false);
    const QByteArray foreign =
        QByteArrayLiteral("foreign raced desktop entry\n");
    const auto readEntry = [](const QString &path) {
        QFile entry(path);
        if (!entry.open(QIODevice::ReadOnly)) {
            return QByteArray{};
        }
        return entry.readAll();
    };
    const auto temporaryEntries = [&]() {
        return QDir(autostartDirectory).entryList(
            {QStringLiteral(
                 ".tryx-panorama-manager.desktop.tmp.*")},
            QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
            QDir::Name);
    };
    const auto runRace = [&](AutostartNamespaceRaceContext *context,
                             bool enabled) {
        QString error;
        gAutostartNamespaceRace = context;
        gui_autostart::testing::
            setBeforeLeafNamespaceMutationHook(
                &mutateAutostartNamespaceBefore);
        gui_autostart::testing::
            setAfterLeafNamespaceMutationHook(
                &mutateAutostartNamespaceAfter);
        gui_autostart::testing::
            setAfterLeafPreconditionCheckHook(
                &mutateAutostartNamespaceAfterPreconditionCheck);
        const bool succeeded =
            gui_autostart::setEnabled(enabled, &error);
        gui_autostart::testing::
            clearAfterLeafNamespaceMutationHook();
        gui_autostart::testing::
            clearAfterLeafPreconditionCheckHook();
        gui_autostart::testing::
            clearBeforeLeafNamespaceMutationHook();
        gAutostartNamespaceRace = nullptr;
        return qMakePair(succeeded, error);
    };

    QFile::remove(entryPath);
    AutostartNamespaceRaceContext preparedRace;
    preparedRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Create;
    preparedRace.action =
        AutostartNamespaceRaceAction::ReplacePreparedEntry;
    preparedRace.foreignContents = foreign;
    const auto preparedResult = runRace(&preparedRace, true);
    QVERIFY(preparedRace.triggered);
    QVERIFY(preparedRace.mutationSucceeded);
    QVERIFY(!preparedResult.first);
    QVERIFY(!preparedResult.second.isEmpty());
    QVERIFY(!QFileInfo::exists(entryPath));
    QCOMPARE(readEntry(preparedRace.mutatedPath), foreign);
    QVERIFY(QFile::remove(preparedRace.mutatedPath));
    QVERIFY(temporaryEntries().isEmpty());

    AutostartNamespaceRaceContext createRace;
    createRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Create;
    createRace.foreignContents = foreign;
    const auto createResult = runRace(&createRace, true);
    QVERIFY(createRace.triggered);
    QVERIFY(createRace.mutationSucceeded);
    QVERIFY(!createResult.first);
    QVERIFY(!createResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext precommitReplaceRace;
    precommitReplaceRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Replace;
    precommitReplaceRace.foreignContents = foreign;
    precommitReplaceRace.afterPreconditionCheck = true;
    const auto precommitReplaceResult =
        runRace(&precommitReplaceRace, true);
    QVERIFY(precommitReplaceRace.triggered);
    QVERIFY(precommitReplaceRace.mutationSucceeded);
    QVERIFY(!precommitReplaceResult.first);
    QVERIFY(!precommitReplaceResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext precommitRemoveRace;
    precommitRemoveRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Remove;
    precommitRemoveRace.foreignContents = foreign;
    precommitRemoveRace.afterPreconditionCheck = true;
    const auto precommitRemoveResult =
        runRace(&precommitRemoveRace, false);
    QVERIFY(precommitRemoveRace.triggered);
    QVERIFY(precommitRemoveRace.mutationSucceeded);
    QVERIFY(!precommitRemoveResult.first);
    QVERIFY(!precommitRemoveResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext lateReplaceRace;
    lateReplaceRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Replace;
    lateReplaceRace.action =
        AutostartNamespaceRaceAction::ReplacePreparedEntry;
    lateReplaceRace.foreignContents = foreign;
    lateReplaceRace.afterCommit = true;
    const auto lateReplaceResult =
        runRace(&lateReplaceRace, true);
    QVERIFY(lateReplaceRace.triggered);
    QVERIFY(lateReplaceRace.mutationSucceeded);
    QVERIFY(!lateReplaceResult.first);
    QVERIFY(!lateReplaceResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext lateRemoveRace;
    lateRemoveRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Remove;
    lateRemoveRace.foreignContents = foreign;
    lateRemoveRace.afterCommit = true;
    const auto lateRemoveResult = runRace(&lateRemoveRace, false);
    QVERIFY(lateRemoveRace.triggered);
    QVERIFY(lateRemoveRace.mutationSucceeded);
    QVERIFY(!lateRemoveResult.first);
    QVERIFY(!lateRemoveResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext replaceRace;
    replaceRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Replace;
    replaceRace.foreignContents = foreign;
    const auto replaceResult = runRace(&replaceRace, true);
    QVERIFY(replaceRace.triggered);
    QVERIFY(replaceRace.mutationSucceeded);
    QVERIFY(!replaceResult.first);
    QVERIFY(!replaceResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext removeRace;
    removeRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Remove;
    removeRace.foreignContents = foreign;
    const auto removeResult = runRace(&removeRace, false);
    QVERIFY(removeRace.triggered);
    QVERIFY(removeRace.mutationSucceeded);
    QVERIFY(!removeResult.first);
    QVERIFY(!removeResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QVERIFY(temporaryEntries().isEmpty());

    QVERIFY(QFile::remove(entryPath));
    QVERIFY(writeAutostartRaceEntry(entryPath, managed));
    AutostartNamespaceRaceContext directoryRace;
    directoryRace.expectedOperation =
        gui_autostart::testing::LeafNamespaceMutation::Replace;
    directoryRace.action =
        AutostartNamespaceRaceAction::ReplaceDirectory;
    directoryRace.foreignContents = foreign;
    const auto directoryResult = runRace(&directoryRace, true);
    QVERIFY(directoryRace.triggered);
    QVERIFY(directoryRace.mutationSucceeded);
    QVERIFY(!directoryResult.first);
    QVERIFY(!directoryResult.second.isEmpty());
    QCOMPARE(readEntry(entryPath), foreign);
    QCOMPARE(
        readEntry(QDir(directoryRace.movedDirectoryPath).filePath(
            QFileInfo(entryPath).fileName())),
        managed);
}

void QuickClientTests::
    guiAutostartRebindsManagedEntryToCurrentExecutable() {
    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(QFileInfo(entryPath).absolutePath()));
    QVERIFY(QDir().mkpath(systemConfig));

    ScopedEnvironmentVariable configHomeEnvironment("XDG_CONFIG_HOME");
    ScopedEnvironmentVariable configDirsEnvironment("XDG_CONFIG_DIRS");
    configHomeEnvironment.set(QFile::encodeName(configHome));
    configDirsEnvironment.set(QFile::encodeName(systemConfig));

    const QString previousExecutable =
        QStandardPaths::findExecutable(QStringLiteral("true"));
    QVERIFY(!previousExecutable.isEmpty());
    QVERIFY(previousExecutable !=
            QCoreApplication::applicationFilePath());
    const QByteArray previousEntry = managedAutostartEntry(
        previousExecutable, false);
    QVERIFY(writeAutostartRaceEntry(entryPath, previousEntry));

    AppSettingsController settings(false);
    QVERIFY(settings.autostartAvailable());
    QVERIFY(!settings.autostartEnabled());
    QVERIFY(settings.errorMessage().isEmpty());

    settings.setAutostartEnabled(true);
    QVERIFY(settings.autostartAvailable());
    QVERIFY(settings.autostartEnabled());
    QVERIFY(settings.errorMessage().isEmpty());

    QFile rebound(entryPath);
    QVERIFY(rebound.open(QIODevice::ReadOnly));
    QCOMPARE(
        rebound.readAll(),
        managedAutostartEntry(
            QCoreApplication::applicationFilePath(), false));
}

void QuickClientTests::
    guiAutostartRejectsLeafSwapToFifoWithoutBlocking() {
    constexpr auto probeEnvironment =
        "TRYX_GUI_AUTOSTART_FIFO_SWAP_PROBE";
    if (qEnvironmentVariableIsSet(probeEnvironment)) {
        gui_autostart::testing::setBeforeUserEntryOpenHook(
            [](const QString &path) {
                const QByteArray encoded = QFile::encodeName(path);
                QFile::remove(path);
                ::mkfifo(encoded.constData(), 0600);
            });
        AppSettingsController settings(false);
        gui_autostart::testing::clearBeforeUserEntryOpenHook();

        QVERIFY(!settings.autostartAvailable());
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        const QByteArray encoded = QFile::encodeName(
            guiAutostartEntryPath(
                qEnvironmentVariable("XDG_CONFIG_HOME")));
        struct stat status {};
        QCOMPARE(::lstat(encoded.constData(), &status), 0);
        QVERIFY(S_ISFIFO(status.st_mode));
        return;
    }

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString entryPath = guiAutostartEntryPath(configHome);
    QVERIFY(QDir().mkpath(QFileInfo(entryPath).absolutePath()));
    QVERIFY(QDir().mkpath(systemConfig));

    QFile entry(entryPath);
    QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray contents = managedAutostartEntry(
        QCoreApplication::applicationFilePath(), false);
    QCOMPARE(entry.write(contents), contents.size());
    entry.close();
    QVERIFY(QFile::setPermissions(
        entryPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("XDG_CONFIG_DIRS"), systemConfig);
    environment.insert(
        QString::fromLatin1(probeEnvironment), QStringLiteral("1"));

    QProcess probe;
    probe.setProcessEnvironment(environment);
    probe.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral(
             "guiAutostartRejectsLeafSwapToFifoWithoutBlocking"),
         QStringLiteral("-v1")});
    if (!probe.waitForFinished(3000)) {
        probe.kill();
        probe.waitForFinished(1000);
        QFAIL("GUI autostart query blocked after a FIFO leaf swap");
    }
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    const QByteArray output = probe.readAll();
    QVERIFY2(probe.exitCode() == 0, output.constData());

    const QByteArray encoded = QFile::encodeName(entryPath);
    struct stat status {};
    QCOMPARE(::lstat(encoded.constData(), &status), 0);
    QVERIFY(S_ISFIFO(status.st_mode));
}

void QuickClientTests::guiAutostartEscapesExecutablePath() {
    if (qEnvironmentVariableIsSet(
            "TRYX_GUI_AUTOSTART_ESCAPE_PROBE")) {
        AppSettingsController settings(false);
        QVERIFY(settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(settings.autostartEnabled());
        QVERIFY(settings.errorMessage().isEmpty());
        return;
    }

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString copiedExecutable =
        QDir(testRoot.path()).filePath(QStringLiteral(
            "TRYX manager $`%quote\"back\\slash"));
    QVERIFY(QFile::copy(
        QCoreApplication::applicationFilePath(),
        copiedExecutable));
    QVERIFY(QFile::setPermissions(
        copiedExecutable,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    const QString fakeBin =
        QDir(testRoot.path()).filePath(QStringLiteral("fake-bin"));
    const QString systemctlLog =
        QDir(testRoot.path()).filePath(QStringLiteral("systemctl.log"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(systemConfig));
    QVERIFY(QDir().mkpath(fakeBin));
    ScopedEnvironmentVariable logEnvironment(
        "TRYX_GUI_AUTOSTART_TEST_SYSTEMCTL_LOG");
    QVERIFY(createFakeSystemctl(fakeBin, systemctlLog));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("XDG_CONFIG_DIRS"), systemConfig);
    environment.insert(QStringLiteral("PATH"), fakeBin);
    environment.insert(
        QStringLiteral("TRYX_GUI_AUTOSTART_TEST_SYSTEMCTL_LOG"),
        systemctlLog);
    environment.insert(
        QStringLiteral("TRYX_GUI_AUTOSTART_ESCAPE_PROBE"),
        QStringLiteral("1"));

    QProcess probe;
    probe.setProcessEnvironment(environment);
    probe.start(
        copiedExecutable,
        {QStringLiteral("guiAutostartEscapesExecutablePath"),
         QStringLiteral("-v1")});
    QVERIFY2(
        probe.waitForFinished(10000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QVERIFY2(
        probe.exitCode() == 0,
        probe.readAll().constData());

    QFile entry(guiAutostartEntryPath(configHome));
    QVERIFY(entry.open(QIODevice::ReadOnly));
    const QByteArray contents = entry.readAll();
    const QByteArray expectedExec =
        QStringLiteral("Exec=%1 --autostart\n")
            .arg(expectedExecExecutable(copiedExecutable))
            .toUtf8();
    const QByteArray expectedTryExec =
        QStringLiteral("TryExec=%1\n")
            .arg(expectedDesktopString(copiedExecutable))
            .toUtf8();
    QVERIFY2(
        contents.contains(expectedExec),
        contents.constData());
    QVERIFY2(
        contents.contains(expectedTryExec),
        contents.constData());

    QFile log(systemctlLog);
    if (log.exists()) {
        QVERIFY(log.open(QIODevice::ReadOnly));
        QVERIFY(log.readAll().isEmpty());
    }
}

void QuickClientTests::
    guiAutostartRejectsEqualsInExecutablePath() {
    if (qEnvironmentVariableIsSet(
            "TRYX_GUI_AUTOSTART_EQUALS_PROBE")) {
        AppSettingsController settings(false);
        QVERIFY(settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        return;
    }

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString copiedExecutable =
        QDir(testRoot.path()).filePath(QStringLiteral(
            "TRYX=manager"));
    QVERIFY(QFile::copy(
        QCoreApplication::applicationFilePath(),
        copiedExecutable));
    QVERIFY(QFile::setPermissions(
        copiedExecutable,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(systemConfig));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("XDG_CONFIG_DIRS"), systemConfig);
    environment.insert(
        QStringLiteral("TRYX_GUI_AUTOSTART_EQUALS_PROBE"),
        QStringLiteral("1"));

    QProcess probe;
    probe.setProcessEnvironment(environment);
    probe.start(
        copiedExecutable,
        {QStringLiteral(
             "guiAutostartRejectsEqualsInExecutablePath"),
         QStringLiteral("-v1")});
    QVERIFY2(
        probe.waitForFinished(10000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    const QByteArray output = probe.readAll();
    QVERIFY2(probe.exitCode() == 0, output.constData());
    QVERIFY(!QFileInfo::exists(guiAutostartEntryPath(configHome)));
}

void QuickClientTests::
    guiAutostartRejectsControlInExecutablePath() {
    if (qEnvironmentVariableIsSet(
            "TRYX_GUI_AUTOSTART_CONTROL_PROBE")) {
        AppSettingsController settings(false);
        QVERIFY(settings.autostartAvailable());
        settings.setAutostartEnabled(true);
        QVERIFY(!settings.autostartEnabled());
        QVERIFY(!settings.errorMessage().isEmpty());
        return;
    }

    QTemporaryDir testRoot;
    QVERIFY(testRoot.isValid());
    const QString copiedExecutable =
        QDir(testRoot.path()).filePath(
            QStringLiteral("TRYX") + QChar(0x0007) +
            QStringLiteral("manager"));
    QVERIFY(QFile::copy(
        QCoreApplication::applicationFilePath(),
        copiedExecutable));
    QVERIFY(QFile::setPermissions(
        copiedExecutable,
        QFileDevice::ReadOwner |
            QFileDevice::WriteOwner |
            QFileDevice::ExeOwner));

    const QString configHome =
        QDir(testRoot.path()).filePath(QStringLiteral("config"));
    const QString systemConfig =
        QDir(testRoot.path()).filePath(QStringLiteral("system-config"));
    QVERIFY(QDir().mkpath(configHome));
    QVERIFY(QDir().mkpath(systemConfig));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), configHome);
    environment.insert(QStringLiteral("XDG_CONFIG_DIRS"), systemConfig);
    environment.insert(
        QStringLiteral("TRYX_GUI_AUTOSTART_CONTROL_PROBE"),
        QStringLiteral("1"));

    QProcess probe;
    probe.setProcessEnvironment(environment);
    probe.start(
        copiedExecutable,
        {QStringLiteral(
             "guiAutostartRejectsControlInExecutablePath"),
         QStringLiteral("-v1")});
    QVERIFY2(
        probe.waitForFinished(10000),
        qPrintable(probe.errorString()));
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    const QByteArray output = probe.readAll();
    QVERIFY2(probe.exitCode() == 0, output.constData());
    QVERIFY(!QFileInfo::exists(guiAutostartEntryPath(configHome)));
}

void QuickClientTests::
    windowChromeRejectsOperationsWithoutWindow() {
    WindowChromeController chrome;
    QVERIFY(!chrome.ready());
    QVERIFY(!chrome.maximized());
    QVERIFY(!chrome.trayAvailable());
    QVERIFY(chrome.hideToTrayOnClose());
    QVERIFY(!chrome.hiddenToTray());
    QVERIFY(!chrome.startMove());
    QVERIFY(!chrome.startResize(Qt::LeftEdge));
    QVERIFY(!chrome.startResize(
        Qt::LeftEdge | Qt::RightEdge));
    QVERIFY(!chrome.handleCloseRequest());
    chrome.setTrayAvailable(true);
    QVERIFY(chrome.trayAvailable());
    QVERIFY(!chrome.handleCloseRequest());
    QVERIFY(!chrome.hiddenToTray());
    chrome.showWindow();
    chrome.setTrayAvailable(false);
    QVERIFY(!chrome.trayAvailable());
    chrome.minimize();
    chrome.toggleMaximized();
    chrome.closeWindow();
}

void QuickClientTests::
    windowChromeHidesAndRestoresOnlyWithTray() {
    QWindow window;
    window.resize(640, 480);
    window.show();
    QTRY_VERIFY(window.isVisible());

    WindowChromeController chrome;
    chrome.setWindow(&window);
    QVERIFY(chrome.ready());
    QVERIFY(!chrome.handleCloseRequest());
    QVERIFY(window.isVisible());

    chrome.setTrayAvailable(true);
    chrome.setHideToTrayOnClose(false);
    QVERIFY(!chrome.hideToTrayOnClose());
    QVERIFY(!chrome.handleCloseRequest());
    QVERIFY(window.isVisible());

    chrome.setHideToTrayOnClose(true);
    QVERIFY(chrome.hideToTrayOnClose());
    QVERIFY(chrome.handleCloseRequest());
    QVERIFY(chrome.hiddenToTray());
    QVERIFY(!window.isVisible());

    chrome.showWindow();
    QTRY_VERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());

    QVERIFY(chrome.handleCloseRequest());
    QVERIFY(chrome.hiddenToTray());
    chrome.setHideToTrayOnClose(false);
    QTRY_VERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());

    chrome.setHideToTrayOnClose(true);
    QVERIFY(chrome.handleCloseRequest());
    QVERIFY(chrome.hiddenToTray());
    chrome.setTrayAvailable(false);
    QTRY_VERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());
}

void QuickClientTests::
    windowChromeAutostartHidesOnlyWithPolicyAndTray() {
    QWindow window;
    window.resize(640, 480);
    window.show();
    QTRY_VERIFY(window.isVisible());

    WindowChromeController chrome;
    chrome.setWindow(&window);
    QVERIFY(!chrome.hideWindowToTray());
    QVERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());

    chrome.setTrayAvailable(true);
    chrome.setHideToTrayOnClose(false);
    QVERIFY(!chrome.hideWindowToTray());
    QVERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());

    chrome.setHideToTrayOnClose(true);
    QVERIFY(chrome.hideWindowToTray());
    QVERIFY(!window.isVisible());
    QVERIFY(chrome.hiddenToTray());

    chrome.setTrayAvailable(false);
    QTRY_VERIFY(window.isVisible());
    QVERIFY(!chrome.hiddenToTray());
}

int main(int argc, char **argv) {
    if (argc > 1 &&
        qstrcmp(argv[1], "--internal-stage-copy") == 0) {
        QStringList helperArguments;
        for (int index = 2; index < argc; ++index) {
            helperArguments.append(
                QString::fromLocal8Bit(argv[index]));
        }
        return MediaPreviewController::runStageCopyHelper(
            helperArguments);
    }

    QTemporaryDir isolatedRuntime(
        QDir(QDir::tempPath()).filePath(
            QStringLiteral(
                "tryx-quick-tests-runtime-XXXXXX")));
    if (!isolatedRuntime.isValid() ||
        !QFile::setPermissions(
            isolatedRuntime.path(),
            QFileDevice::ReadOwner |
                QFileDevice::WriteOwner |
                QFileDevice::ExeOwner)) {
        return 2;
    }
    qputenv(
        "XDG_RUNTIME_DIR",
        QFile::encodeName(isolatedRuntime.path()));

    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    QuickClientTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "tst_quickmodels.moc"
