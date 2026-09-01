#pragma once

#include <QString>
#include <QByteArray>
#include <QStringList>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPair>

#include <functional>
#include <memory>
#include <optional>

class QSocketNotifier;
struct udev;
struct udev_monitor;
namespace panorama {
namespace wire {
namespace v1 {
class UserConfiguration;
}
}
}

enum class PrinterIdleMode {
    OverlayLayout,
    TransferOnly
};

struct PrinterProductProfile {
    quint16 productId = 0;
    int mediaWidth = 0;
    int mediaHeight = 0;
    PrinterIdleMode idleMode = PrinterIdleMode::TransferOnly;
    bool mediaUploadSupported = false;
    bool mediaCatalogSupported = false;
    bool displayConfigurationSupported = false;
    bool splitAreaMediaSupported = false;
    bool overlayMetricsSupported = false;
    bool firmwareFlashSupported = false;
};

std::optional<PrinterProductProfile> printerProductProfileForId(
    quint16 productId);
QString printerProductIdString(quint16 productId);

class PrinterFrameCodec {
public:
    static constexpr qsizetype MaxPayloadSize = 1024 * 1024;

    enum class DecodeStatus {
        NeedMoreData,
        FrameReady,
        Malformed
    };

    static QByteArray encode(const QByteArray &payload);
    static DecodeStatus takeFrame(QByteArray *buffer, QByteArray *payload,
                                  QString *errorMessage = nullptr);
};

class PrinterProtocol {
public:
    enum class DiscoveryState {
        Absent,
        RockchipGadget391a0006,
        EnumeratingPrinterClass,
        Enumerating391a1021 = EnumeratingPrinterClass,
        Ready,
        PermissionDenied,
        Ambiguous,
        MonitoringUnavailable
    };

    struct UsbPrinterDevice {
        QString devicePath;
        QString sysfsPath;
        quint16 productId = 0;
        QString manufacturer;
        QString product;
        QString serial;
        bool accessible = false;

        bool operator==(const UsbPrinterDevice &other) const;
    };

    struct DiscoverySnapshot {
        DiscoveryState state = DiscoveryState::Absent;
        QList<UsbPrinterDevice> devices;
        int rockchipGadgetDeviceCount = 0;
        int workingUsbDeviceCount = 0;

        bool operator==(const DiscoverySnapshot &other) const;
        bool blocksLegacyTransport() const;
        QString statusText() const;
    };

    struct DeviceInfo {
        QString devicePath;
        QString manufacturer;
        QString usbProduct;
        QString usbSerial;
        QString osName;
        QString osVersion;
        QString firmwareVersion;
        QString productName;
        QString appVersion;
        QString serialNumber;
        QString chipId;
        bool serialNumberLocked = false;
    };

    struct DeviceSpecifications {
        bool valid = false;
        QString reportedProductName;
        quint32 videoOutputWidth = 0;
        quint32 videoOutputHeight = 0;
        QString screenType;
        bool usbAutoKeepalive = false;
    };

    struct Result {
        bool success = false;
        QString error;
        DeviceInfo deviceInfo;
        DeviceSpecifications deviceSpecifications;
    };

    enum class MediaSource {
        User,
        Preset
    };

    struct MediaFile {
        QString name;
        quint32 size = 0;
        bool readOnly = false;
        MediaSource source = MediaSource::User;
    };

    struct MediaListResult {
        bool success = false;
        QString error;
        QList<MediaFile> files;
    };

    struct MediaPullResult {
        bool success = false;
        bool cancelled = false;
        QString error;
        QString mediaName;
        qint64 fileSize = 0;
        qint64 bytesDecoded = 0;
        int chunkCount = 0;
        QString rawSha256;
        QString decodedSha256;
    };

    struct MediaReferenceResult {
        bool success = false;
        bool originalIdentityVerified = false;
        bool replacementIdentityVerified = false;
        QString error;
        MediaFile media;
        // Fixed order: PowerOn, Standby, Single, DualLeft, DualRight,
        // Kaleidoscope, FilterSingle, FilterDualLeft, FilterDualRight.
        QStringList references;
        QStringList referencingSlots;
        QString activeScreenMode;
        QString activePlayMode;
        QStringList activeMedia;
    };

    struct ReadinessRetryInfo {
        int attempt = 0;
        qint64 expectedBytes = 0;
        qint64 actualBytes = 0;
        int elapsedMs = 0;
        int backoffMs = 0;
        QString transferStatus;
    };

    struct OperationContext {
        int cancellationFd = -1;
        std::function<bool()> isCancelled;
        bool maintainKeepalive = false;
        std::function<void()> onDeviceInfoReady;
        std::function<void(const ReadinessRetryInfo &)>
            onReadinessProbeRetry;
    };

    enum class KeepaliveOutcome {
        Sent,
        RetryableFailure,
        FatalFailure
    };

    enum class MutationOutcome {
        NotStarted,
        Rejected,
        VerificationFailed,
        Succeeded,
        Cancelled,
        FinalizationUnknown,
        PartialOrUnknown
    };

    struct MutationDetails {
        MutationOutcome outcome = MutationOutcome::NotStarted;
        QString stage;
        qint64 bytesSent = 0;
        qint64 totalBytes = 0;
    };

    struct DeleteResult {
        bool success = false;
        MutationOutcome outcome = MutationOutcome::NotStarted;
        bool commandAcknowledged = false;
        QString error;
        QString currentName;
        QStringList deletedNames;
        QList<MediaFile> files;
    };

    struct PaseOverlayAreaConfig {
        QStringList metrics;
        QStringList badges;
        QString alignment = QStringLiteral("Left");
        quint32 textColor = 0x00DCDCDC;
        QString verticalPlacement = QStringLiteral("Top");
        QStringList initialLabels;
        QStringList initialValues;
        QStringList initialUnits;
    };

    struct PaseOverlayConfig {
        PaseOverlayAreaConfig left;
        PaseOverlayAreaConfig right;
        bool dualMode = false;
        bool waterfallMode = false;
        QString cpuBadgeText;
        QString gpuBadgeText;
        QString temperatureUnit = QStringLiteral("Celsius");
        QString timeFormat = QStringLiteral("24H");
    };

    struct PaseDisplayMutation {
        bool brightnessPresent = false;
        int brightness = 0;
        bool standbyPresent = false;
        bool standbyEnabled = false;
        bool backlightPresent = false;
        bool backlightEnabled = true;
        bool orientationPresent = false;
        bool mirrorMode = false;
        bool waterfallMode = false;
    };

    struct PaseApplyConfig {
        QStringList media;
        QString screenMode;
        QString playMode;
        bool mediaPresent = false;
        bool replaceOverlay = false;
        PaseDisplayMutation display;
        PaseOverlayConfig overlay;
    };

    struct PaseDisplayState {
        bool backlightEnabled = false;
        int brightness = 0;
        bool standbyEnabled = false;
        QString standbyMedia;
        bool mirrorMode = false;
        bool waterfallMode = false;
        QString screenMode = QStringLiteral("Full Screen");
        QString playMode = QStringLiteral("Single");
        QStringList media;
    };

    struct PaseDisplayStateResult {
        bool success = false;
        QString error;
        PaseDisplayState state;
    };

    using UploadProgress = std::function<void(qint64 bytesSent, qint64 totalBytes)>;
    using MediaPullChunkSink = std::function<bool(
        qint64 offset, const QByteArray &decodedChunk,
        QString *errorMessage)>;
    using MediaPullProgress = std::function<void(
        qint64 bytesDecoded, qint64 totalBytes)>;
    using DeleteProgress = std::function<void(
        const QString &stage, const QString &fileName,
        int completedFiles, int totalFiles)>;
    using BeforeDeleteDispatch = std::function<bool(
        int index, const MediaFile &media, QString *errorMessage)>;

    PrinterProtocol();
    explicit PrinterProtocol(int transactionTimeoutMs);
    PrinterProtocol(int transactionTimeoutMs, int deviceInfoReadyTimeoutMs);
    explicit PrinterProtocol(const PrinterProductProfile &productProfile);
    PrinterProtocol(const PrinterProductProfile &productProfile,
                    int transactionTimeoutMs);
    PrinterProtocol(const PrinterProductProfile &productProfile,
                    int transactionTimeoutMs,
                    int deviceInfoReadyTimeoutMs);
    ~PrinterProtocol();

    PrinterProtocol(const PrinterProtocol &) = delete;
    PrinterProtocol &operator=(const PrinterProtocol &) = delete;

    void close();
    const PrinterProductProfile &productProfile() const;
    bool persistentUsbInputFailure() const;
    Result startDisplaySession(
        const QString &devicePath, const OperationContext &context);
    Result readDeviceInfo(const QString &devicePath,
                          const OperationContext &context);
    MediaListResult readMediaList(const QString &devicePath,
                                  const OperationContext &context);
    MediaPullResult pullUserMedia(
        const QString &devicePath, const QString &mediaName,
        qint64 expectedSize, const MediaPullChunkSink &sink,
        const MediaPullProgress &progress,
        const OperationContext &context);
    MediaReferenceResult readUserMediaReferences(
        const QString &devicePath, const QString &mediaName,
        qint64 expectedSize,
        const QString &expectedReplacementName,
        qint64 expectedReplacementSize,
        const OperationContext &context);
    DeleteResult removeUserMedia(
        const QString &devicePath, const QStringList &fileNames,
        const BeforeDeleteDispatch &beforeDispatch,
        const DeleteProgress &progress,
        const OperationContext &context,
        bool reconcileOnly = false,
        qint64 expectedSingleSize = 0,
        const QString &expectedReplacementName = QString(),
        qint64 expectedReplacementSize = 0);
    bool uploadMedia(const QString &devicePath, const QString &localPath,
                     const QString &remoteFileName, QString *uploadedName,
                     QString *errorMessage, const UploadProgress &progress,
                     const OperationContext &context,
                     MutationDetails *mutationDetails = nullptr,
                     const QString &expectedSha256 = QString());
    bool applyPresetMedia(const QString &devicePath, const QString &mediaFile,
                          int brightness, QString *errorMessage,
                          const OperationContext &context,
                          MutationDetails *mutationDetails = nullptr);
    bool applyPresetMediaWithOverlay(
        const QString &devicePath, const QString &mediaFile, int brightness,
        const PaseOverlayConfig &overlay, QString *errorMessage,
        const OperationContext &context,
        MutationDetails *mutationDetails = nullptr);
    bool applyPaseConfiguration(
        const QString &devicePath, const PaseApplyConfig &config,
        QString *errorMessage, const OperationContext &context,
        MutationDetails *mutationDetails = nullptr,
        PaseDisplayState *appliedState = nullptr);
    PaseDisplayStateResult readPaseDisplayState(
        const QString &devicePath, const OperationContext &context);
    bool configurePaseOverlay(
        const QString &devicePath, const PaseOverlayConfig &overlay,
        QString *errorMessage, const OperationContext &context,
        MutationDetails *mutationDetails = nullptr);
    bool sendPaseMetricBatch(
        const QString &devicePath, const PaseOverlayConfig &overlay,
        const QStringList &labels, const QStringList &values,
        const QStringList &units, QString *errorMessage,
        const OperationContext &context);
    bool setBrightness(const QString &devicePath, int brightness,
                       QString *errorMessage,
                       const OperationContext &context);
    KeepaliveOutcome sendKeepalive(const QString &devicePath,
                                   QString *errorMessage,
                                   const OperationContext &context);
    KeepaliveOutcome sendDisplayKeepalive(
        const QString &devicePath, QString *errorMessage,
        const OperationContext &context,
        const PaseOverlayConfig *overlay = nullptr);
    int millisecondsUntilKeepalive() const;

    static DiscoverySnapshot discover(const QString &sysfsRoot = QStringLiteral("/sys"),
                                      const QString &devRoot = QStringLiteral("/dev"));
    static QStringList devicePaths(const DiscoverySnapshot &snapshot);
    static bool isSafeUploadMediaName(const QString &fileName);
#ifdef TRYX_PROTOCOL_TESTING
    enum class DuplexTestDirection {
        Input,
        Output
    };

    enum class DuplexTestStatus {
        Completed,
        Error,
        TimedOut,
        Cancelled,
        Stall,
        NoDevice,
        Overflow
    };

    struct DuplexTestEvent {
        DuplexTestDirection direction = DuplexTestDirection::Input;
        DuplexTestStatus status = DuplexTestStatus::Completed;
        QByteArray payload;
        int actualLength = -1;
        int deferredDispatches = 0;
    };

    struct DuplexTestResult {
        bool writeSucceeded = false;
        bool responseReceived = false;
        QString error;
        QByteArray response;
        int inputSubmissions = 0;
        int outputSubmissions = 0;
        int maximumConcurrentInputs = 0;
        int inputCompletions = 0;
        int zeroLengthInputCompletions = 0;
        int inputErrors = 0;
        int inputRearmsDuringOutput = 0;
        bool persistentInputFailure = false;
    };

    bool trackedPingForTesting(const QString &devicePath, QString *payload,
                               QString *errorMessage,
                               const OperationContext &context);
    void adoptFileDescriptorForTesting(int fd, const QString &devicePath);
    void setUnframedRecoveryEligibleForTesting(bool eligible);
    void setFileTransmitDataWriteTimeoutForTesting(int timeoutMs);
    void setFileTransmitResponseTimeoutForTesting(int timeoutMs);
    void setMediaPullLimitsForTesting(
        qint64 maximumBytes, int maximumChunks, int deadlineMs);
    void setPersistentUsbInputFailureForTesting(bool persistent);
    void setBootstrapZeroByteWriteFailuresForTesting(int failureCount);
    QList<qint64> bootstrapReadinessAttemptOffsetsForTesting() const;
    bool sendPaseRunConfigForTesting(
        const QString &devicePath, const PaseOverlayConfig &overlay,
        QString *errorMessage, const OperationContext &context);
    static bool validateEndpointForTesting(const QString &devicePath, int openFd,
                                           const QString &sysfsRoot,
                                           const QString &devRoot,
                                           QString *errorMessage);
    static bool validateEndpointForTesting(const QString &devicePath, int openFd,
                                           const QString &sysfsRoot,
                                           const QString &devRoot,
                                           quint16 expectedProductId,
                                           QString *errorMessage);
    static DuplexTestResult runDuplexTransportScenarioForTesting(
        const QList<DuplexTestEvent> &events,
        const QByteArray &request,
        int writeTimeoutMs = 100,
        int readTimeoutMs = 100);
    static QByteArray applyMediaPullXorForTesting(
        const QByteArray &bytes, quint64 absoluteOffset);
    static bool validateMediaPullPathForTesting(
        const QByteArray &rawPath, const QString &mediaName);
#endif

private:
    bool sendUserConfigWithOutcome(
        const QString &devicePath,
        const panorama::wire::v1::UserConfiguration &userConfig,
        QString *errorMessage,
        const OperationContext &context,
        MutationDetails *mutationDetails = nullptr);
    bool activateAcceptedConfig(const QString &devicePath,
                                QString *errorMessage,
                                const OperationContext &context,
                                MutationDetails *mutationDetails = nullptr,
                                const PaseOverlayConfig *overlay = nullptr,
                                bool *activationRejected = nullptr);
    bool sendRunConfigTrigger(const QString &devicePath, QString *errorMessage,
                              const OperationContext &context,
                              const PaseOverlayConfig *overlay = nullptr,
                              MutationDetails *mutationDetails = nullptr);

    class Impl;
    const PrinterProductProfile productProfile_;
    std::unique_ptr<Impl> impl_;
};

Q_DECLARE_METATYPE(PrinterProtocol::UsbPrinterDevice)
Q_DECLARE_METATYPE(PrinterProtocol::DiscoverySnapshot)
Q_DECLARE_METATYPE(PrinterProtocol::DeviceInfo)
Q_DECLARE_METATYPE(PrinterProtocol::DeviceSpecifications)
Q_DECLARE_METATYPE(PrinterProtocol::MediaFile)
Q_DECLARE_METATYPE(PrinterProtocol::MutationOutcome)
Q_DECLARE_METATYPE(PrinterProtocol::PaseOverlayAreaConfig)
Q_DECLARE_METATYPE(PrinterProtocol::PaseOverlayConfig)
Q_DECLARE_METATYPE(PrinterProtocol::PaseDisplayState)

class PrinterDeviceMonitor : public QObject {
    Q_OBJECT

public:
    explicit PrinterDeviceMonitor(QObject *parent = nullptr);
    ~PrinterDeviceMonitor() override;

    bool start();
    PrinterProtocol::DiscoverySnapshot snapshot() const;

#ifdef TRYX_PROTOCOL_TESTING
    void forceStartFailureForTesting();
    void setDiscoveryRootsForTesting(const QString &sysfsRoot,
                                     const QString &devRoot);
    void rescanForTesting(bool currentEndpointEvent);
    void injectUdevEventForTesting(const QByteArray &subsystem,
                                   const QString &syspath,
                                   const QString &sysname);
    static QPair<bool, bool> eventPolicyForTesting(
        const QByteArray &subsystem, const QByteArray &action,
        const QByteArray &product,
        bool touchesCurrentEndpoint = false);
#endif

signals:
    void snapshotChanged(const PrinterProtocol::DiscoverySnapshot &snapshot);
    void currentEndpointRemoved();
    void monitorError(const QString &message);

private slots:
    void drainEvents();

private:
#ifdef TRYX_PROTOCOL_TESTING
    friend class PrinterProtocolTests;
#endif
    bool eventTouchesCurrentEndpoint(const QByteArray &subsystem,
                                     const QString &eventPath,
                                     const QString &eventName) const;
    void finishEventBatch(bool relevantEvent, bool currentEndpointEvent);
    void rescan(bool forceSignal = false);
    void stop();

    udev *udev_ = nullptr;
    udev_monitor *monitor_ = nullptr;
    QSocketNotifier *notifier_ = nullptr;
    QString sysfsRoot_ = QStringLiteral("/sys");
    QString devRoot_ = QStringLiteral("/dev");
    PrinterProtocol::DiscoverySnapshot snapshot_;
    bool hasSnapshot_ = false;
#ifdef TRYX_PROTOCOL_TESTING
    bool forceStartFailureForTesting_ = false;
#endif
};
