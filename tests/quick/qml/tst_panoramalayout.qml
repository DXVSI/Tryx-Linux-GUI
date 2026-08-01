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
            thumbnailUrl: ""
            deleteAllowed: true
            deleteBlockReason: ""
            deviceCopyAllowed: true
            deviceCopyBlockReason: ""
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
        property string activeOperationId: ""
        property string operationSummary: ""
        property real operationProgress: 0
        property var mediaModel: mediaModel
        property var operationModel: operationModel
        property var availableMetrics: [
            "CPU Temperature", "GPU Temperature"
        ]
        property int brightness: 75
        property bool mirrorMode: false
        property bool waterfallMode: false
        property bool displayStateValid: true
        property bool backlightEnabled: true
        property string currentScreenMode: "Full Screen"
        property string currentPlayMode: "Single"
        property var displayedMedia: []
        property var displayLeftMetrics: []
        property var displayRightMetrics: []
        property var displayLeftBadges: ["CPU Badge"]
        property var displayRightBadges: ["GPU Badge"]
        property var activeMetrics: []
        property string metricsAlignment: "Left"
        property string metricsColor: "#dcdcdc"
        property bool metricsEnabled: false
        property bool samplingActive: false
        property string diagnostic: ""

        signal displayChanged()
        signal metricsChanged()

        function refreshMedia() {}
        function deleteMedia(media) {}
        function applyFullScreen(media, playMode,
                                 metrics, badges) {}
        function applySplitScreen(left, right, playMode,
                                  leftMetrics, rightMetrics,
                                  leftBadges, rightBadges) {}
        function configureMetrics(enabled, metrics,
                                  alignment, color) {}
        function setBrightness(value) {}
        function setBacklight(enabled) {}
        function setOrientation(mirror, waterfall) {}
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
}
