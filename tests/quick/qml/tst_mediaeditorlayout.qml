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
            property string preparationTarget: "FullFrame"
            property bool splitTargetAvailable: true
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
        host.width = 1060
        host.height = 700
        controller.recoveredDeviceCopy = false
        controller.sourceKind = "LocalMedia"
        controller.originalMediaName = ""
        controller.replaceAllowed = false
        controller.replaceBlockReason = ""
        controller.submissionPending = false
        controller.submissionAction = ""
        controller.targetWidth = 2240
        controller.targetHeight = 1080
        controller.mode = "Fit"
        controller.zoomPercent = 100
        controller.focusX = 5000
        controller.focusY = 5000
        controller.rotation = 0
        controller.backgroundColor = "#000000"
        controller.preparationTarget = "FullFrame"
        controller.splitTargetAvailable = true
        controller.deviceCopyMetadataStatus = "NotSupported"
        controller.deviceCopyDimensionsAvailable = false
        controller.deviceCopyWidth = 0
        controller.deviceCopyHeight = 0
        controller.deviceCopyDurationAvailable = false
        controller.deviceCopyDurationMilliseconds = 0
        controller.deviceCopyFrameRateAvailable = false
        controller.deviceCopyFrameRateNumerator = 0
        controller.deviceCopyFrameRateDenominator = 0
        controller.open = false
        wait(0)
        controller.open = true
        tryVerify(() => editor.opened)
    }

    function setReadyDeviceCopyMetadata() {
        controller.deviceCopyMetadataStatus = "Ready"
        controller.deviceCopyDimensionsAvailable = true
        controller.deviceCopyWidth = 2240
        controller.deviceCopyHeight = 1080
        controller.deviceCopyDurationAvailable = true
        controller.deviceCopyDurationMilliseconds = 60000
        controller.deviceCopyFrameRateAvailable = true
        controller.deviceCopyFrameRateNumerator = 30
        controller.deviceCopyFrameRateDenominator = 1
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

    function test_preparationTargetSelectorUsesActualAreaCanvas() {
        const group = findChild(editor, "mediaPreparationTargetGroup")
        const choices = findChild(editor, "mediaPreparationTargetChoices")
        const full = findChild(editor, "fullFramePreparationTarget")
        const split = findChild(editor, "splitAreaPreparationTarget")
        const description =
            findChild(editor, "mediaPreparationTargetDescription")
        const warning =
            findChild(editor, "mediaPreparationTargetResetWarning")
        const unavailable =
            findChild(editor, "splitAreaPreparationUnavailable")
        const canvas = findChild(editor, "mediaPreviewCanvas")
        const resolution = findChild(editor, "mediaTargetResolution")

        for (const item of [group, choices, full, split, description,
                            warning, unavailable, canvas, resolution]) {
            verify(item !== null)
        }
        compare(group.title, "Prepare for")
        compare(group.Accessible.name, "Prepare for")
        verify(group.Accessible.description.indexOf(
                   "Changing the target") >= 0)
        compare(full.text, "Full screen")
        compare(split.text, "Split area")
        compare(full.Accessible.name, full.text)
        compare(split.Accessible.name, split.text)
        verify(full.activeFocusOnTab)
        verify(split.activeFocusOnTab)
        verify(full.checked)
        verify(!split.checked)
        verify(full.enabled)
        verify(split.enabled)
        verify(!unavailable.visible)
        compare(description.textFormat, Text.PlainText)
        compare(warning.textFormat, Text.PlainText)
        verify(description.text.indexOf("full display") >= 0)
        verify(description.text.indexOf("2240 × 1080") >= 0)
        verify(description.text.indexOf("does not apply") >= 0)
        verify(warning.text.indexOf("resets all editing settings") >= 0)
        verify(Math.abs(canvas.width / canvas.height -
                        2240 / 1080) < 0.001)
        verify(resolution.text.indexOf("Full screen") >= 0)
        verify(resolution.text.indexOf("2240 × 1080") >= 0)
        compare(resolution.textFormat, Text.PlainText)

        host.requestActivate()
        tryVerify(() => host.active)
        split.forceActiveFocus()
        tryVerify(() => split.activeFocus)
        keyClick(Qt.Key_Space)
        compare(controller.preparationTarget, "SplitArea")
        controller.targetWidth = 1120
        controller.targetHeight = 1080
        wait(0)

        verify(!full.checked)
        verify(split.checked)
        verify(description.text.indexOf("left or right side") >= 0)
        verify(description.text.indexOf("1120 × 1080") >= 0)
        verify(description.text.indexOf("does not choose a side") >= 0)
        verify(description.text.indexOf("apply changes") >= 0)
        verify(Math.abs(canvas.width / canvas.height -
                        1120 / 1080) < 0.001)
        verify(resolution.text.indexOf("Split area") >= 0)
        verify(resolution.text.indexOf("1120 × 1080") >= 0)

        controller.submissionPending = true
        wait(0)
        verify(!full.enabled)
        verify(!split.enabled)
    }

    function test_unavailableSplitTargetHasVisibleAccessibleReason() {
        controller.splitTargetAvailable = false
        wait(0)

        const full = findChild(editor, "fullFramePreparationTarget")
        const split = findChild(editor, "splitAreaPreparationTarget")
        const unavailable =
            findChild(editor, "splitAreaPreparationUnavailable")
        verify(full !== null)
        verify(split !== null)
        verify(unavailable !== null)
        verify(full.enabled)
        verify(!split.enabled)
        verify(unavailable.visible)
        compare(unavailable.textFormat, Text.PlainText)
        verify(unavailable.text.indexOf("not available") >= 0)
        compare(unavailable.Accessible.name, unavailable.text)
        compare(split.Accessible.description, unavailable.text)
    }

    function test_preparationTargetSelectorReflowsAtNarrowWidth() {
        controller.splitTargetAvailable = false
        host.width = 360
        host.height = 640
        wait(0)

        const content = findChild(editor, "mediaEditorContent")
        const group = findChild(editor, "mediaPreparationTargetGroup")
        const choices = findChild(editor, "mediaPreparationTargetChoices")
        const full = findChild(editor, "fullFramePreparationTarget")
        const split = findChild(editor, "splitAreaPreparationTarget")
        const description =
            findChild(editor, "mediaPreparationTargetDescription")
        const warning =
            findChild(editor, "mediaPreparationTargetResetWarning")
        const unavailable =
            findChild(editor, "splitAreaPreparationUnavailable")
        const canvas = findChild(editor, "mediaPreviewCanvas")
        const resolution = findChild(editor, "mediaTargetResolution")
        for (const item of [content, group, choices, full, split,
                            description, warning, unavailable, canvas,
                            resolution]) {
            verify(item !== null)
        }
        compare(choices.columns, 1)
        verify(editor.width <= host.width - 32)
        verify(group.width <= content.width + 0.5)
        for (const item of [group, full, split, description, warning,
                            unavailable]) {
            const left = item.mapToItem(content, 0, 0)
            const right = item.mapToItem(content, item.width, 0)
            verify(left.x >= -0.5)
            verify(right.x <= content.width + 0.5)
        }
        const resolutionLeft = resolution.mapToItem(canvas, 0, 0)
        const resolutionRight =
            resolution.mapToItem(canvas, resolution.width, 0)
        verify(resolutionLeft.x >= -0.5)
        verify(resolutionRight.x <= canvas.width + 0.5)
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
        setReadyDeviceCopyMetadata()
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

    function test_deviceCopyMetadataStates_data() {
        return [
            {"tag": "loading", "status": "Loading",
             "dimensions": false, "duration": false,
             "frameRate": false, "expectedState": "Loading",
             "expectedDimensions": "Unavailable",
             "expectedDuration": "Unavailable",
             "expectedFrameRate": "Unavailable"},
            {"tag": "ready", "status": "Ready",
             "dimensions": true, "duration": true,
             "frameRate": true, "expectedState": "Ready",
             "expectedDimensions": "2240 × 1080",
             "expectedDuration": "01:00",
             "expectedFrameRate": "30 FPS"},
            {"tag": "partial", "status": "Partial",
             "dimensions": true, "duration": false,
             "frameRate": false, "expectedState": "Partial",
             "expectedDimensions": "2240 × 1080",
             "expectedDuration": "Unavailable",
             "expectedFrameRate": "Unavailable"},
            {"tag": "unavailable", "status": "Unavailable",
             "dimensions": false, "duration": false,
             "frameRate": false, "expectedState": "Unavailable",
             "expectedDimensions": "Unavailable",
             "expectedDuration": "Unavailable",
             "expectedFrameRate": "Unavailable"},
            {"tag": "not-supported", "status": "NotSupported",
             "dimensions": false, "duration": false,
             "frameRate": false, "expectedState": "Not supported",
             "expectedDimensions": "Unavailable",
             "expectedDuration": "Unavailable",
             "expectedFrameRate": "Unavailable"}
        ]
    }

    function test_deviceCopyMetadataStates(data) {
        controller.recoveredDeviceCopy = true
        controller.sourceKind = "RecoveredDeviceCopy"
        controller.originalMediaName =
            "device-copy.mp4.h264_1280x720"
        controller.deviceCopyMetadataStatus = data.status
        controller.deviceCopyDimensionsAvailable = data.dimensions
        controller.deviceCopyWidth = data.dimensions ? 2240 : 0
        controller.deviceCopyHeight = data.dimensions ? 1080 : 0
        controller.deviceCopyDurationAvailable = data.duration
        controller.deviceCopyDurationMilliseconds =
            data.duration ? 60000 : 0
        controller.deviceCopyFrameRateAvailable = data.frameRate
        controller.deviceCopyFrameRateNumerator =
            data.frameRate ? 30 : 0
        controller.deviceCopyFrameRateDenominator =
            data.frameRate ? 1 : 0
        wait(0)

        const metadata = findChild(editor, "deviceCopyMetadata")
        const title = findChild(editor, "deviceCopyMetadataTitle")
        const state = findChild(editor, "deviceCopyMetadataState")
        const dimensionsLabel =
            findChild(editor, "deviceCopyDimensionsLabel")
        const dimensions =
            findChild(editor, "deviceCopyDimensionsValue")
        const durationLabel =
            findChild(editor, "deviceCopyDurationLabel")
        const duration = findChild(editor, "deviceCopyDurationValue")
        const frameRateLabel =
            findChild(editor, "deviceCopyFrameRateLabel")
        const frameRate =
            findChild(editor, "deviceCopyFrameRateValue")
        verify(metadata !== null)
        verify(metadata.visible)
        verify(title !== null)
        verify(state !== null)
        verify(dimensionsLabel !== null)
        verify(dimensions !== null)
        verify(durationLabel !== null)
        verify(duration !== null)
        verify(frameRateLabel !== null)
        verify(frameRate !== null)
        compare(title.text, "Device copy metadata")
        compare(dimensionsLabel.text, "Dimensions")
        compare(durationLabel.text, "Duration")
        compare(frameRateLabel.text, "Frame rate")
        compare(state.text, data.expectedState)
        compare(dimensions.text, data.expectedDimensions)
        compare(duration.text, data.expectedDuration)
        compare(frameRate.text, data.expectedFrameRate)
        compare(title.textFormat, Text.PlainText)
        compare(state.textFormat, Text.PlainText)
        compare(dimensionsLabel.textFormat, Text.PlainText)
        compare(dimensions.textFormat, Text.PlainText)
        compare(durationLabel.textFormat, Text.PlainText)
        compare(duration.textFormat, Text.PlainText)
        compare(frameRateLabel.textFormat, Text.PlainText)
        compare(frameRate.textFormat, Text.PlainText)
    }

    function test_deviceCopyMetadataIsHiddenForLocalMedia() {
        controller.recoveredDeviceCopy = false
        controller.sourceKind = "LocalMedia"
        setReadyDeviceCopyMetadata()
        wait(0)

        const metadata = findChild(editor, "deviceCopyMetadata")
        verify(metadata !== null)
        verify(!metadata.visible)
    }

    function test_deviceCopyMetadataFormatsLongDuration() {
        controller.recoveredDeviceCopy = true
        controller.sourceKind = "RecoveredDeviceCopy"
        setReadyDeviceCopyMetadata()
        controller.deviceCopyDurationMilliseconds = 3723000
        wait(0)

        const duration = findChild(editor, "deviceCopyDurationValue")
        const frameRate =
            findChild(editor, "deviceCopyFrameRateValue")
        verify(duration !== null)
        verify(frameRate !== null)
        compare(duration.text, "01:02:03")
        compare(frameRate.text, "30 FPS")
    }

    function test_recoveredResolutionUsesProbeNotFilenameOrTarget() {
        controller.recoveredDeviceCopy = true
        controller.sourceKind = "RecoveredDeviceCopy"
        controller.originalMediaName =
            "misleading-name.mp4.h264_1280x720"
        controller.targetWidth = 800
        controller.targetHeight = 480
        setReadyDeviceCopyMetadata()
        wait(0)

        const noticeText =
            findChild(editor, "recoveredDeviceCopyNoticeText")
        verify(noticeText !== null)
        verify(noticeText.text.indexOf("2240") >= 0)
        verify(noticeText.text.indexOf("1080") >= 0)
        verify(noticeText.text.indexOf("1280") < 0)
        verify(noticeText.text.indexOf("720") < 0)
        verify(noticeText.text.indexOf("800") < 0)
        verify(noticeText.text.indexOf("480") < 0)

        controller.deviceCopyMetadataStatus = "Partial"
        controller.deviceCopyDimensionsAvailable = false
        controller.deviceCopyWidth = 0
        controller.deviceCopyHeight = 0
        wait(0)
        verify(noticeText.text.indexOf("1280") < 0)
        verify(noticeText.text.indexOf("720") < 0)
        verify(noticeText.text.indexOf("800") < 0)
        verify(noticeText.text.indexOf("480") < 0)
    }

    function test_deviceCopyMetadataIsNarrowAndAccessible() {
        controller.recoveredDeviceCopy = true
        controller.sourceKind = "RecoveredDeviceCopy"
        controller.originalMediaName = "device-copy.h264"
        setReadyDeviceCopyMetadata()
        host.width = 360
        host.height = 640
        wait(0)

        const content = findChild(editor, "mediaEditorContent")
        const metadata = findChild(editor, "deviceCopyMetadata")
        const state = findChild(editor, "deviceCopyMetadataState")
        const dimensions =
            findChild(editor, "deviceCopyDimensionsValue")
        const duration = findChild(editor, "deviceCopyDurationValue")
        const frameRate =
            findChild(editor, "deviceCopyFrameRateValue")
        verify(content !== null)
        verify(metadata !== null)
        verify(state !== null)
        verify(dimensions !== null)
        verify(duration !== null)
        verify(frameRate !== null)
        verify(editor.width <= host.width - 32)
        verify(metadata.width <= content.width + 0.5)

        for (const value of [state, dimensions, duration, frameRate]) {
            const left = value.mapToItem(content, 0, 0)
            const right = value.mapToItem(content, value.width, 0)
            verify(left.x >= -0.5)
            verify(right.x <= content.width + 0.5)
            verify(value.visible)
        }

        verify(metadata.Accessible.description.indexOf("Dimensions") >= 0)
        verify(metadata.Accessible.description.indexOf("2240 × 1080") >= 0)
        verify(metadata.Accessible.description.indexOf("Duration") >= 0)
        verify(metadata.Accessible.description.indexOf("01:00") >= 0)
        verify(metadata.Accessible.description.indexOf("Frame rate") >= 0)
        verify(metadata.Accessible.description.indexOf("30 FPS") >= 0)
    }

    function test_deviceCopyMetadataRussianTranslation() {
        controller.recoveredDeviceCopy = true
        controller.sourceKind = "RecoveredDeviceCopy"
        controller.originalMediaName = "device-copy.h264"
        setReadyDeviceCopyMetadata()
        wait(0)

        const title = findChild(editor, "deviceCopyMetadataTitle")
        const state = findChild(editor, "deviceCopyMetadataState")
        const dimensionsLabel =
            findChild(editor, "deviceCopyDimensionsLabel")
        const dimensions =
            findChild(editor, "deviceCopyDimensionsValue")
        const durationLabel =
            findChild(editor, "deviceCopyDurationLabel")
        const duration = findChild(editor, "deviceCopyDurationValue")
        const frameRateLabel =
            findChild(editor, "deviceCopyFrameRateLabel")
        const frameRate =
            findChild(editor, "deviceCopyFrameRateValue")
        for (const item of [title, state, dimensionsLabel, dimensions,
                            durationLabel, duration, frameRateLabel,
                            frameRate]) {
            verify(item !== null)
        }
        if (title.text === "Device copy metadata")
            skip("Russian translation catalog is not loaded")

        compare(title.text, "Метаданные копии с устройства")
        compare(state.text, "Готово")
        compare(dimensionsLabel.text, "Размеры")
        compare(dimensions.text, "2240 × 1080")
        compare(durationLabel.text, "Длительность")
        compare(duration.text, "01:00")
        compare(frameRateLabel.text, "Частота кадров")
        compare(frameRate.text, "30 FPS")

        controller.deviceCopyMetadataStatus = "Loading"
        wait(0)
        compare(state.text, "Загрузка")
        controller.deviceCopyMetadataStatus = "Partial"
        wait(0)
        compare(state.text, "Частично")
        controller.deviceCopyMetadataStatus = "Unavailable"
        wait(0)
        compare(state.text, "Недоступно")
        controller.deviceCopyMetadataStatus = "NotSupported"
        wait(0)
        compare(state.text, "Не поддерживается")
    }

    function test_preparationTargetRussianTranslation() {
        controller.preparationTarget = "SplitArea"
        controller.targetWidth = 1120
        controller.targetHeight = 1080
        wait(0)

        const group = findChild(editor, "mediaPreparationTargetGroup")
        const full = findChild(editor, "fullFramePreparationTarget")
        const split = findChild(editor, "splitAreaPreparationTarget")
        const description =
            findChild(editor, "mediaPreparationTargetDescription")
        const warning =
            findChild(editor, "mediaPreparationTargetResetWarning")
        const unavailable =
            findChild(editor, "splitAreaPreparationUnavailable")
        const resolution = findChild(editor, "mediaTargetResolution")
        for (const item of [group, full, split, description, warning,
                            unavailable, resolution]) {
            verify(item !== null)
        }
        if (group.title === "Prepare for")
            skip("Russian translation catalog is not loaded")

        compare(group.title, "Подготовить для")
        compare(group.Accessible.name, group.title)
        compare(full.text, "Весь экран")
        compare(split.text, "Область разделения")
        compare(
            description.text,
            "Подготавливает эту копию для левой или правой области " +
            "с разрешением 1120 × 1080. Сторона не выбирается, а " +
            "изменения не применяются к дисплею.")
        compare(
            warning.text,
            "Смена назначения сбрасывает все настройки редактирования.")
        compare(
            resolution.text,
            "Область разделения: 1120 × 1080")

        controller.splitTargetAvailable = false
        wait(0)
        verify(unavailable.visible)
        compare(
            unavailable.text,
            "Подготовка для области разделения недоступна для " +
            "подключённого устройства.")
    }
}
