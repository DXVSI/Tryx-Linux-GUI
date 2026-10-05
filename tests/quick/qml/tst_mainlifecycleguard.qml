pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtTest

import "../../../qml" as App

TestCase {
    id: testCase
    name: "MainLifecycleGuard"
    when: windowShown
    width: 1100
    height: 760

    ListModel {
        id: mediaModel
        ListElement {
            mediaName: "one.mp4.h264_2240x1080"
            mediaId: "media-one"
            mediaSize: 1024
            mediaSource: 1
            managedOrigin: true
            readOnly: false
            thumbnailUrl: ""
            deleteAllowed: true
            deleteBlockReason: ""
            deviceCopyAllowed: true
            deviceCopyBlockReason: ""
        }
        function canDelete(mediaName) {
            return mediaName.length > 0
        }
        function deleteBlockReason(mediaName) {
            return ""
        }
    }

    ListModel {
        id: operationModel
    }

    QtObject {
        id: runtimeMock

        property bool serviceAvailable: true
        property bool compatible: true
        property bool legacyConnected: false
        property bool printerClassDevicePresent: true
        property bool displaySessionActive: true
        property bool displayStateValid: true
        property bool operationBusy: false
        property bool backlightEnabled: true
        property bool mirrorMode: false
        property bool waterfallMode: false
        property bool metricsEnabled: false
        property bool samplingActive: false
        property bool presentationPreferencesReady: true
        property bool presentationPreferencesBusy: false
        property string temperatureUnit: "Celsius"
        property string timeFormat: "24H"
        property int brightness: 75
        property string connectionStatus: "Ready"
        property string deviceModel: "PANORAMA SE"
        property string productId: "391a:1021"
        property string firmwareVersion: "PASE-FW-1"
        property string deviceAppVersion: "PASE-APP-1"
        property bool deviceSpecificationsSupported: false
        property string deviceSpecificationsStatus: "NotSupported"
        property bool deviceSpecificationsReady: false
        property string deviceReportedProductName: ""
        property int deviceVideoOutputWidth: 0
        property int deviceVideoOutputHeight: 0
        property string deviceScreenType: ""
        property bool deviceUsbAutoKeepalive: false
        property int mediaTargetWidth: 2240
        property int mediaTargetHeight: 1080
        property string diagnostic: ""
        property string activeOperationId: ""
        property string operationSummary: ""
        property real operationProgress: 0
        property string currentScreenMode: "Full Screen"
        property string currentPlayMode: "Single"
        property string displayDeviceIdentity: "device-a"
        property var displayedMedia: [
            "one.mp4.h264_2240x1080"
        ]
        property var displayLeftMetrics: []
        property var displayRightMetrics: []
        property var displayLeftBadges: []
        property var displayRightBadges: []
        property string displayLeftPosition: "Top"
        property string displayLeftColor: "#112233"
        property string displayLeftAlignment: "Left"
        property string displayRightPosition: "Bottom"
        property string displayRightColor: "#445566"
        property string displayRightAlignment: "Right"
        property var availableMetrics: [
            "CPU Temperature", "GPU Temperature"
        ]
        property bool metricsCatalogReady: true
        property var metricsCatalog: [
            "CPU Temperature", "CPU Frequency", "CPU Usage", "CPU Power",
            "GPU Temperature", "GPU Frequency", "GPU Usage", "GPU Power",
            "Memory Frequency", "Memory Usage", "Date&Time"
        ]
        property var activeMetrics: []
        property bool capabilitiesReady: false
        property bool savedLayoutsSupported: false
        property bool savedLayoutsReady: false
        property bool savedLayoutsBusy: false
        property string savedLayoutsStatus: "NotSupported"
        property string savedLayoutsDiagnostic: ""
        property string savedLayoutsDeviceIdentity: ""
        property var savedLayoutModel: []
        property string metricsAlignment: "Left"
        property string metricsColor: "#dcdcdc"
        property var mediaModel: mediaModel
        property var operationModel: operationModel
        property int fullSubmitCount: 0
        property int splitSubmitCount: 0
        property string lastSubmissionId: ""

        signal displayChanged()
        signal connectionChanged()
        signal runtimeInvalidated()
        signal savedLayoutsChanged()
        signal savedLayoutPutFinished(
            string requestedLayoutId, string savedLayoutId,
            string revisionDecimal, bool success, string message)
        signal savedLayoutDeleteFinished(
            string layoutId, bool success, string message)
        signal metricsChanged()
        signal displayApplyFinished(string submissionId,
                                    string outcome,
                                    string message)
        signal userMessage(string message, bool isError)

        function refreshAll() {}
        function refreshMedia() {}
        function deleteMedia(media) {}
        function configureMetrics(enabled, metrics,
                                  alignment, color) {}
        function formatTemperature(available, celsius) {
            return available ? Number(celsius).toFixed(0) + " °C" : "—"
        }
        function setPresentationPreferences(temperature, time) {
            temperatureUnit = temperature
            timeFormat = time
        }
        function setBrightness(value) {}
        function setBacklight(enabled) {}
        function setOrientation(mirror, waterfall) {}
        function savedLayoutIdForName(name) { return "" }
        function savedLayoutDraft(layoutId) { return ({}) }
        function putSavedLayout(name, overwriteLayoutId, fullState) {}
        function deleteSavedLayout(layoutId) {}
        function submitSavedLayoutDraft(
                layoutId, revisionDecimal, fullState) { return "" }
        function retryOperation(operationId) {}
        function cancelActiveOperation() {}
        function connectDevice(port) {}
        function disconnectDevice() {}
        function startKeepalive(interval) {}
        function submitFullDisplayDraft(
                media, playMode, metrics, badges,
                position, color, alignment,
                layoutPresent,
                brightnessPresent, brightness,
                orientationPresent, mirror, waterfall) {
            fullSubmitCount += 1
            lastSubmissionId = "display-submission-" +
                               fullSubmitCount
            return lastSubmissionId
        }
        function submitSplitDisplayDraft(
                left, right, playMode,
                leftMetrics, rightMetrics,
                leftBadges, rightBadges,
                leftPosition, leftColor, leftAlignment,
                rightPosition, rightColor, rightAlignment,
                layoutPresent,
                brightnessPresent, brightness,
                orientationPresent, mirror, waterfall) {
            splitSubmitCount += 1
            lastSubmissionId = "display-split-submission-" +
                               splitSubmitCount
            return lastSubmissionId
        }
        function abandonDisplaySubmission(submissionId) {}
    }

    QtObject {
        id: metricsMock
        property bool sampled: true
        property string cpuName: "Test CPU"
        property real cpuUsage: 25
        property bool cpuUsageAvailable: true
        property real cpuTemperature: 50
        property bool cpuTemperatureAvailable: true
        property real cpuFrequencyMHz: 4200
        property bool cpuFrequencyAvailable: true
        property string gpuName: "Test GPU"
        property real gpuUsage: 35
        property bool gpuUsageAvailable: true
        property real gpuTemperature: 60
        property bool gpuTemperatureAvailable: true
        property real gpuFrequencyMHz: 2300
        property bool gpuFrequencyAvailable: true
        property real gpuPowerWatts: 0
        property bool gpuPowerAvailable: true
        property int gpuVramUsedMB: 0
        property int gpuVramTotalMB: 8192
        property bool gpuVramAvailable: true
        property real ramUsage: 45
        property bool ramUsageAvailable: true
        property int ramUsedMB: 16384
        property int ramTotalMB: 32768
        property real diskUsage: 55
        property bool diskUsageAvailable: true
        property int diskUsedGB: 550
        property int diskTotalGB: 1000
        property bool networkAvailable: true
        property real rxSpeedKBs: 2048
        property real txSpeedKBs: 512
        property bool dashboardActive: false
        function refresh() {}
    }

    QtObject {
        id: editorMock
        property bool open: false
        property bool busy: false
        property bool submissionPending: false
        property bool ready: true
        property string sourceName: "example.mp4"
        property string sourceKind: "LocalMedia"
        property bool recoveredDeviceCopy: false
        property string originalMediaName: ""
        property bool replaceAllowed: false
        property string replaceBlockReason: ""
        property string submissionAction: ""
        property url previewUrl: ""
        property string error: ""
        property string mode: "Fit"
        property int zoomPercent: 100
        property int focusX: 5000
        property int focusY: 5000
        property int rotation: 0
        property string backgroundColor: "#000000"
        property string preparationTarget: "FullFrame"
        property bool splitTargetAvailable: false
        property int targetWidth: 2240
        property int targetHeight: 1080
        property string deviceCopyMetadataStatus: "NotSupported"
        property bool deviceCopyDimensionsAvailable: false
        property int deviceCopyWidth: 0
        property int deviceCopyHeight: 0
        property bool deviceCopyDurationAvailable: false
        property double deviceCopyDurationMilliseconds: 0
        property bool deviceCopyFrameRateAvailable: false
        property int deviceCopyFrameRateNumerator: 0
        property int deviceCopyFrameRateDenominator: 0
        property url homeFolder: Qt.resolvedUrl(".")
        function begin(url) {}
        function beginDropped(urls) {}
        function reset() {}
        function cancel() {}
        function submit() {}
        function submitSaveAsNew() {}
        function submitReplace() {}
    }

    QtObject {
        id: deviceMediaMock
        property bool busy: false
        property bool overwriteConfirmationPending: false
        property string overwriteFileName: ""
        signal stateChanged()
        signal userMessage(string message, bool error)
        function beginEdit(mediaId, mediaName) {}
        function beginExport(mediaId, mediaName,
                             folder, fileName) {}
        function confirmOverwrite() {}
        function cancelOverwrite() {}
        function suggestedExportFileName(mediaName) {
            return "device-copy.h264"
        }
    }

    QtObject {
        id: settingsMock
        property string language: "en"
        property bool hideToTrayOnClose: true
        property string devicePort: ""
        property int keepaliveInterval: 10
        property var serialPorts: []
        property bool autostartEnabled: false
        property bool autostartAvailable: true
        property bool busy: false
        property string errorMessage: ""
        function setLanguage(code) { language = code }
        function setHideToTrayOnClose(enabled) {
            hideToTrayOnClose = enabled
        }
        function setDevicePort(port) { devicePort = port }
        function setKeepaliveInterval(interval) {
            keepaliveInterval = interval
        }
        function refreshSerialPorts() {}
        function setAutostartEnabled(enabled) {
            autostartEnabled = enabled
        }
        function refreshAutostart() {}
    }

    QtObject {
        id: firmwareMock
        property bool serviceAvailable: true
        property bool compatible: true
        property bool ready: true
        property url homeFolder: Qt.resolvedUrl(".")
        property string packagePath: ""
        property bool busy: false
        property bool validationBusy: false
        property bool flashBusy: false
        property bool approvalAvailable: false
        property bool flashSupported: false
        property bool recoveryRequired: false
        property bool canValidate: false
        property bool canFlash: false
        property bool confirmationRequired: false
        property int progress: 0
        property string phase: "Idle"
        property string status: "Ready"
        property string kind: ""
        property string canonicalPath: ""
        property string sha256: ""
        property real sizeBytes: 0
        property string productCode: ""
        property string firmwareVersion: ""
        property string appVersion: ""
        property string errorMessage: ""
        function setPackagePath(path) { packagePath = path }
        function validatePackage() {}
        function requestFlashConfirmation() {}
        function cancelFlashConfirmation() {}
        function confirmFlash() {}
        function requestCancel() {}
        function acknowledgeFirmwareRecovery() {}
        function refresh() {}
    }

    QtObject {
        id: windowChromeMock
        property var targetWindow
        property bool maximized: false
        property bool trayAvailable: true
        property bool closeHidesToTray: true
        property bool hideSucceeds: true
        property int closeCount: 0
        property int hideCount: 0
        property int approveQuitCount: 0
        signal explicitQuitRequested()
        function startMove() {}
        function startSystemResize(edges) {}
        function toggleMaximized() { maximized = !maximized }
        function minimize() {}
        function closeWindow() {
            closeCount += 1
            if (targetWindow)
                targetWindow.close()
        }
        function handleCloseRequest() {
            if (!closeHidesToTray)
                return false
            return hideWindowToTray()
        }
        function closeWouldHideToTray() {
            return closeHidesToTray && trayAvailable
        }
        function hideWindowToTray() {
            hideCount += 1
            if (!hideSucceeds || !trayAvailable)
                return false
            if (targetWindow)
                targetWindow.hide()
            return true
        }
        function approveExplicitQuit() {
            approveQuitCount += 1
        }
    }

    QtObject {
        id: supportBundleMock
        property bool busy: false
        property bool runtimeDetailsAvailable: false
        property string state: "idle"
        property string message: ""
        property string lastExportPath: ""
        property url homeFolder: Qt.resolvedUrl(".")
        function exportToFolder(folder) {}
    }

    QtObject {
        id: cacheManagementMock

        signal eligibilityChanged()

        property string state: "Ready"
        property string phase: "Idle"
        property string message: "Ready to remove unused temporary files."
        property bool canStart: true
        property bool canCancel: false
        property bool canRefresh: false
        property bool requiresAcknowledgment: false
        property real totalFiles: 0
        property real removedFiles: 0
        property real removedBytes: 0
        property real runtimeRemovedFiles: 0
        property real runtimeRemovedBytes: 0
        property real localRemovedFiles: 0
        property real localRemovedBytes: 0
        property real eligibilityRevision: 0

        function startCleanup(expectedEligibilityRevision) {
            return expectedEligibilityRevision === eligibilityRevision
        }
        function cancelCleanup() { return true }
        function refresh() { return true }
        function acknowledgeUnresolved() {}
    }

    Component {
        id: mainComponent
        App.Main {
            runtime: runtimeMock
            mediaEditor: editorMock
            deviceMedia: deviceMediaMock
            firmware: firmwareMock
            systemMetrics: metricsMock
            settings: settingsMock
            windowChrome: windowChromeMock
            supportBundle: supportBundleMock
            cacheManagement: cacheManagementMock
            releaseUpdates: QtObject {
                property bool updateAvailable: false
                property string availableVersion: ""
                property url releaseUrl: ""
            }
            quickSmokeTest: false
            autostartRequested: false
        }
    }

    function resetMocks() {
        runtimeMock.displayStateValid = true
        runtimeMock.displaySessionActive = true
        runtimeMock.operationBusy = false
        runtimeMock.displayDeviceIdentity = "device-a"
        runtimeMock.currentScreenMode = "Full Screen"
        runtimeMock.currentPlayMode = "Single"
        runtimeMock.displayedMedia = [
            "one.mp4.h264_2240x1080"
        ]
        runtimeMock.displayLeftColor = "#112233"
        runtimeMock.brightness = 75
        runtimeMock.mirrorMode = false
        runtimeMock.waterfallMode = false
        runtimeMock.fullSubmitCount = 0
        runtimeMock.splitSubmitCount = 0
        runtimeMock.lastSubmissionId = ""
        windowChromeMock.targetWindow = null
        windowChromeMock.closeHidesToTray = true
        windowChromeMock.hideSucceeds = true
        windowChromeMock.trayAvailable = true
        windowChromeMock.closeCount = 0
        windowChromeMock.hideCount = 0
        windowChromeMock.approveQuitCount = 0
    }

    function createMainWindow() {
        const main = createTemporaryObject(
            mainComponent, testCase,
            {"width": 1100, "height": 760})
        verify(main !== null)
        windowChromeMock.targetWindow = main
        wait(0)
        return main
    }

    function waitForGuardToFinishClosing(guard) {
        tryCompare(guard, "_pendingIntent", 0)
        verify(!guard.opened)
    }

    function init() {
        resetMocks()
    }

    function test_routeFirstIntentStayAndDiscardUseProductionMain() {
        const main = createMainWindow()
        const page = findChild(main, "panoramaPage")
        const guard = findChild(main, "dirtyDraftGuard")
        verify(page !== null)
        verify(guard !== null)
        main.requestPage(1)
        compare(main.currentPage, 1)

        page.fullStyleColor = "#abcdef"
        wait(0)
        verify(page.hasUnsavedChanges)
        main.requestPage(2)
        tryVerify(() => guard.opened)
        compare(main.guardedIntent, 1)
        compare(main.guardedTarget, 2)

        windowChromeMock.explicitQuitRequested()
        wait(0)
        compare(main.guardedIntent, 1)
        compare(main.guardedTarget, 2)
        compare(windowChromeMock.approveQuitCount, 0)

        guard.stay()
        waitForGuardToFinishClosing(guard)
        compare(main.currentPage, 1)
        verify(page.hasUnsavedChanges)

        main.requestPage(2)
        tryVerify(() => guard.opened)
        guard.discard()
        tryCompare(main, "currentPage", 2)
        verify(!page.hasUnsavedChanges)
    }

    function test_dashboardSamplingFollowsVisibleHomePage() {
        const main = createMainWindow()
        tryCompare(metricsMock, "dashboardActive", true)

        main.requestPage(1)
        tryCompare(metricsMock, "dashboardActive", false)

        main.requestPage(0)
        tryCompare(metricsMock, "dashboardActive", true)

        main.hide()
        tryCompare(metricsMock, "dashboardActive", false)

        main.show()
        tryCompare(metricsMock, "dashboardActive", true)

        main.showMinimized()
        tryCompare(main, "visibility", Window.Minimized)
        tryCompare(metricsMock, "dashboardActive", false)

        main.showNormal()
        tryCompare(metricsMock, "dashboardActive", true)
    }

    function test_confirmedApplyContinuesSavedRouteOnce() {
        const main = createMainWindow()
        const page = findChild(main, "panoramaPage")
        const guard = findChild(main, "dirtyDraftGuard")
        main.requestPage(1)
        page.fullStyleColor = "#abcdef"
        wait(0)
        main.requestPage(0)
        tryVerify(() => guard.opened)

        guard.apply()
        compare(runtimeMock.fullSubmitCount, 1)
        verify(page.applyPending)
        compare(main.currentPage, 1)

        runtimeMock.displayLeftColor = "#abcdef"
        runtimeMock.displayChanged()
        runtimeMock.displayApplyFinished(
            runtimeMock.lastSubmissionId,
            "Succeeded", "Applied")
        tryCompare(main, "currentPage", 0)
        compare(runtimeMock.fullSubmitCount, 1)
        verify(!page.hasUnsavedChanges)
        verify(!guard.opened)
    }

    function test_hideAndExplicitQuitWaitForDiscard() {
        const main = createMainWindow()
        const page = findChild(main, "panoramaPage")
        const guard = findChild(main, "dirtyDraftGuard")
        main.requestPage(1)
        page.fullStyleColor = "#abcdef"
        wait(0)

        main.close()
        tryVerify(() => guard.opened)
        compare(main.guardedIntent, 2)
        verify(main.visible)
        compare(windowChromeMock.hideCount, 0)
        guard.discard()
        tryCompare(windowChromeMock, "hideCount", 1)
        verify(!main.visible)

        main.show()
        wait(0)
        waitForGuardToFinishClosing(guard)
        page.fullStyleColor = "#fedcba"
        windowChromeMock.explicitQuitRequested()
        tryVerify(() => guard.opened)
        compare(main.guardedIntent, 4)
        compare(windowChromeMock.approveQuitCount, 0)
        guard.discard()
        tryCompare(windowChromeMock, "approveQuitCount", 1)
    }

    function test_approvedCloseCannotBypassLaterDirtyState() {
        const main = createMainWindow()
        const page = findChild(main, "panoramaPage")
        const guard = findChild(main, "dirtyDraftGuard")
        main.requestPage(1)
        windowChromeMock.closeHidesToTray = false
        main.approvedCloseBypass = true
        page.fullStyleColor = "#abcdef"
        wait(0)

        main.close()
        tryVerify(() => guard.opened)
        verify(main.visible)
        verify(!main.approvedCloseBypass)
        compare(main.guardedIntent, 3)
        guard.stay()
    }
}
