pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase
    name: "MediaEditorLayout"
    when: host.visible

    ApplicationWindow {
        id: host
        width: 1060
        height: 700
        visible: true

        QtObject {
            id: controller

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
            property int targetWidth: 2240
            property int targetHeight: 1080

            function reset() {}
            function cancel() {}
            function submit() {}
            function submitSaveAsNew() {}
            function submitReplace() {}
        }

        Components.MediaEditor {
            id: editor
            controller: controller
        }
    }

    function init() {
        controller.recoveredDeviceCopy = false
        controller.sourceKind = "LocalMedia"
        controller.originalMediaName = ""
        controller.replaceAllowed = false
        controller.replaceBlockReason = ""
        controller.submissionPending = false
        controller.submissionAction = ""
        controller.targetWidth = 2240
        controller.targetHeight = 1080
        controller.open = false
        wait(0)
        controller.open = true
        tryVerify(() => editor.opened)
    }

    function cleanup() {
        controller.open = false
        wait(0)
    }

    function test_contentIsVisibleAfterClosedToOpenTransition() {
        const content = findChild(editor, "mediaEditorContent")
        verify(content !== null)
        verify(content.visible)
        verify(content.opacity > 0)
        verify(content.width > 0)
        verify(content.height > 0)
        compare(editor.contentItem, content)
    }

    function test_previewCanvasKeepsDeviceAspectRatio() {
        const canvas = findChild(editor, "mediaPreviewCanvas")
        const resolution =
            findChild(editor, "mediaTargetResolution")
        verify(canvas !== null)
        verify(resolution !== null)
        verify(canvas.width > 0)
        verify(canvas.height > 0)
        const profiles = [
            {"width": 2240, "height": 1080},
            {"width": 1280, "height": 720}
        ]
        for (const profile of profiles) {
            controller.targetWidth = profile.width
            controller.targetHeight = profile.height
            wait(0)
            const expectedRatio =
                profile.width / profile.height
            verify(Math.abs(canvas.width / canvas.height -
                            expectedRatio) < 0.001)
            verify(canvas.width <= 1000)
            verify(canvas.height <= 420)
            verify(resolution.text.indexOf(
                       profile.width.toString()) >= 0)
            verify(resolution.text.indexOf(
                       profile.height.toString()) >= 0)
        }
        verify(editor.width <= host.width - 32)
        verify(editor.height <= host.height - 32)
    }

    function test_sizingModesExplainTheirEffect() {
        const description =
            findChild(editor, "mediaSizingDescription")
        const horizontal =
            findChild(editor, "cropHorizontalPosition")
        const vertical =
            findChild(editor, "cropVerticalPosition")

        compare(editor.sizingModes.length, 4)
        compare(editor.sizingModes[0].value, "Fit")
        compare(editor.sizingModes[1].value, "Fill")
        compare(editor.sizingModes[2].value, "Crop")
        compare(editor.sizingModes[3].value, "Stretch")
        verify(description !== null)
        verify(horizontal !== null)
        verify(vertical !== null)

        controller.mode = "Stretch"
        wait(0)
        verify(description.text.indexOf("distorted") >= 0)

        controller.mode = "Crop"
        controller.zoomPercent = 100
        controller.focusX = 2500
        controller.focusY = 7500
        wait(0)
        compare(horizontal.value, 25)
        compare(vertical.value, 75)
        verify(!horizontal.enabled)
        verify(!vertical.enabled)

        controller.zoomPercent = 160
        wait(0)
        verify(horizontal.enabled)
        verify(vertical.enabled)

        controller.mode = "Fit"
        wait(0)
        verify(!horizontal.enabled)
        verify(!vertical.enabled)
    }

    function test_recoveredCopyUsesExplicitActions() {
        controller.recoveredDeviceCopy = true
        controller.sourceKind = "RecoveredDeviceCopy"
        controller.originalMediaName = "device-video.h264_2240x1080"
        controller.replaceAllowed = true
        wait(0)

        const notice =
            findChild(editor, "recoveredDeviceCopyNotice")
        const noticeText =
            findChild(editor, "recoveredDeviceCopyNoticeText")
        const upload =
            findChild(editor, "mediaEditorUploadButton")
        const saveAsNew =
            findChild(editor, "mediaEditorSaveAsNewButton")
        const replace =
            findChild(editor, "mediaEditorReplaceButton")

        verify(notice !== null)
        verify(notice.visible)
        verify(noticeText !== null)
        verify(noticeText.text.indexOf("2240") >= 0)
        verify(noticeText.text.indexOf("1080") >= 0)
        verify(noticeText.text.indexOf("Crop") >= 0)
        verify(noticeText.text.indexOf("Zoom") >= 0)
        verify(noticeText.text.indexOf(
                   "does not change the active display") >= 0)
        verify(editor.recoveredTransformIsGeometryNeutral)
        verify(upload !== null)
        verify(!upload.visible)
        verify(saveAsNew !== null)
        verify(saveAsNew.visible)
        verify(saveAsNew.enabled)
        verify(replace !== null)
        verify(replace.visible)
        verify(replace.enabled)

        controller.targetWidth = 1280
        controller.targetHeight = 720
        wait(0)
        verify(noticeText.text.indexOf("2240") >= 0)
        verify(noticeText.text.indexOf("1080") >= 0)
        verify(noticeText.text.indexOf("1280") < 0)

        controller.submissionPending = true
        controller.submissionAction = "Replace"
        wait(0)
        verify(!saveAsNew.enabled)
        verify(!replace.enabled)
        verify(replace.text.indexOf("Replacing") >= 0)
    }

    function test_actionRowRemainsVisibleInShortWindow() {
        controller.recoveredDeviceCopy = true
        controller.replaceAllowed = true
        wait(0)

        const content =
            findChild(editor, "mediaEditorContent")
        const actionRow =
            findChild(editor, "mediaEditorActionRow")
        const saveAsNew =
            findChild(editor, "mediaEditorSaveAsNewButton")
        verify(content !== null)
        verify(actionRow !== null)
        verify(saveAsNew !== null)
        verify(actionRow.visible)
        verify(saveAsNew.visible)

        const top = actionRow.mapToItem(content, 0, 0)
        const bottom = actionRow.mapToItem(
            content, 0, actionRow.height)
        verify(top.y >= 0)
        verify(bottom.y <= content.height + 0.5)
    }

    function test_recoveredCopyWarnsOnlyForNeutralGeometry() {
        controller.recoveredDeviceCopy = true
        controller.mode = "Crop"
        controller.zoomPercent = 100
        controller.rotation = 0
        wait(0)

        const noticeText =
            findChild(editor, "recoveredDeviceCopyNoticeText")
        verify(noticeText !== null)
        verify(editor.recoveredTransformIsGeometryNeutral)
        verify(noticeText.text.indexOf(
                   "will not visibly change") >= 0)

        controller.zoomPercent = 160
        wait(0)
        verify(!editor.recoveredTransformIsGeometryNeutral)
        verify(noticeText.text.indexOf(
                   "Save as new stores") >= 0)

        controller.mode = "Stretch"
        controller.zoomPercent = 100
        controller.rotation = 180
        wait(0)
        verify(!editor.recoveredTransformIsGeometryNeutral)
    }
}
