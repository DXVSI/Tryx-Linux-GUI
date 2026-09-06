pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtTest

import "../../../qml/pages" as Pages

TestCase {
    id: testCase
    name: "PanoramaLayout"
    when: windowShown
    width: 1100
    height: 760

    ListModel {
        id: mediaModel

        function canDelete(mediaName) {
            return mediaName.length > 0
        }

        function deleteBlockReason(mediaName) {
            return ""
        }

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
        ListElement {
            mediaName: "two.mp4.h264_2240x1080"
            mediaId: "media-two"
            mediaSize: 2048
            mediaSource: 2
            managedOrigin: false
            readOnly: true
            thumbnailUrl: ""
            deleteAllowed: true
            deleteBlockReason: ""
            deviceCopyAllowed: true
            deviceCopyBlockReason: ""
        }
    }

    ListModel {
        id: emptyMediaModel

        function canDelete(mediaName) {
            return false
        }

        function deleteBlockReason(mediaName) {
            return ""
        }
    }

    ListModel {
        id: operationModel
        ListElement {
            operationId: "one"
            subject: "Upload"
            operationState: "Succeeded"
            message: "Done"
            canRetry: false
        }
    }

    QtObject {
        id: runtimeMock

        property bool displaySessionActive: true
        property bool operationBusy: false
        property bool compatible: true
        property bool legacyConnected: false
        property string activeOperationId: ""
        property string operationSummary: ""
        property real operationProgress: 0
        property var mediaModel: mediaModel
        property var operationModel: operationModel
        property var availableMetrics: [
            "CPU Temperature", "GPU Temperature"
        ]
        property bool metricsCatalogReady: true
        property var metricsCatalog: [
            "CPU Temperature", "CPU Frequency", "CPU Usage", "CPU Power",
            "GPU Temperature", "GPU Frequency", "GPU Usage", "GPU Power",
            "Memory Frequency", "Memory Usage", "Date&Time"
        ]
        property int brightness: 75
        property bool mirrorMode: false
        property bool waterfallMode: false
        property bool displayStateValid: true
        property bool legacyDisplayLayoutConfirmed: true
        property bool legacyDisplayBrightnessConfirmed: true
        property string displayDeviceIdentity: "device-a"
        property bool backlightEnabled: true
        property string currentScreenMode: "Full Screen"
        property string currentPlayMode: "Single"
        property var displayedMedia: []
        property var displayLeftMetrics: []
        property var displayRightMetrics: []
        property var displayLeftBadges: ["CPU Badge"]
        property var displayRightBadges: ["GPU Badge"]
        property string displayLeftPosition: "Top"
        property string displayLeftColor: "#112233"
        property string displayLeftAlignment: "Left"
        property string displayRightPosition: "Bottom"
        property string displayRightColor: "#445566"
        property string displayRightAlignment: "Right"
        property var activeMetrics: []
        property string metricsAlignment: "Left"
        property string metricsColor: "#dcdcdc"
        property bool metricsEnabled: false
        property bool samplingActive: false
        property string diagnostic: ""
        property bool capabilitiesReady: true
        property bool customBadgeTextSupported: false
        property var displayBadgeChoices: autoBadgeChoices()
        property var lastBadgeChoices: ({})
        function autoBadgeChoices() {
            return {"schemaVersion": 1, "primaryCpu": {"mode": "Auto", "text": ""},
                "primaryGpu": {"mode": "Auto", "text": ""}, "secondaryCpu": {"mode": "Auto", "text": ""},
                "secondaryGpu": {"mode": "Auto", "text": ""}}
        }
        function badgeTextError(mode, text) {
            return mode === "Auto" || (mode === "Custom" && text.trim().length > 0
                && Array.from(text.trim()).length <= 32 && !/[\n\r\t]/.test(text)) ? "" : "Invalid badge text"
        }
        property bool savedLayoutsSupported: true
        property bool savedLayoutsReady: true
        property bool savedLayoutsBusy: false
        property string savedLayoutsStatus: "Ready"
        property string savedLayoutsDiagnostic: ""
        property string savedLayoutsDeviceIdentity: "device-a"
        property var savedLayoutRows: []
        property var savedLayoutModel: savedLayoutRows
        property var savedLayoutDrafts: ({})
        property int savedLayoutDraftCount: 0
        property int savedLayoutPutCount: 0
        property int savedLayoutDeleteCount: 0
        property int savedLayoutSubmitCount: 0
        property var lastSavedLayoutPut: []
        property string lastSavedLayoutDeleteId: ""
        property var lastSavedLayoutSubmit: []
        property string nextSavedLayoutSubmissionId:
            "saved-layout-submission-1"
        property int fullApplyCount: 0
        property int splitApplyCount: 0
        property var lastFullApply: []
        property var lastSplitApply: []
        property int fullDraftSubmitCount: 0
        property int splitDraftSubmitCount: 0
        property int brightnessSetCount: 0
        property int orientationSetCount: 0
        property int abandonedDisplaySubmissionCount: 0
        property string abandonedDisplaySubmissionId: ""
        property var lastFullDraftSubmit: []
        property var lastSplitDraftSubmit: []
        property string nextDisplaySubmissionId:
            "display-submission-1"

        signal displayChanged()
        signal connectionChanged()
        signal metricsChanged()
        signal runtimeInvalidated()
        signal savedLayoutsChanged()
        signal savedLayoutPutFinished(
            string requestedLayoutId,
            string savedLayoutId,
            string revisionDecimal,
            bool success,
            string message)
        signal savedLayoutDeleteFinished(
            string layoutId, bool success, string message)
        signal displayApplyFinished(string submissionId,
                                    string outcome,
                                    string message)

        function refreshMedia() {}
        function deleteMedia(media) {}
        function applyFullScreen(media, playMode,
                                 metrics, badges,
                                 position, color,
                                 alignment) {
            fullApplyCount += 1
            lastFullApply = [media, playMode, metrics, badges,
                             position, color, alignment]
        }
        function applySplitScreen(left, right, playMode,
                                  leftMetrics, rightMetrics,
                                  leftBadges, rightBadges,
                                  leftPosition, leftColor,
                                  leftAlignment, rightPosition,
                                  rightColor, rightAlignment) {
            splitApplyCount += 1
            lastSplitApply = [
                left, right, playMode,
                leftMetrics, rightMetrics,
                leftBadges, rightBadges,
                leftPosition, leftColor, leftAlignment,
                rightPosition, rightColor, rightAlignment
            ]
        }
        function submitFullDisplayDraft(
                media, playMode, metrics, badges,
                position, color, alignment,
                layoutPresent,
                brightnessPresent, brightness,
                orientationPresent, mirror, waterfall, badgeChoices) {
            lastBadgeChoices = badgeChoices || autoBadgeChoices()
            fullDraftSubmitCount += 1
            lastFullDraftSubmit = [
                media, playMode, metrics, badges,
                position, color, alignment,
                layoutPresent,
                brightnessPresent, brightness,
                orientationPresent, mirror, waterfall
            ]
            return nextDisplaySubmissionId
        }
        function submitSplitDisplayDraft(
                left, right, playMode,
                leftMetrics, rightMetrics,
                leftBadges, rightBadges,
                leftPosition, leftColor, leftAlignment,
                rightPosition, rightColor, rightAlignment,
                layoutPresent,
                brightnessPresent, brightness,
                orientationPresent, mirror, waterfall, badgeChoices) {
            lastBadgeChoices = badgeChoices || autoBadgeChoices()
            splitDraftSubmitCount += 1
            lastSplitDraftSubmit = [
                left, right, playMode,
                leftMetrics, rightMetrics,
                leftBadges, rightBadges,
                leftPosition, leftColor, leftAlignment,
                rightPosition, rightColor, rightAlignment,
                layoutPresent,
                brightnessPresent, brightness,
                orientationPresent, mirror, waterfall
            ]
            return nextDisplaySubmissionId
        }
        function configureMetrics(enabled, metrics,
                                  alignment, color) {}
        function setBrightness(value) {
            brightnessSetCount += 1
        }
        function setBacklight(enabled) {}
        function setOrientation(mirror, waterfall) {
            orientationSetCount += 1
        }
        function abandonDisplaySubmission(submissionId) {
            abandonedDisplaySubmissionCount += 1
            abandonedDisplaySubmissionId = submissionId
        }
        function savedLayoutIdForName(name) {
            const normalized = String(name).toLowerCase()
            for (let index = 0;
                 index < savedLayoutRows.length; ++index) {
                const layout = savedLayoutRows[index]
                if (String(layout.layoutName).toLowerCase() ===
                    normalized) {
                    return String(layout.layoutId)
                }
            }
            return ""
        }
        function savedLayoutDraft(layoutId) {
            savedLayoutDraftCount += 1
            return savedLayoutDrafts[String(layoutId)] || ({})
        }
        function putSavedLayout(name, overwriteLayoutId,
                                fullState) {
            savedLayoutPutCount += 1
            lastSavedLayoutPut = [
                name, overwriteLayoutId, fullState
            ]
        }
        function deleteSavedLayout(layoutId) {
            savedLayoutDeleteCount += 1
            lastSavedLayoutDeleteId = layoutId
        }
        function submitSavedLayoutDraft(
                layoutId, revisionDecimal, fullState) {
            savedLayoutSubmitCount += 1
            lastSavedLayoutSubmit = [
                layoutId, revisionDecimal, fullState
            ]
            return nextSavedLayoutSubmissionId
        }
        function retryOperation(operationId) {}
        function cancelActiveOperation() {}
    }

    QtObject {
        id: editorMock
        property url homeFolder: Qt.resolvedUrl(".")
        function begin(url) {}
        function beginDropped(urls) {}
    }

    QtObject {
        id: deviceMediaMock

        property bool busy: false
        property bool overwriteConfirmationPending: false
        property string overwriteFileName: ""

        signal stateChanged()

        function beginEdit(mediaId, mediaName) {}
        function beginExport(mediaId, mediaName, folder, fileName) {}
        function confirmOverwrite() {}
        function cancelOverwrite() {}
        function suggestedExportFileName(mediaName) {
            return "device-copy.h264"
        }
    }

    Component {
        id: panoramaComponent
        Pages.PanoramaPage {
            runtime: runtimeMock
            editor: editorMock
            deviceMedia: deviceMediaMock
        }
    }

    Component {
        id: signalSpyComponent

        SignalSpy {}
    }

    function fullSavedLayoutDraft() {
        return {
            "layoutId": "layout-full",
            "revision": "11",
            "deviceIdentity": "device-a",
            "layout": {
                "split": false,
                "media": ["two.mp4.h264_2240x1080"],
                "playMode": "Shuffle",
                "metrics": ["GPU Usage", "CPU Temperature"],
                "badges": ["GPU Badge", "CPU Badge"],
                "position": "Bottom",
                "color": "#123abc",
                "alignment": "Center"
            },
            "brightness": 64,
            "orientation": {
                "mirror": true,
                "waterfall": false
            }
        }
    }

    function splitSavedLayoutDraft() {
        return {
            "layoutId": "layout-split",
            "revision": "19",
            "deviceIdentity": "device-a",
            "layout": {
                "split": true,
                "media": [
                    "two.mp4.h264_2240x1080",
                    "one.mp4.h264_2240x1080"
                ],
                "playMode": "Single",
                "leftMetrics": [
                    "GPU Temperature", "CPU Usage"
                ],
                "rightMetrics": [
                    "Memory Usage", "CPU Frequency"
                ],
                "leftBadges": ["GPU Badge", "CPU Badge"],
                "rightBadges": ["CPU Badge", "GPU Badge"],
                "leftPosition": "Bottom",
                "leftColor": "#010203",
                "leftAlignment": "Right",
                "rightPosition": "Top",
                "rightColor": "#a0b0c0",
                "rightAlignment": "Center"
            },
            "brightness": 37,
            "orientation": {
                "mirror": false,
                "waterfall": true
            }
        }
    }

    function init() {
        runtimeMock.mediaModel = mediaModel
        mediaModel.setProperty(0, "mediaName",
                               "one.mp4.h264_2240x1080")
        mediaModel.setProperty(0, "mediaSize", 1024)
        mediaModel.setProperty(0, "mediaSource", 1)
        mediaModel.setProperty(0, "managedOrigin", true)
        mediaModel.setProperty(0, "readOnly", false)
        mediaModel.setProperty(1, "mediaName",
                               "two.mp4.h264_2240x1080")
        mediaModel.setProperty(1, "mediaSize", 2048)
        mediaModel.setProperty(1, "mediaSource", 2)
        mediaModel.setProperty(1, "managedOrigin", false)
        mediaModel.setProperty(1, "readOnly", true)
        runtimeMock.legacyConnected = false
        runtimeMock.currentScreenMode = "Full Screen"
        runtimeMock.currentPlayMode = "Single"
        runtimeMock.displayedMedia = []
        runtimeMock.displayLeftMetrics = []
        runtimeMock.displayRightMetrics = []
        runtimeMock.displayLeftBadges = ["CPU Badge"]
        runtimeMock.displayRightBadges = ["GPU Badge"]
        runtimeMock.brightness = 75
        runtimeMock.mirrorMode = false
        runtimeMock.waterfallMode = false
        runtimeMock.displaySessionActive = true
        runtimeMock.displayStateValid = true
        runtimeMock.legacyDisplayLayoutConfirmed = true
        runtimeMock.legacyDisplayBrightnessConfirmed = true
        runtimeMock.displayDeviceIdentity = "device-a"
        runtimeMock.operationBusy = false
        runtimeMock.displayLeftPosition = "Top"
        runtimeMock.displayLeftColor = "#112233"
        runtimeMock.displayLeftAlignment = "Left"
        runtimeMock.displayRightPosition = "Bottom"
        runtimeMock.displayRightColor = "#445566"
        runtimeMock.displayRightAlignment = "Right"
        runtimeMock.availableMetrics = [
            "CPU Temperature", "GPU Temperature"
        ]
        runtimeMock.metricsCatalogReady = true
        runtimeMock.metricsCatalog = [
            "CPU Temperature", "CPU Frequency", "CPU Usage", "CPU Power",
            "GPU Temperature", "GPU Frequency", "GPU Usage", "GPU Power",
            "Memory Frequency", "Memory Usage", "Date&Time"
        ]
        runtimeMock.capabilitiesReady = true
        runtimeMock.customBadgeTextSupported = false
        runtimeMock.displayBadgeChoices = runtimeMock.autoBadgeChoices()
        runtimeMock.lastBadgeChoices = ({})
        runtimeMock.savedLayoutsSupported = true
        runtimeMock.savedLayoutsReady = true
        runtimeMock.savedLayoutsBusy = false
        runtimeMock.savedLayoutsStatus = "Ready"
        runtimeMock.savedLayoutsDiagnostic = ""
        runtimeMock.savedLayoutsDeviceIdentity = "device-a"
        runtimeMock.savedLayoutRows = [
            {
                "layoutId": "layout-full",
                "layoutRevision": "11",
                "layoutName": "Full desk",
                "screenMode": "Full Screen",
                "mediaNames": [
                    "two.mp4.h264_2240x1080"
                ]
            },
            {
                "layoutId": "layout-split",
                "layoutRevision": "19",
                "layoutName": "Split desk",
                "screenMode": "Screen Splitting",
                "mediaNames": [
                    "two.mp4.h264_2240x1080",
                    "one.mp4.h264_2240x1080"
                ]
            }
        ]
        runtimeMock.savedLayoutDrafts = ({
            "layout-full": fullSavedLayoutDraft(),
            "layout-split": splitSavedLayoutDraft()
        })
        runtimeMock.savedLayoutDraftCount = 0
        runtimeMock.savedLayoutPutCount = 0
        runtimeMock.savedLayoutDeleteCount = 0
        runtimeMock.savedLayoutSubmitCount = 0
        runtimeMock.lastSavedLayoutPut = []
        runtimeMock.lastSavedLayoutDeleteId = ""
        runtimeMock.lastSavedLayoutSubmit = []
        runtimeMock.nextSavedLayoutSubmissionId =
            "saved-layout-submission-1"
        runtimeMock.fullApplyCount = 0
        runtimeMock.splitApplyCount = 0
        runtimeMock.lastFullApply = []
        runtimeMock.lastSplitApply = []
        runtimeMock.fullDraftSubmitCount = 0
        runtimeMock.splitDraftSubmitCount = 0
        runtimeMock.brightnessSetCount = 0
        runtimeMock.orientationSetCount = 0
        runtimeMock.abandonedDisplaySubmissionCount = 0
        runtimeMock.abandonedDisplaySubmissionId = ""
        runtimeMock.lastFullDraftSubmit = []
        runtimeMock.lastSplitDraftSubmit = []
        runtimeMock.nextDisplaySubmissionId =
            "display-submission-1"
    }

    function test_draftEditorsWaitForFirstConfirmedState() {
        runtimeMock.displaySessionActive = false
        runtimeMock.displayStateValid = false
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const layoutGroup =
            findChild(page, "displayLayoutGroup")
        const brightness =
            findChild(page, "brightnessSlider")
        const mirror =
            findChild(page, "mirrorDraftCheckBox")
        const waterfall =
            findChild(page, "waterfallDraftCheckBox")
        verify(layoutGroup !== null)
        verify(brightness !== null)
        verify(mirror !== null)
        verify(waterfall !== null)
        verify(!page.confirmedSnapshotReady)
        verify(!layoutGroup.enabled)
        verify(!brightness.enabled)
        verify(!mirror.enabled)
        verify(!waterfall.enabled)

        page.toggleMedia("one.mp4.h264_2240x1080")
        const metricSelector = findChild(page, "fullMetricSelector")
        verify(metricSelector !== null)
        metricSelector.requestToggle("CPU Temperature", null)
        page.toggleBadge("full", "GPU Badge")
        compare(page.selectedMedia, [])
        compare(page.fullMetrics, [])
        compare(page.fullBadges, [])
        verify(!page.hasUnsavedChanges)

        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics =
            ["GPU Temperature"]
        runtimeMock.displaySessionActive = true
        runtimeMock.displayStateValid = true
        runtimeMock.displayChanged()
        wait(0)

        verify(page.confirmedSnapshotReady)
        verify(layoutGroup.enabled)
        verify(brightness.enabled)
        verify(mirror.enabled)
        verify(waterfall.enabled)
        compare(page.selectedMedia,
                ["one.mp4.h264_2240x1080"])
        compare(page.fullMetrics, ["GPU Temperature"])
        verify(!page.hasUnsavedChanges)
    }

    function test_legacyDraftUsesIdentityBoundBootstrapAndFence() {
        runtimeMock.legacyConnected = true
        runtimeMock.displaySessionActive = false
        runtimeMock.displayStateValid = false
        runtimeMock.legacyDisplayLayoutConfirmed = false
        runtimeMock.legacyDisplayBrightnessConfirmed = false
        runtimeMock.displayDeviceIdentity = "legacy:device-a"
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        verify(page.confirmedSnapshotReady)
        page.toggleMedia("one.mp4.h264_2240x1080")
        wait(0)
        verify(page.hasUnsavedChanges)
        verify(page.canApplyChanges)

        page.discardChanges()
        page.waterfallDraft = true
        wait(0)
        verify(page.hasUnsavedChanges)
        verify(!page.canApplyChanges)
        verify(page.applyBlockReason.indexOf("legacy") >= 0)
        page.discardChanges()
        page.toggleMedia("one.mp4.h264_2240x1080")
        wait(0)

        runtimeMock.displayDeviceIdentity =
            "legacy:device-b"
        wait(0)
        verify(page.hasUnsavedChanges)
        verify(page.hasExternalConflict)
        verify(!page.canApplyChanges)

        runtimeMock.connectionChanged()
        wait(0)
        verify(page.hasExternalConflict)
        page.discardChanges()
        wait(0)
        verify(!page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
        compare(page.selectedMedia, [])
    }

    function test_sameLegacyIdentityReconnectPreservesDefaultValuedDraft() {
        runtimeMock.legacyConnected = true
        runtimeMock.displaySessionActive = false
        runtimeMock.displayStateValid = true
        runtimeMock.legacyDisplayLayoutConfirmed = true
        runtimeMock.legacyDisplayBrightnessConfirmed = true
        runtimeMock.displayDeviceIdentity = "legacy:device-a"
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.brightness = 50
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.toggleMedia("one.mp4.h264_2240x1080")
        page.brightnessDraft = 0
        wait(0)
        compare(page.selectedMedia, [])
        compare(page.brightnessDraft, 0)
        verify(page.hasUnsavedChanges)

        runtimeMock.legacyConnected = false
        runtimeMock.displayStateValid = false
        runtimeMock.legacyDisplayLayoutConfirmed = false
        runtimeMock.legacyDisplayBrightnessConfirmed = false
        runtimeMock.connectionChanged()
        wait(0)
        verify(page.hasUnsavedChanges)

        // An intermediate legacy snapshot without an exact serial must not
        // publish zero/empty defaults as a new baseline.
        runtimeMock.displayedMedia = []
        runtimeMock.brightness = 0
        runtimeMock.legacyConnected = true
        runtimeMock.displayDeviceIdentity = ""
        runtimeMock.connectionChanged()
        wait(0)
        compare(page.selectedMedia, [])
        compare(page.brightnessDraft, 0)
        verify(page.hasUnsavedChanges)

        // Rebinding the same exact serial remains unconfirmed until a fresh
        // legacy mutation signal, so the default-valued draft stays dirty.
        runtimeMock.displayDeviceIdentity = "legacy:device-a"
        runtimeMock.connectionChanged()
        wait(0)
        compare(page.selectedMedia, [])
        compare(page.brightnessDraft, 0)
        verify(page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
    }

    function test_boundedContentAndGroups() {
        const compactPage = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 820, "height": 620})
        verify(compactPage !== null)
        wait(0)
        const compactContent =
            findChild(compactPage, "panoramaContent")
        const compactWorkspace =
            findChild(compactPage, "displayWorkspace")
        verify(compactContent !== null)
        verify(compactWorkspace !== null)
        compare(compactWorkspace.columns, 1)
        verify(compactContent.width <= compactPage.availableWidth)

        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const content = findChild(page, "panoramaContent")
        const mediaGroup = findChild(page, "mediaLibraryGroup")
        const operationGroup =
            findChild(page, "recentOperationsGroup")
        const displayLayoutGroup =
            findChild(page, "displayLayoutGroup")
        const metricsOverlayControls =
            findChild(page, "metricsOverlayControls")
        const workspace =
            findChild(page, "displayWorkspace")
        verify(content !== null)
        verify(mediaGroup !== null)
        verify(operationGroup !== null)
        verify(displayLayoutGroup !== null)
        verify(metricsOverlayControls !== null)
        compare(findChild(page, "liveMetricsGroup"), null)
        verify(workspace !== null)
        compare(workspace.columns, 1)
        compare(content.x, 24)
        verify(content.width <= page.availableWidth)
        verify(content.width >= page.availableWidth - 49)
        verify(mediaGroup.height >= 235)
        verify(operationGroup.height >= 115)

        const widePage = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1250, "height": 700})
        verify(widePage !== null)
        wait(0)
        const wideWorkspace =
            findChild(widePage, "displayWorkspace")
        verify(wideWorkspace !== null)
        compare(wideWorkspace.columns, 2)
    }

    function test_twoColumnControlsStayInsideContentBounds() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1310, "height": 760})
        verify(page !== null)
        wait(0)

        const content = findChild(page, "panoramaContent")
        const workspace = findChild(page, "displayWorkspace")
        verify(content !== null)
        verify(workspace !== null)
        compare(workspace.columns, 2)

        const contentRight = content.mapToItem(
            page, content.width, 0).x
        const boundedItems = [
            "displayLayoutGroup",
            "playModeCombo",
            "mediaSelectionSummary",
            "fullMetricSelector",
            "fullMetricSearch",
            "fullMetricCounter",
            "fullGpuBadge",
            "fullStyleEditor",
            "fullStyleAlignmentCombo",
            "fullStyleColorField",
            "fullStyleColorButton",
            "metricsOverlayControls",
            "metricsSamplingStatus",
            "metricsSamplingExplanation",
            "applyDisplayButton"
        ]
        for (const objectName of boundedItems) {
            const item = findChild(page, objectName)
            verify(item !== null, objectName + " must exist")
            const itemRight = item.mapToItem(page, item.width, 0).x
            verify(itemRight <= contentRight + 0.5,
                   objectName + " exceeds the content boundary: " +
                   itemRight + " > " + contentRight)
        }

        page.splitMode = true
        wait(0)
        const splitBoundedItems = [
            "splitMetricGrid",
            "leftMetricSelector",
            "leftMetricCounter",
            "rightMetricSelector",
            "rightMetricCounter",
            "splitStyleGrid",
            "leftStyleEditor",
            "leftStyleAlignmentCombo",
            "rightStyleEditor",
            "rightStyleAlignmentCombo"
        ]
        for (const objectName of splitBoundedItems) {
            const item = findChild(page, objectName)
            verify(item !== null, objectName + " must exist")
            const itemRight = item.mapToItem(page, item.width, 0).x
            verify(itemRight <= contentRight + 0.5,
                   objectName + " exceeds the content boundary: " +
                   itemRight + " > " + contentRight)
        }
    }

    function test_dateAndTimeMetricUsesCanonicalToken() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        const selector = findChild(page, "fullMetricSelector")
        verify(selector !== null)
        compare(selector.metricLabel("Date&Time"), "Date and time")
        compare(selector.metricLabel("DateTime"), "DateTime")
    }

    function test_mediaMetadataUsesApi8Roles() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const userCard = findChild(page, "mediaCard-media-one")
        const userOrigin =
            findChild(page, "mediaOriginLabel-media-one")
        const userSize = findChild(page, "mediaSizeLabel-media-one")
        const presetOrigin =
            findChild(page, "mediaOriginLabel-media-two")
        const presetSize =
            findChild(page, "mediaSizeLabel-media-two")

        verify(userCard !== null)
        verify(userOrigin !== null)
        verify(userSize !== null)
        verify(presetOrigin !== null)
        verify(presetSize !== null)
        compare(userOrigin.text, "User media")
        compare(userSize.text, "1.0 KiB")
        compare(presetOrigin.text, "Device preset")
        compare(presetSize.text, "2.0 KiB")
        compare(userCard.Accessible.name,
                "one.mp4.h264_2240x1080")
        verify(userCard.Accessible.description.indexOf(
                   "User media") >= 0)
        verify(userCard.Accessible.description.indexOf(
                   "1.0 KiB") >= 0)
        verify(userCard.Accessible.selectable)
        verify(!userCard.Accessible.selected)
    }

    function test_mediaMetadataSizeBoundaries_data() {
        return [
            {"tag": "unknown", "bytes": 0,
             "expected": "Unknown size"},
            {"tag": "negative", "bytes": -1,
             "expected": "Unknown size"},
            {"tag": "not-a-number", "bytes": Number.NaN,
             "expected": "Unknown size"},
            {"tag": "infinite", "bytes": Number.POSITIVE_INFINITY,
             "expected": "Unknown size"},
            {"tag": "one-byte", "bytes": 1,
             "expected": "1 B"},
            {"tag": "bytes", "bytes": 1023,
             "expected": "1023 B"},
            {"tag": "one-kib", "bytes": 1024,
             "expected": "1.0 KiB"},
            {"tag": "fractional-kib", "bytes": 1536,
             "expected": "1.5 KiB"},
            {"tag": "one-mib", "bytes": 1048576,
             "expected": "1.0 MiB"},
            {"tag": "one-gib", "bytes": 1073741824,
             "expected": "1.0 GiB"}
        ]
    }

    function test_mediaMetadataSizeBoundaries(data) {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        compare(page.formatMediaSize(data.bytes), data.expected)
    }

    function test_mediaMetadataFallbacksAndLegacyBoundary() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const origin = findChild(page, "mediaOriginLabel-media-one")
        const size = findChild(page, "mediaSizeLabel-media-one")
        verify(origin !== null)
        verify(size !== null)

        mediaModel.setProperty(0, "mediaSource", 0)
        mediaModel.setProperty(0, "mediaSize", 0)
        wait(0)
        compare(origin.text, "Unknown origin")
        compare(size.text, "Unknown size")

        mediaModel.setProperty(0, "mediaSource", 99)
        wait(0)
        compare(origin.text, "Unknown origin")

        mediaModel.setProperty(0, "mediaSource", 1)
        runtimeMock.legacyConnected = true
        wait(0)
        compare(origin.text, "Unknown origin")

        runtimeMock.legacyConnected = false
        mediaModel.setProperty(0, "readOnly", true)
        mediaModel.setProperty(0, "managedOrigin", false)
        mediaModel.setProperty(1, "readOnly", false)
        mediaModel.setProperty(1, "managedOrigin", true)
        wait(0)
        compare(origin.text, "User media")
        compare(findChild(page, "mediaOriginLabel-media-two").text,
                "Device preset")
    }

    function test_mediaCardMetadataFitsCompactWidthAndKeyboard() {
        const longName =
            "a-very-long-user-media-name-that-must-remain-accessible" +
            ".mp4.h264_2240x1080"
        mediaModel.setProperty(0, "mediaName", longName)

        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 820, "height": 620})
        verify(page !== null)
        wait(0)

        const card = findChild(page, "mediaCard-media-one")
        const badge = findChild(page, "mediaOriginBadge-media-one")
        const action = findChild(page, "mediaActionButton")
        verify(card !== null)
        verify(badge !== null)
        verify(action !== null)
        compare(card.Accessible.name, longName)
        verify(badge.x + badge.width <= action.x)
        verify(badge.x >= 0)
        verify(action.x + action.width <= action.parent.width)
        verify(card.activeFocusOnTab)

        card.forceActiveFocus()
        tryVerify(() => card.activeFocus)
        keyClick(Qt.Key_Space)
        wait(0)
        compare(page.selectedMedia, [longName])
        verify(card.Accessible.selected)
        keyClick(Qt.Key_Return)
        wait(0)
        compare(page.selectedMedia, [])
        verify(!card.Accessible.selected)
    }

    function test_emptyMediaLibraryStateIsOriginNeutral() {
        runtimeMock.mediaModel = emptyMediaModel
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const emptyState =
            findChild(page, "mediaLibraryEmptyState")
        const grid = findChild(page, "mediaGrid")
        verify(emptyState !== null)
        verify(grid !== null)
        compare(grid.count, 0)
        compare(emptyState.text, "No media available")
    }

    function test_playModeReflectsSnapshot() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        const combo = findChild(page, "playModeCombo")
        verify(combo !== null)

        runtimeMock.currentScreenMode = "Full Screen"
        runtimeMock.currentPlayMode = "Loop"
        runtimeMock.displayChanged()
        wait(0)
        compare(combo.currentText, "Loop")

        runtimeMock.currentPlayMode = "Single"
        runtimeMock.displayChanged()
    }

    function test_mediaActionsArePerCardAndBrightnessIsBounded() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const actionButton =
            findChild(page, "mediaActionButton")
        const uploadButton =
            findChild(page, "uploadMediaButton")
        const brightness =
            findChild(page, "brightnessSlider")
        const fullCpuBadge =
            findChild(page, "fullCpuBadge")
        const fullGpuBadge =
            findChild(page, "fullGpuBadge")
        const leftCpuBadge =
            findChild(page, "leftCpuBadge")
        const rightGpuBadge =
            findChild(page, "rightGpuBadge")
        verify(actionButton !== null)
        verify(actionButton.enabled)
        verify(actionButton.width <= 30)
        verify(actionButton.height <= 30)
        verify(uploadButton !== null)
        compare(uploadButton.contentItem.color.toString(), "#171a1e")
        verify(fullCpuBadge !== null)
        verify(fullGpuBadge !== null)
        verify(leftCpuBadge !== null)
        verify(rightGpuBadge !== null)
        verify(fullCpuBadge.checked)
        verify(!fullGpuBadge.checked)
        verify(leftCpuBadge.checked)
        verify(rightGpuBadge.checked)
        page.toggleBadge("full", "GPU Badge")
        compare(page.fullBadges,
                ["CPU Badge", "GPU Badge"])
        verify(brightness !== null)
        verify(brightness.width <= 380)
    }

    function test_styleEditorsFollowConfirmedStateAndMode() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        const fullEditor = findChild(page, "fullStyleEditor")
        const leftEditor = findChild(page, "leftStyleEditor")
        const rightEditor = findChild(page, "rightStyleEditor")
        const sharedEditor = findChild(page, "sharedStyleEditor")
        verify(fullEditor !== null)
        verify(leftEditor !== null)
        verify(rightEditor !== null)
        verify(sharedEditor !== null)
        verify(fullEditor.activeForMode)
        verify(!leftEditor.activeForMode)
        verify(!rightEditor.activeForMode)
        compare(fullEditor.placement, "Top")
        compare(fullEditor.textColor, "#112233")
        compare(fullEditor.alignment, "Left")

        page.splitMode = true
        wait(0)
        verify(!fullEditor.activeForMode)
        verify(leftEditor.activeForMode)
        verify(rightEditor.activeForMode)
        verify(!sharedEditor.activeForMode)
        compare(rightEditor.placement, "Bottom")
        compare(rightEditor.textColor, "#445566")
        compare(rightEditor.alignment, "Right")

        runtimeMock.legacyConnected = true
        wait(0)
        verify(!leftEditor.activeForMode)
        verify(!rightEditor.activeForMode)
        verify(sharedEditor.activeForMode)
    }

    function test_fullStyleSubmitsExactlyOnce() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        page.selectedMedia = ["one.mp4.h264_2240x1080"]
        const editor = findChild(page, "fullStyleEditor")
        const applyButton = findChild(page, "applyDisplayButton")
        verify(editor !== null)
        verify(applyButton !== null)

        editor.placementEdited("Bottom")
        editor.textColorEdited("#abcdef")
        editor.alignmentEdited("Center")
        wait(0)
        verify(applyButton.enabled)
        applyButton.clicked()

        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)
        compare(runtimeMock.lastFullDraftSubmit[4], "Bottom")
        compare(runtimeMock.lastFullDraftSubmit[5], "#abcdef")
        compare(runtimeMock.lastFullDraftSubmit[6], "Center")
        compare(runtimeMock.lastFullDraftSubmit[7], true)
    }

    function test_splitStylesSubmitExactlyOnce() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        page.splitMode = true
        page.selectedMedia = ["one.mp4.h264_2240x1080",
                              "two.mp4.h264_2240x1080"]
        const leftEditor = findChild(page, "leftStyleEditor")
        const rightEditor = findChild(page, "rightStyleEditor")
        const applyButton = findChild(page, "applyDisplayButton")
        verify(leftEditor !== null)
        verify(rightEditor !== null)
        verify(applyButton !== null)

        leftEditor.textColorEdited("#123456")
        leftEditor.alignmentEdited("Center")
        rightEditor.textColorEdited("#abcdef")
        rightEditor.alignmentEdited("Right")
        wait(0)
        applyButton.clicked()

        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 1)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)
        compare(runtimeMock.lastSplitDraftSubmit[7], "Top")
        compare(runtimeMock.lastSplitDraftSubmit[8], "#123456")
        compare(runtimeMock.lastSplitDraftSubmit[9], "Center")
        compare(runtimeMock.lastSplitDraftSubmit[10], "Bottom")
        compare(runtimeMock.lastSplitDraftSubmit[11], "#abcdef")
        compare(runtimeMock.lastSplitDraftSubmit[12], "Right")
        compare(runtimeMock.lastSplitDraftSubmit[13], true)
    }

    function test_legacySplitSubmitsOneSharedStyle() {
        runtimeMock.legacyConnected = true
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        page.splitMode = true
        page.selectedMedia = ["one.mp4.h264_2240x1080",
                              "two.mp4.h264_2240x1080"]
        const sharedEditor = findChild(page, "sharedStyleEditor")
        const applyButton = findChild(page, "applyDisplayButton")
        verify(sharedEditor !== null)
        verify(applyButton !== null)

        sharedEditor.textColorEdited("#778899")
        sharedEditor.alignmentEdited("Center")
        wait(0)
        applyButton.clicked()

        compare(runtimeMock.splitDraftSubmitCount, 1)
        compare(runtimeMock.splitApplyCount, 0)
        compare(runtimeMock.lastSplitDraftSubmit[7],
                runtimeMock.lastSplitDraftSubmit[10])
        compare(runtimeMock.lastSplitDraftSubmit[8], "#778899")
        compare(runtimeMock.lastSplitDraftSubmit[8],
                runtimeMock.lastSplitDraftSubmit[11])
        compare(runtimeMock.lastSplitDraftSubmit[9], "Center")
        compare(runtimeMock.lastSplitDraftSubmit[9],
                runtimeMock.lastSplitDraftSubmit[12])
    }

    function test_styleEditorsValidateAndReflow() {
        runtimeMock.currentScreenMode = "Screen Splitting"
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 520, "height": 700})
        verify(page !== null)
        page.selectedMedia = ["one.mp4.h264_2240x1080",
                              "two.mp4.h264_2240x1080"]
        wait(0)

        const styleGrid = findChild(page, "splitStyleGrid")
        const leftEditor = findChild(page, "leftStyleEditor")
        const colorField = findChild(page, "leftStyleColorField")
        const colorError = findChild(page, "leftStyleColorError")
        const alignmentCombo =
            findChild(page, "leftStyleAlignmentCombo")
        const placementCombo =
            findChild(page, "leftStylePlacementCombo")
        const applyButton = findChild(page, "applyDisplayButton")
        const applyBlockReason = findChild(
            page, "displayApplyBlockReason")
        verify(styleGrid !== null)
        verify(leftEditor !== null)
        verify(colorField !== null)
        verify(colorError !== null)
        verify(alignmentCombo !== null)
        verify(placementCombo !== null)
        verify(applyButton !== null)
        verify(applyBlockReason !== null)
        compare(page.splitMode, true)
        compare(runtimeMock.legacyConnected, false)
        compare(styleGrid.columns, 1)
        verify(colorField.activeFocusOnTab)
        verify(alignmentCombo.activeFocusOnTab)
        verify(colorField.Accessible.name.length > 0)
        verify(alignmentCombo.Accessible.name.length > 0)

        leftEditor.textColorEdited("#12")
        wait(0)
        verify(!leftEditor.valid)
        verify(leftEditor.validationErrorVisible)
        compare(colorError.Accessible.role, Accessible.AlertMessage)
        compare(applyBlockReason.Accessible.role,
                Accessible.AlertMessage)
        verify(!applyButton.enabled)

        leftEditor.textColorEdited("#123456")
        leftEditor.placementEdited("Middle")
        wait(0)
        verify(!leftEditor.valid)
        compare(placementCombo.currentIndex, -1)
        verify(leftEditor.validationErrorVisible)
        verify(!applyButton.enabled)

        leftEditor.placementEdited("Top")
        leftEditor.alignmentEdited("Justify")
        wait(0)
        verify(!leftEditor.valid)
        compare(alignmentCombo.currentIndex, -1)
        verify(leftEditor.validationErrorVisible)
        verify(!applyButton.enabled)

        leftEditor.alignmentEdited("Left")
        page.width = 820
        wait(0)
        verify(leftEditor.valid)
        verify(styleGrid.columns === 2,
               "style grid width: " + styleGrid.width +
               ", parent: " + styleGrid.parent.width +
               ", group: " +
               findChild(page, "displayLayoutGroup").width +
               ", workspace: " +
               findChild(page, "displayWorkspace").width +
               ", page available width: " + page.availableWidth)
    }

    function test_customBadgeEditsOneImmutableDraft() {
        runtimeMock.customBadgeTextSupported = true
        runtimeMock.displayedMedia = ["one.mp4.h264_2240x1080"]
        let accepted = runtimeMock.autoBadgeChoices()
        accepted.primaryCpu = {"mode": "Custom", "text": "Accepted"}
        runtimeMock.displayBadgeChoices = accepted
        const page = createTemporaryObject(panoramaComponent, testCase, {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)
        const field = findChild(page, "fullCpuBadgeTextField")
        const mode = findChild(page, "fullCpuBadgeMode")
        verify(field !== null)
        verify(mode !== null)
        compare(field.text, "Accepted")
        verify(field.activeFocusOnTab)
        verify(mode.activeFocusOnTab)
        verify(!page.hasUnsavedChanges)
        field.forceActiveFocus()
        tryCompare(field, "activeFocus", true)
        keyClick(Qt.Key_A, Qt.ControlModifier)
        keyClick(Qt.Key_R)
        keyClick(Qt.Key_I)
        keyClick(Qt.Key_G)
        compare(field.text, "rig")
        compare(page.fullBadgeChoices.cpu.text, "rig")
        compare(runtimeMock.fullDraftSubmitCount, 0)
        page.editBadgeChoice("full", "cpu", "Custom", "")
        verify(!page.canApplyChanges)
        page.editBadgeChoice("full", "cpu", "Custom", "  My rig  ")
        verify(page.hasUnsavedChanges)
        verify(page.canApplyChanges)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.lastBadgeChoices.primaryCpu.text, "My rig")
        compare(runtimeMock.lastBadgeChoices.secondaryCpu.mode, "Auto")
        verify(!field.enabled)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 1)
    }

    function test_customBadgeSplitAndCapabilityLossPreserveDraft() {
        runtimeMock.customBadgeTextSupported = true
        runtimeMock.currentScreenMode = "Screen Splitting"
        runtimeMock.displayedMedia = ["one.mp4.h264_2240x1080", "two.mp4.h264_2240x1080"]
        runtimeMock.displayRightBadges = ["CPU Badge"]
        let accepted = runtimeMock.autoBadgeChoices()
        accepted.primaryCpu = {"mode": "Custom", "text": "Left"}
        accepted.secondaryCpu = {"mode": "Custom", "text": "Right"}
        runtimeMock.displayBadgeChoices = accepted
        const page = createTemporaryObject(panoramaComponent, testCase, {"width": 780, "height": 700})
        verify(page !== null)
        wait(0)
        page.editBadgeChoice("right", "cpu", "Custom", "New right")
        const draft = page.captureSavedLayoutFullState()
        compare(draft.layout.badgeChoices.primaryCpu.text, "Left")
        compare(draft.layout.badgeChoices.secondaryCpu.text, "New right")
        runtimeMock.customBadgeTextSupported = false
        verify(!page.canApplyChanges)
        compare(page.captureSavedLayoutFullState().layout.badgeChoices.secondaryCpu.text, "New right")
        runtimeMock.customBadgeTextSupported = true
        page.toggleBadge("right", "CPU Badge")
        page.toggleBadge("right", "CPU Badge")
        compare(page.captureSavedLayoutFullState().layout.badgeChoices.secondaryCpu.mode, "Auto")
        compare(runtimeMock.splitDraftSubmitCount, 0)
    }

    function test_dirtyUsesCanonicalActiveLayoutAndScreenDomains() {
        runtimeMock.currentPlayMode = "Loop"
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics =
            ["CPU Temperature", "GPU Temperature"]
        runtimeMock.displayLeftColor = "#aabbcc"

        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        verify(!page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)

        // RGB hex case is presentation-only, not a dirty change.
        page.fullStyleColor = "#AABBCC"
        wait(0)
        verify(!page.hasUnsavedChanges)

        // Inactive Split drafts do not participate in the Full snapshot.
        page.rightStyleColor = "#fedcba"
        page.rightStyleAlignment = "Center"
        wait(0)
        verify(!page.hasUnsavedChanges)

        // Metric ordering is part of the immutable layout request.
        page.fullMetrics =
            ["GPU Temperature", "CPU Temperature"]
        wait(0)
        verify(page.hasUnsavedChanges)
        page.fullMetrics =
            ["CPU Temperature", "GPU Temperature"]
        wait(0)
        verify(!page.hasUnsavedChanges)

        page.brightnessDraft = 82
        wait(0)
        verify(page.hasUnsavedChanges)
        page.brightnessDraft = runtimeMock.brightness
        wait(0)
        verify(!page.hasUnsavedChanges)

        page.mirrorDraft = true
        wait(0)
        verify(page.hasUnsavedChanges)
        page.mirrorDraft = runtimeMock.mirrorMode
        page.waterfallDraft = true
        wait(0)
        verify(page.hasUnsavedChanges)
    }

    function test_splitDirtyPreservesSideAndArrayOrder() {
        runtimeMock.currentScreenMode = "Screen Splitting"
        runtimeMock.displayedMedia = [
            "one.mp4.h264_2240x1080",
            "two.mp4.h264_2240x1080"
        ]
        runtimeMock.displayLeftMetrics =
            ["CPU Temperature", "GPU Temperature"]
        runtimeMock.displayRightBadges =
            ["CPU Badge", "GPU Badge"]
        runtimeMock.displayRightColor = "#aabbcc"
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)
        verify(!page.hasUnsavedChanges)

        // Full controls are inactive while Split is canonical.
        page.fullMetrics = ["Memory Usage"]
        page.fullStyleColor = "#fedcba"
        wait(0)
        verify(!page.hasUnsavedChanges)

        // Color case is normalized independently for each active side.
        page.rightStyleColor = "#AABBCC"
        wait(0)
        verify(!page.hasUnsavedChanges)

        // Media order carries the Left/Right meaning.
        page.selectedMedia = [
            "two.mp4.h264_2240x1080",
            "one.mp4.h264_2240x1080"
        ]
        wait(0)
        verify(page.hasUnsavedChanges)
        page.selectedMedia = runtimeMock.displayedMedia.slice(0)
        wait(0)
        verify(!page.hasUnsavedChanges)

        page.leftMetrics =
            ["GPU Temperature", "CPU Temperature"]
        wait(0)
        verify(page.hasUnsavedChanges)
        page.leftMetrics =
            runtimeMock.displayLeftMetrics.slice(0)
        wait(0)
        verify(!page.hasUnsavedChanges)

        page.rightBadges = ["GPU Badge", "CPU Badge"]
        wait(0)
        verify(page.hasUnsavedChanges)
    }

    function test_displayChangedPreservesDirtyAndMergesCleanDomains() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.fullStyleColor = "#abcdef"
        wait(0)
        verify(page.hasUnsavedChanges)

        // A displayChanged emitted only for retranslation must not erase the
        // dirty layout or invent a conflict.
        runtimeMock.displayChanged()
        wait(0)
        compare(page.fullStyleColor, "#abcdef")
        verify(!page.hasExternalConflict)

        // A disconnect provides no replacement confirmed snapshot. It must
        // preserve the draft, avoid a false overlap, and block dispatch until
        // the display session is usable again.
        runtimeMock.displaySessionActive = false
        runtimeMock.displayStateValid = false
        runtimeMock.displayChanged()
        wait(0)
        compare(page.fullStyleColor, "#abcdef")
        verify(page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
        verify(!page.canApplyChanges)

        // Confirmed changes in clean domains merge around the local layout.
        runtimeMock.displaySessionActive = true
        runtimeMock.displayStateValid = true
        runtimeMock.brightness = 42
        runtimeMock.mirrorMode = true
        runtimeMock.displayChanged()
        wait(0)
        compare(page.fullStyleColor, "#abcdef")
        compare(page.brightnessDraft, 42)
        compare(page.mirrorDraft, true)
        verify(page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
    }

    function test_displayChangedMergesCleanLayoutAroundDirtyBrightness() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.brightnessDraft = 83
        wait(0)
        verify(page.hasUnsavedChanges)

        runtimeMock.displayedMedia =
            ["two.mp4.h264_2240x1080"]
        runtimeMock.currentPlayMode = "Loop"
        runtimeMock.displayLeftMetrics = ["GPU Temperature"]
        runtimeMock.displayLeftColor = "#445566"
        runtimeMock.displayChanged()
        wait(0)

        compare(page.brightnessDraft, 83)
        compare(page.selectedMedia,
                ["two.mp4.h264_2240x1080"])
        compare(page.playMode, "Loop")
        compare(page.fullMetrics, ["GPU Temperature"])
        compare(page.fullStyleColor, "#445566")
        verify(page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)

        // A different confirmed brightness overlaps the dirty domain.
        runtimeMock.brightness = 61
        runtimeMock.displayChanged()
        wait(0)
        compare(page.brightnessDraft, 83)
        verify(page.hasExternalConflict)
        verify(!page.canApplyChanges)
    }

    function test_overlappingUpdateSetsConflictWithoutClobberingDraft() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.fullStyleColor = "#abcdef"
        page.mirrorDraft = true
        wait(0)

        runtimeMock.displayLeftColor = "#654321"
        runtimeMock.mirrorMode = false
        runtimeMock.waterfallMode = true
        runtimeMock.displayChanged()
        wait(0)

        compare(page.fullStyleColor, "#abcdef")
        compare(page.mirrorDraft, true)
        compare(page.waterfallDraft, false)
        verify(page.hasUnsavedChanges)
        verify(page.hasExternalConflict)
        verify(!page.canApplyChanges)
    }

    function test_discardUsesLatestConfirmedStateWithoutDispatch() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.selectedMedia = ["two.mp4.h264_2240x1080"]
        page.fullStyleColor = "#abcdef"
        page.rightMetrics = ["CPU Temperature"]
        page.rightBadges = ["CPU Badge"]
        page.rightStylePosition = "Top"
        page.rightStyleColor = "#010203"
        page.rightStyleAlignment = "Left"
        page.brightnessDraft = 88
        page.mirrorDraft = true

        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.currentPlayMode = "Shuffle"
        runtimeMock.displayLeftMetrics = ["GPU Temperature"]
        runtimeMock.displayLeftColor = "#654321"
        runtimeMock.displayRightMetrics = ["Memory Usage"]
        runtimeMock.displayRightBadges = ["Memory Badge"]
        runtimeMock.displayRightPosition = "Bottom"
        runtimeMock.displayRightColor = "#112233"
        runtimeMock.displayRightAlignment = "Center"
        runtimeMock.brightness = 41
        runtimeMock.mirrorMode = false
        runtimeMock.waterfallMode = true
        runtimeMock.displayChanged()
        wait(0)
        verify(page.hasExternalConflict)

        page.discardChanges()
        wait(0)

        compare(page.selectedMedia,
                ["one.mp4.h264_2240x1080"])
        compare(page.playMode, "Shuffle")
        compare(page.fullMetrics, ["GPU Temperature"])
        compare(page.fullStyleColor, "#654321")
        compare(page.rightMetrics, ["Memory Usage"])
        compare(page.rightBadges, ["Memory Badge"])
        compare(page.rightStylePosition, "Bottom")
        compare(page.rightStyleColor, "#112233")
        compare(page.rightStyleAlignment, "Center")
        compare(page.brightnessDraft, 41)
        compare(page.mirrorDraft, false)
        compare(page.waterfallDraft, true)
        verify(!page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
        verify(!page.applyPending)
        verify(!page.applyUnresolved)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)
        compare(runtimeMock.brightnessSetCount, 0)
        compare(runtimeMock.orientationSetCount, 0)
    }

    function test_applyChangesSubmitsOneImmutableCombinedFullDraft() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics = ["CPU Temperature"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.fullStyleColor = "#abcdef"
        page.fullStyleAlignment = "Center"
        page.brightnessDraft = 88
        page.mirrorDraft = true
        page.waterfallDraft = true
        wait(0)
        verify(page.canApplyChanges)

        page.applyChanges()
        wait(0)

        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)
        compare(runtimeMock.brightnessSetCount, 0)
        compare(runtimeMock.orientationSetCount, 0)
        compare(runtimeMock.lastFullDraftSubmit[0],
                ["one.mp4.h264_2240x1080"])
        compare(runtimeMock.lastFullDraftSubmit[5], "#abcdef")
        compare(runtimeMock.lastFullDraftSubmit[6], "Center")
        compare(runtimeMock.lastFullDraftSubmit[7], true)
        compare(runtimeMock.lastFullDraftSubmit[8], true)
        compare(runtimeMock.lastFullDraftSubmit[9], 88)
        compare(runtimeMock.lastFullDraftSubmit[10], true)
        compare(runtimeMock.lastFullDraftSubmit[11], true)
        compare(runtimeMock.lastFullDraftSubmit[12], true)
        verify(page.applyPending)
        verify(!page.applyUnresolved)
        verify(!page.canApplyChanges)

        // A second call and later edits cannot mutate or duplicate the
        // captured submission.
        page.fullStyleColor = "#010203"
        page.brightnessDraft = 12
        page.selectedMedia[0] =
            "two.mp4.h264_2240x1080"
        page.fullMetrics[0] = "GPU Temperature"
        page.applyChanges()
        wait(0)
        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.lastFullDraftSubmit[0],
                ["one.mp4.h264_2240x1080"])
        compare(runtimeMock.lastFullDraftSubmit[2],
                ["CPU Temperature"])
        compare(runtimeMock.lastFullDraftSubmit[5], "#abcdef")
        compare(runtimeMock.lastFullDraftSubmit[9], 88)
    }

    function test_screenControlsOnlySubmitWithoutLayout() {
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        // No media is selected, but the unchanged layout is deliberately not
        // part of this brightness/orientation-only submission.
        page.brightnessDraft = 64
        page.mirrorDraft = true
        wait(0)
        verify(page.hasUnsavedChanges)
        verify(page.canApplyChanges)

        page.applyChanges()
        wait(0)

        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.lastFullDraftSubmit[7], false)
        compare(runtimeMock.lastFullDraftSubmit[8], true)
        compare(runtimeMock.lastFullDraftSubmit[9], 64)
        compare(runtimeMock.lastFullDraftSubmit[10], true)
        compare(runtimeMock.lastFullDraftSubmit[11], true)
        compare(runtimeMock.lastFullDraftSubmit[12], false)
        compare(runtimeMock.brightnessSetCount, 0)
        compare(runtimeMock.orientationSetCount, 0)
    }

    function test_splitApplyChangesSubmitsOneActiveLayout() {
        runtimeMock.currentScreenMode = "Screen Splitting"
        runtimeMock.displayedMedia = [
            "one.mp4.h264_2240x1080",
            "two.mp4.h264_2240x1080"
        ]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.rightStyleColor = "#abcdef"
        page.rightStyleAlignment = "Center"
        page.brightnessDraft = 69
        wait(0)
        verify(page.canApplyChanges)

        page.applyChanges()
        wait(0)

        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 1)
        compare(runtimeMock.lastSplitDraftSubmit[0],
                "one.mp4.h264_2240x1080")
        compare(runtimeMock.lastSplitDraftSubmit[1],
                "two.mp4.h264_2240x1080")
        compare(runtimeMock.lastSplitDraftSubmit[11], "#abcdef")
        compare(runtimeMock.lastSplitDraftSubmit[12], "Center")
        compare(runtimeMock.lastSplitDraftSubmit[13], true)
        compare(runtimeMock.lastSplitDraftSubmit[14], true)
        compare(runtimeMock.lastSplitDraftSubmit[15], 69)
        compare(runtimeMock.lastSplitDraftSubmit[16], false)
    }

    function test_applyCompletionIsCorrelatedAndUnknownStaysUnresolved() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        const finishedSpy = createTemporaryObject(
            signalSpyComponent, testCase,
            {"target": page, "signalName": "applyFinished"})
        verify(finishedSpy !== null)
        page.fullStyleColor = "#abcdef"
        wait(0)

        page.applyChanges()
        wait(0)
        verify(page.applyPending)

        runtimeMock.displayApplyFinished(
            "unrelated-submission", "Rejected", "unrelated")
        wait(0)
        verify(page.applyPending)
        compare(finishedSpy.count, 0)

        runtimeMock.displayApplyFinished(
            "display-submission-1", "PartialOrUnknown",
            "Device outcome is unknown")
        wait(0)
        verify(!page.applyPending)
        verify(page.applyUnresolved)
        verify(page.hasUnsavedChanges)
        verify(!page.canApplyChanges)
        compare(finishedSpy.count, 1)
        compare(finishedSpy.signalArguments[0][0],
                "PartialOrUnknown")
        compare(finishedSpy.signalArguments[0][1],
                "Device outcome is unknown")

        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 1)

        page.discardChanges()
        wait(0)
        compare(runtimeMock.abandonedDisplaySubmissionCount, 1)
        compare(runtimeMock.abandonedDisplaySubmissionId,
                "display-submission-1")
        verify(!page.applyUnresolved)
        verify(!page.hasUnsavedChanges)
    }

    function test_legacyCompositeAndMirrorAreBlockedBeforeSubmit() {
        runtimeMock.legacyConnected = true
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.fullStyleColor = "#abcdef"
        page.brightnessDraft = 64
        wait(0)
        verify(page.hasUnsavedChanges)
        verify(!page.canApplyChanges)
        verify(page.applyBlockReason.indexOf("legacy") >= 0)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 0)

        page.discardChanges()
        page.mirrorDraft = true
        wait(0)
        verify(page.hasUnsavedChanges)
        verify(!page.canApplyChanges)
        verify(page.applyBlockReason.indexOf("legacy") >= 0)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 0)

        page.discardChanges()
        page.waterfallDraft = true
        wait(0)
        verify(page.canApplyChanges)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.lastFullDraftSubmit[7], true)
        compare(runtimeMock.lastFullDraftSubmit[8], false)
        compare(runtimeMock.lastFullDraftSubmit[10], true)
        compare(runtimeMock.lastFullDraftSubmit[11], false)
        compare(runtimeMock.lastFullDraftSubmit[12], true)
    }

    function test_differentDeviceBlocksDraftUntilExplicitDiscard() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.fullStyleColor = "#abcdef"
        runtimeMock.displayDeviceIdentity = "device-b"
        runtimeMock.displayedMedia =
            ["two.mp4.h264_2240x1080"]
        runtimeMock.displayLeftColor = "#654321"
        runtimeMock.displayChanged()
        wait(0)

        compare(page.fullStyleColor, "#abcdef")
        verify(page.hasUnsavedChanges)
        verify(page.hasExternalConflict)
        verify(!page.canApplyChanges)
        verify(page.applyBlockReason.length > 0)

        // Repeated snapshots from the replacement device must not rebase the
        // old-device draft or silently clear the identity conflict.
        runtimeMock.displayChanged()
        wait(0)
        compare(page.fullStyleColor, "#abcdef")
        verify(page.hasUnsavedChanges)
        verify(page.hasExternalConflict)
        verify(!page.canApplyChanges)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 0)

        page.discardChanges()
        wait(0)
        compare(page.selectedMedia,
                ["two.mp4.h264_2240x1080"])
        compare(page.fullStyleColor, "#654321")
        verify(!page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
    }

    function test_succeededApplyFinishesAfterMatchingConfirmedState() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        const finishedSpy = createTemporaryObject(
            signalSpyComponent, testCase,
            {"target": page, "signalName": "applyFinished"})
        verify(finishedSpy !== null)
        page.fullStyleColor = "#abcdef"
        page.brightnessDraft = 66
        wait(0)

        page.applyChanges()
        wait(0)
        runtimeMock.displayLeftColor = "#ABCDEF"
        runtimeMock.brightness = 66
        runtimeMock.displayChanged()
        runtimeMock.displayApplyFinished(
            "display-submission-1", "Succeeded", "Applied")
        wait(0)

        verify(!page.applyPending)
        verify(!page.applyUnresolved)
        verify(!page.hasUnsavedChanges)
        verify(!page.hasExternalConflict)
        compare(finishedSpy.count, 1)
        compare(finishedSpy.signalArguments[0][0], "Succeeded")
        compare(finishedSpy.signalArguments[0][1], "Applied")
    }

    function test_metricSelectorsHaveIndependentCountersAndReflow() {
        runtimeMock.currentScreenMode = "Screen Splitting"
        runtimeMock.displayedMedia = [
            "one.mp4.h264_2240x1080",
            "two.mp4.h264_2240x1080"
        ]
        runtimeMock.displayLeftMetrics = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        runtimeMock.displayRightMetrics = [
            "GPU Temperature", "GPU Frequency", "GPU Usage"
        ]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        compare(findChild(page, "leftMetricCounter").text, "Left 3/3")
        compare(findChild(page, "rightMetricCounter").text, "Right 3/3")
        const grid = findChild(page, "splitMetricGrid")
        verify(grid !== null)
        compare(grid.columns, 2)

        page.width = 520
        wait(0)
        compare(grid.columns, 1)
        compare(findChild(page, "leftMetricCounter").text, "Left 3/3")
        compare(findChild(page, "rightMetricCounter").text, "Right 3/3")
    }

    function test_pendingFourthMetricDoesNotChangeDraftOrQueuedApply() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)
        const selector = findChild(page, "fullMetricSelector")
        verify(selector !== null)
        const original = page.fullMetrics.slice(0)

        page.brightnessDraft = 80
        wait(0)
        verify(page.hasUnsavedChanges)
        selector.requestToggle("GPU Temperature", null)
        tryVerify(() => selector.replacementOpen)
        compare(page.fullMetrics, original)
        verify(page.hasUnsavedChanges)
        compare(runtimeMock.fullDraftSubmitCount, 0)

        page.applyChanges()
        wait(0)
        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.lastFullDraftSubmit[2], original)
        compare(runtimeMock.lastFullDraftSubmit[7], false)
        compare(runtimeMock.lastFullDraftSubmit[8], true)
        compare(page.fullMetrics, original)
        tryVerify(() => !selector.replacementOpen)
    }

    function test_confirmedMetricReplacementUsesExactDraftPosition() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)
        const selector = findChild(page, "fullMetricSelector")
        verify(selector !== null)
        selector.requestToggle("GPU Temperature", null)
        tryVerify(() => selector.replacementOpen)

        const position = selector.replacementItemAt(2)
        verify(position !== null)
        mouseClick(position)
        tryVerify(() => !selector.replacementOpen)
        compare(page.fullMetrics, [
            "CPU Temperature", "CPU Frequency", "GPU Temperature"
        ])
        verify(page.hasUnsavedChanges)

        page.applyChanges()
        wait(0)
        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.lastFullDraftSubmit[2], [
            "CPU Temperature", "CPU Frequency", "GPU Temperature"
        ])
        compare(runtimeMock.lastFullDraftSubmit[7], true)
    }

    function test_metricDraftOverLimitIsBlockedInQml() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)
        page.fullMetrics = [
            "CPU Temperature", "CPU Frequency", "CPU Usage", "CPU Power"
        ]
        wait(0)

        verify(page.hasUnsavedChanges)
        verify(!page.canApplyChanges)
        page.applyChanges()
        compare(runtimeMock.fullDraftSubmitCount, 0)
    }

    function test_availabilityLossPreservesDraftDirtyStateAndApplyToken() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics = ["CPU Temperature"]
        runtimeMock.availableMetrics = [
            "CPU Temperature", "GPU Temperature"
        ]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)
        verify(!page.hasUnsavedChanges)
        compare(page.fullMetrics, ["CPU Temperature"])

        runtimeMock.availableMetrics = ["GPU Temperature"]
        runtimeMock.metricsChanged()
        wait(0)
        compare(page.fullMetrics, ["CPU Temperature"])
        verify(!page.hasUnsavedChanges)
        const selectedUnavailable = findChild(
            page, "fullMetricCpuTemperature")
        verify(selectedUnavailable !== null)
        const selector = findChild(page, "fullMetricSelector")
        verify(selector !== null)
        verify(selector.visibleMetricTokens.indexOf(
                   "CPU Temperature") >= 0)
        verify(selectedUnavailable.checked)
        verify(selectedUnavailable.enabled)
        verify(selectedUnavailable.Accessible.description.length > 0)

        page.brightnessDraft = 80
        wait(0)
        verify(page.hasUnsavedChanges)
        page.applyChanges()
        wait(0)
        compare(runtimeMock.fullDraftSubmitCount, 1)
        compare(runtimeMock.lastFullDraftSubmit[2],
                ["CPU Temperature"])
        compare(runtimeMock.lastFullDraftSubmit[7], false)
        compare(runtimeMock.lastFullDraftSubmit[8], true)
    }

    function test_saveCurrentCapturesFullDraftWithoutApplyOrDirtyReset() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        runtimeMock.displayLeftMetrics = ["CPU Temperature"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.playMode = "Loop"
        page.fullMetrics = [
            "GPU Temperature", "CPU Temperature"
        ]
        page.fullBadges = ["GPU Badge", "CPU Badge"]
        page.fullStylePosition = "Bottom"
        page.fullStyleColor = "#abcdef"
        page.fullStyleAlignment = "Center"
        page.brightnessDraft = 61
        page.mirrorDraft = true
        page.waterfallDraft = true
        wait(0)
        verify(page.hasUnsavedChanges)

        const nameField = findChild(
            page, "savedLayoutNameField")
        const saveButton = findChild(
            page, "saveCurrentLayoutButton")
        verify(nameField !== null)
        verify(saveButton !== null)
        nameField.text = "Travel layout"
        tryVerify(() => saveButton.enabled)
        page.saveCurrentLayout(nameField.text, "")
        wait(0)

        compare(runtimeMock.savedLayoutPutCount, 1)
        compare(runtimeMock.lastSavedLayoutPut[0],
                "Travel layout")
        compare(runtimeMock.lastSavedLayoutPut[1], "")
        const fullState = runtimeMock.lastSavedLayoutPut[2]
        compare(fullState.layout.split, false)
        compare(fullState.layout.media,
                ["one.mp4.h264_2240x1080"])
        compare(fullState.layout.playMode, "Loop")
        compare(fullState.layout.metrics,
                ["GPU Temperature", "CPU Temperature"])
        compare(fullState.layout.badges,
                ["GPU Badge", "CPU Badge"])
        compare(fullState.layout.position, "Bottom")
        compare(fullState.layout.color, "#abcdef")
        compare(fullState.layout.alignment, "Center")
        compare(fullState.brightness, 61)
        compare(fullState.orientation.mirror, true)
        compare(fullState.orientation.waterfall, true)
        verify(page.hasUnsavedChanges)
        compare(runtimeMock.savedLayoutSubmitCount, 0)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)

        runtimeMock.savedLayoutPutFinished(
            "", "layout-new", "21", true, "")
        wait(0)
        verify(!page.hasSavedLayoutBinding)
        compare(page.loadedSavedLayoutId, "")
    }

    function test_loadFullAndSplitPreservesEveryOrderedFieldInDraft() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.loadSavedLayoutIntoDraft("layout-full")
        wait(0)
        compare(runtimeMock.savedLayoutDraftCount, 1)
        compare(page.loadedSavedLayoutId, "layout-full")
        compare(page.loadedSavedLayoutRevision, "11")
        compare(page.loadedSavedLayoutDeviceIdentity, "device-a")
        compare(page.splitMode, false)
        compare(page.selectedMedia,
                ["two.mp4.h264_2240x1080"])
        compare(page.playMode, "Shuffle")
        compare(page.fullMetrics,
                ["GPU Usage", "CPU Temperature"])
        compare(page.fullBadges,
                ["GPU Badge", "CPU Badge"])
        compare(page.fullStylePosition, "Bottom")
        compare(page.fullStyleColor, "#123abc")
        compare(page.fullStyleAlignment, "Center")
        compare(page.brightnessDraft, 64)
        compare(page.mirrorDraft, true)
        compare(page.waterfallDraft, false)
        verify(page.hasUnsavedChanges)

        page.loadSavedLayoutIntoDraft("layout-split")
        wait(0)
        compare(runtimeMock.savedLayoutDraftCount, 2)
        compare(page.loadedSavedLayoutId, "layout-split")
        compare(page.loadedSavedLayoutRevision, "19")
        compare(page.splitMode, true)
        compare(page.selectedMedia, [
            "two.mp4.h264_2240x1080",
            "one.mp4.h264_2240x1080"
        ])
        compare(page.playMode, "Single")
        compare(page.leftMetrics,
                ["GPU Temperature", "CPU Usage"])
        compare(page.rightMetrics,
                ["Memory Usage", "CPU Frequency"])
        compare(page.leftBadges,
                ["GPU Badge", "CPU Badge"])
        compare(page.rightBadges,
                ["CPU Badge", "GPU Badge"])
        compare(page.leftStylePosition, "Bottom")
        compare(page.leftStyleColor, "#010203")
        compare(page.leftStyleAlignment, "Right")
        compare(page.rightStylePosition, "Top")
        compare(page.rightStyleColor, "#a0b0c0")
        compare(page.rightStyleAlignment, "Center")
        compare(page.brightnessDraft, 37)
        compare(page.mirrorDraft, false)
        compare(page.waterfallDraft, true)
        verify(page.hasUnsavedChanges)
        compare(runtimeMock.savedLayoutSubmitCount, 0)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 0)
    }

    function test_editedLoadedDraftKeepsProvenanceForApply() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.loadSavedLayoutIntoDraft("layout-full")
        page.selectedMedia = ["one.mp4.h264_2240x1080"]
        page.fullMetrics = [
            "CPU Temperature", "GPU Temperature"
        ]
        page.fullBadges = ["CPU Badge"]
        page.fullStylePosition = "Top"
        page.fullStyleColor = "#fedcba"
        page.fullStyleAlignment = "Right"
        page.brightnessDraft = 42
        page.mirrorDraft = false
        page.waterfallDraft = true
        wait(0)

        compare(page.loadedSavedLayoutId, "layout-full")
        compare(page.loadedSavedLayoutRevision, "11")
        verify(page.hasSavedLayoutBinding)
        verify(page.canApplyChanges)
        page.applyChanges()
        wait(0)

        compare(runtimeMock.savedLayoutSubmitCount, 1)
        compare(runtimeMock.lastSavedLayoutSubmit[0],
                "layout-full")
        compare(runtimeMock.lastSavedLayoutSubmit[1], "11")
        const fullState = runtimeMock.lastSavedLayoutSubmit[2]
        compare(fullState.layout.split, false)
        compare(fullState.layout.media,
                ["one.mp4.h264_2240x1080"])
        compare(fullState.layout.metrics,
                ["CPU Temperature", "GPU Temperature"])
        compare(fullState.layout.badges, ["CPU Badge"])
        compare(fullState.layout.position, "Top")
        compare(fullState.layout.color, "#fedcba")
        compare(fullState.layout.alignment, "Right")
        compare(fullState.brightness, 42)
        compare(fullState.orientation.mirror, false)
        compare(fullState.orientation.waterfall, true)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)
        verify(page.applyPending)
        compare(page.activeDisplaySubmissionId,
                "saved-layout-submission-1")
    }

    function test_bindingClearsOnlyOnExplicitLifecycleBoundaries() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.loadSavedLayoutIntoDraft("layout-full")
        verify(page.hasSavedLayoutBinding)
        page.discardChanges()
        wait(0)
        verify(!page.hasSavedLayoutBinding)
        compare(page.selectedMedia,
                ["one.mp4.h264_2240x1080"])

        page.loadSavedLayoutIntoDraft("layout-full")
        verify(page.hasSavedLayoutBinding)
        runtimeMock.runtimeInvalidated()
        wait(0)
        verify(!page.hasSavedLayoutBinding)

        page.loadSavedLayoutIntoDraft("layout-full")
        verify(page.hasSavedLayoutBinding)
        runtimeMock.savedLayoutsDeviceIdentity = "device-b"
        runtimeMock.savedLayoutsChanged()
        wait(0)
        verify(!page.hasSavedLayoutBinding)

        runtimeMock.savedLayoutsDeviceIdentity = "device-a"
        page.loadSavedLayoutIntoDraft("layout-full")
        verify(page.hasSavedLayoutBinding)
        runtimeMock.savedLayoutDrafts = ({
            "layout-split": splitSavedLayoutDraft()
        })
        runtimeMock.savedLayoutsChanged()
        wait(0)
        verify(!page.hasSavedLayoutBinding)

        runtimeMock.savedLayoutDrafts = ({
            "layout-full": fullSavedLayoutDraft(),
            "layout-split": splitSavedLayoutDraft()
        })
        page.loadSavedLayoutIntoDraft("layout-full")
        verify(page.hasSavedLayoutBinding)
        runtimeMock.savedLayoutDeleteFinished(
            "layout-full", true, "Deleted")
        wait(0)
        verify(!page.hasSavedLayoutBinding)
        compare(runtimeMock.savedLayoutSubmitCount, 0)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 0)
    }

    function test_savedLayoutActionsFailClosedForApplyAndConflict() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.applyPending = true
        verify(!page.savedLayoutActionsAllowed())
        verify(page.savedLayoutActionsBlockReason()
               .indexOf("Apply") >= 0)
        page.saveCurrentLayout("Blocked pending", "")
        page.loadSavedLayoutIntoDraft("layout-full")
        compare(runtimeMock.savedLayoutPutCount, 0)
        compare(runtimeMock.savedLayoutDraftCount, 0)

        page.applyPending = false
        page.applyUnresolved = true
        verify(!page.savedLayoutActionsAllowed())
        verify(page.savedLayoutActionsBlockReason()
               .indexOf("Apply") >= 0)
        page.saveCurrentLayout("Blocked unresolved", "")
        page.loadSavedLayoutIntoDraft("layout-full")
        compare(runtimeMock.savedLayoutPutCount, 0)
        compare(runtimeMock.savedLayoutDraftCount, 0)

        page.applyUnresolved = false
        page.loadSavedLayoutIntoDraft("layout-full")
        compare(runtimeMock.savedLayoutDraftCount, 1)
        runtimeMock.displayLeftColor = "#654321"
        runtimeMock.displayChanged()
        wait(0)
        verify(page.hasExternalConflict)
        verify(!page.savedLayoutActionsAllowed())
        verify(page.savedLayoutActionsBlockReason()
               .indexOf("conflicted") >= 0)
        page.saveCurrentLayout("Blocked conflict", "")
        page.loadSavedLayoutIntoDraft("layout-split")
        compare(runtimeMock.savedLayoutPutCount, 0)
        compare(runtimeMock.savedLayoutDraftCount, 1)
        compare(runtimeMock.savedLayoutSubmitCount, 0)
    }

    function test_reconnectNeverLoadsOrAppliesSavedLayout() {
        runtimeMock.displayedMedia =
            ["one.mp4.h264_2240x1080"]
        const page = createTemporaryObject(
            panoramaComponent, testCase,
            {"width": 1000, "height": 700})
        verify(page !== null)
        wait(0)

        page.loadSavedLayoutIntoDraft("layout-full")
        verify(page.hasSavedLayoutBinding)
        compare(runtimeMock.savedLayoutDraftCount, 1)
        runtimeMock.savedLayoutDraftCount = 0

        runtimeMock.savedLayoutsReady = false
        runtimeMock.savedLayoutsStatus = "Disconnected"
        runtimeMock.savedLayoutsDeviceIdentity = ""
        runtimeMock.displaySessionActive = false
        runtimeMock.connectionChanged()
        runtimeMock.savedLayoutsChanged()
        wait(0)
        verify(!page.hasSavedLayoutBinding)

        runtimeMock.savedLayoutsReady = true
        runtimeMock.savedLayoutsStatus = "Ready"
        runtimeMock.savedLayoutsDeviceIdentity = "device-a"
        runtimeMock.displaySessionActive = true
        runtimeMock.connectionChanged()
        runtimeMock.savedLayoutsChanged()
        wait(0)

        compare(runtimeMock.savedLayoutDraftCount, 0)
        compare(runtimeMock.savedLayoutPutCount, 0)
        compare(runtimeMock.savedLayoutDeleteCount, 0)
        compare(runtimeMock.savedLayoutSubmitCount, 0)
        compare(runtimeMock.fullDraftSubmitCount, 0)
        compare(runtimeMock.splitDraftSubmitCount, 0)
        compare(runtimeMock.fullApplyCount, 0)
        compare(runtimeMock.splitApplyCount, 0)
        verify(!page.applyPending)
    }
}
