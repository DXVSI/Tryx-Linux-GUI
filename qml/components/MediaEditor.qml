pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: root

    required property var controller

    parent: Overlay.overlay
    x: Math.max(16, (parent.width - width) / 2)
    y: Math.max(16, (parent.height - height) / 2)
    width: Math.min(1040, parent.width - 32)
    height: Math.min(800, parent.height - 32)
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    padding: 20
    readonly property var sizingModes: [
        {"value": "Fit", "label": qsTr("Fit")},
        {"value": "Fill", "label": qsTr("Fill")},
        {"value": "Crop", "label": qsTr("Crop")},
        {"value": "Stretch", "label": qsTr("Stretch")}
    ]
    readonly property bool recoveredTransformIsGeometryNeutral:
        controller.recoveredDeviceCopy &&
        controller.rotation === 0 &&
        (controller.mode !== "Crop" ||
         controller.zoomPercent === 100)
    readonly property real targetAspectRatio:
        controller.targetWidth > 0 && controller.targetHeight > 0
        ? controller.targetWidth / controller.targetHeight
        : 1
    readonly property string targetResolution:
        qsTr("%1 × %2").arg(controller.targetWidth)
                       .arg(controller.targetHeight)
    readonly property string preparationTargetLabel:
        controller.preparationTarget === "SplitArea"
        ? qsTr("Split area") : qsTr("Full screen")
    readonly property string preparationTargetDescription:
        controller.preparationTarget === "SplitArea"
        ? qsTr("Prepares this copy for either the left or right side at %1. It does not choose a side or apply changes to the display.")
          .arg(targetResolution)
        : qsTr("Prepares this copy for the full display at %1. It does not apply changes to the display.")
          .arg(targetResolution)
    readonly property string splitTargetUnavailableText:
        qsTr("Split area preparation is not available for the connected device.")
    readonly property string preparationTargetResetWarning:
        qsTr("Changing the target resets all editing settings.")
    readonly property string recoveredSourceResolution:
        controller.deviceCopyDimensionsAvailable &&
        controller.deviceCopyWidth > 0 &&
        controller.deviceCopyHeight > 0
        ? qsTr("%1 × %2").arg(controller.deviceCopyWidth)
                           .arg(controller.deviceCopyHeight)
        : ""
    readonly property string deviceCopyDimensionsText:
        recoveredSourceResolution.length > 0
        ? recoveredSourceResolution : qsTr("Unavailable")
    readonly property string deviceCopyDurationText:
        controller.deviceCopyDurationAvailable
        ? formatDuration(controller.deviceCopyDurationMilliseconds)
        : qsTr("Unavailable")
    readonly property string deviceCopyFrameRateText:
        controller.deviceCopyFrameRateAvailable
        ? formatFrameRate(controller.deviceCopyFrameRateNumerator,
                          controller.deviceCopyFrameRateDenominator)
        : qsTr("Unavailable")

    function synchronizeVisibility() {
        if (controller.open && !root.opened)
            root.open()
        else if (!controller.open && root.opened)
            root.close()
    }

    function modeDescription(mode) {
        switch (mode) {
        case "Fit":
            return qsTr("Show the whole image and fill any free space with the selected background color.")
        case "Fill":
            return qsTr("Fill the screen while preserving proportions; edges are cropped from the center.")
        case "Crop":
            return qsTr("Fill the screen and adjust zoom and position manually.")
        case "Stretch":
            return qsTr("Fill the screen exactly without preserving proportions. The image may be distorted.")
        default:
            return ""
        }
    }

    function metadataStateText(status) {
        switch (status) {
        case "Loading":
            return qsTr("Loading")
        case "Ready":
            return qsTr("Ready")
        case "Partial":
            return qsTr("Partial")
        case "NotSupported":
            return qsTr("Not supported")
        case "Unavailable":
        case "ProbeFailed":
        default:
            return qsTr("Unavailable")
        }
    }

    function padTwo(value) {
        return value < 10 ? "0" + value : value.toString()
    }

    function formatDuration(milliseconds) {
        if (!Number.isFinite(milliseconds) || milliseconds < 0)
            return qsTr("Unavailable")
        const totalSeconds = Math.floor(milliseconds / 1000)
        const seconds = totalSeconds % 60
        const totalMinutes = Math.floor(totalSeconds / 60)
        const minutes = totalMinutes % 60
        const hours = Math.floor(totalMinutes / 60)
        if (hours > 0) {
            return padTwo(hours) + ":" + padTwo(minutes) +
                   ":" + padTwo(seconds)
        }
        return padTwo(totalMinutes) + ":" + padTwo(seconds)
    }

    function formatFrameRate(numerator, denominator) {
        if (numerator <= 0 || denominator <= 0)
            return qsTr("Unavailable")
        const value = numerator / denominator
        const rounded = Math.round(value * 100) / 100
        const text = Math.abs(rounded - Math.round(rounded)) < 0.0001
                   ? Math.round(rounded).toString()
                   : rounded.toFixed(2).replace(/0+$/, "")
                                      .replace(/\.$/, "")
        return qsTr("%1 FPS").arg(text)
    }

    Component.onCompleted: synchronizeVisibility()

    Connections {
        target: root.controller
        function onOpenChanged() {
            root.synchronizeVisibility()
        }
    }

    background: Rectangle {
        color: "#1d2125"
        radius: 12
        border.color: "#7f8d36"
    }

    contentItem: ColumnLayout {
        id: editorContent

        objectName: "mediaEditorContent"
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Media Editor")
                font.pixelSize: 22
                font.bold: true
            }
            Label {
                text: root.controller.sourceName
                color: "#9da1b3"
                elide: Text.ElideMiddle
                Layout.maximumWidth: 420
            }
        }

        ScrollView {
            id: editorScroll

            objectName: "mediaEditorScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 160
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: editorScrollableContent

                width: editorScroll.availableWidth
                spacing: 14

                Rectangle {
                    objectName: "recoveredDeviceCopyNotice"
                    Layout.fillWidth: true
                    Layout.preferredHeight:
                        recoveredNotice.implicitHeight + 20
                    visible: root.controller.recoveredDeviceCopy
                    radius: 7
                    color: root.recoveredTransformIsGeometryNeutral
                           ? "#352d20" : "#2b3024"
                    border.width: 1
                    border.color:
                        root.recoveredTransformIsGeometryNeutral
                        ? "#b9833f" : "#7f8d36"

                    Label {
                        id: recoveredNotice

                        objectName:
                            "recoveredDeviceCopyNoticeText"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter:
                            parent.verticalCenter
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        text:
                            root.recoveredTransformIsGeometryNeutral
                            ? (root.recoveredSourceResolution.length > 0
                               ? qsTr("This device copy is already encoded at %1, so the current settings will not visibly change it. Existing padding is baked into the video. Choose Crop and raise Zoom above 100%, or rotate the video. Save as new does not change the active display; select the new copy in the library and apply it. Previously lost areas cannot be restored.").arg(root.recoveredSourceResolution)
                               : qsTr("This device copy is already encoded, so the current settings will not visibly change it. Existing padding is baked into the video. Choose Crop and raise Zoom above 100%, or rotate the video. Save as new does not change the active display; select the new copy in the library and apply it. Previously lost areas cannot be restored."))
                            : qsTr("This is a private working copy recovered from the device. Save as new stores another media item but does not change the active display; select the new copy in the library and apply it. Replace original updates the original item. Saving re-encodes the video; areas lost before the original upload cannot be restored.")
                        color:
                            root.recoveredTransformIsGeometryNeutral
                            ? "#f0c27b" : "#d8ddb9"
                        wrapMode: Text.WordWrap
                    }
                }

                Rectangle {
                    id: deviceCopyMetadata

                    objectName: "deviceCopyMetadata"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth:
                        Math.min(editorScrollableContent.width,
                                 editorContent.width)
                    Layout.maximumWidth:
                        Math.min(editorScrollableContent.width,
                                 editorContent.width)
                    Layout.preferredHeight:
                        deviceCopyMetadataContent.implicitHeight + 20
                    visible: root.controller.recoveredDeviceCopy
                    radius: 7
                    color: "#242a2f"
                    border.width: 1
                    border.color: "#59636b"
                    Accessible.name: deviceCopyMetadataTitle.text
                    Accessible.description:
                        deviceCopyMetadataTitle.text + ". " +
                        deviceCopyMetadataState.text + ". " +
                        deviceCopyDimensionsLabel.text + ": " +
                        deviceCopyDimensionsValue.text + ". " +
                        deviceCopyDurationLabel.text + ": " +
                        deviceCopyDurationValue.text + ". " +
                        deviceCopyFrameRateLabel.text + ": " +
                        deviceCopyFrameRateValue.text + "."
                    Accessible.role: Accessible.StaticText

                    ColumnLayout {
                        id: deviceCopyMetadataContent

                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 6

                        Label {
                            id: deviceCopyMetadataTitle

                            objectName: "deviceCopyMetadataTitle"
                            Layout.fillWidth: true
                            text: qsTr("Device copy metadata")
                            textFormat: Text.PlainText
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            id: deviceCopyMetadataState

                            objectName: "deviceCopyMetadataState"
                            Layout.fillWidth: true
                            text: root.metadataStateText(
                                      root.controller
                                          .deviceCopyMetadataStatus)
                            textFormat: Text.PlainText
                            color: "#b7bac7"
                            wrapMode: Text.WordWrap
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 12
                            rowSpacing: 4

                            Label {
                                id: deviceCopyDimensionsLabel

                                objectName:
                                    "deviceCopyDimensionsLabel"
                                text: qsTr("Dimensions")
                                textFormat: Text.PlainText
                                color: "#aeb5bb"
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                id: deviceCopyDimensionsValue

                                objectName:
                                    "deviceCopyDimensionsValue"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: root.deviceCopyDimensionsText
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignRight
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                id: deviceCopyDurationLabel

                                objectName:
                                    "deviceCopyDurationLabel"
                                text: qsTr("Duration")
                                textFormat: Text.PlainText
                                color: "#aeb5bb"
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                id: deviceCopyDurationValue

                                objectName:
                                    "deviceCopyDurationValue"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: root.deviceCopyDurationText
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignRight
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                id: deviceCopyFrameRateLabel

                                objectName:
                                    "deviceCopyFrameRateLabel"
                                text: qsTr("Frame rate")
                                textFormat: Text.PlainText
                                color: "#aeb5bb"
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                id: deviceCopyFrameRateValue

                                objectName:
                                    "deviceCopyFrameRateValue"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: root.deviceCopyFrameRateText
                                textFormat: Text.PlainText
                                horizontalAlignment: Text.AlignRight
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                GroupBox {
                    id: preparationTargetGroup

                    objectName: "mediaPreparationTargetGroup"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth:
                        Math.min(editorScrollableContent.width,
                                 editorContent.width)
                    Layout.maximumWidth:
                        Math.min(editorScrollableContent.width,
                                 editorContent.width)
                    title: qsTr("Prepare for")
                    Accessible.role: Accessible.Grouping
                    Accessible.name: title
                    Accessible.description:
                        preparationTargetDescription.text + " " +
                        preparationTargetResetWarning.text +
                        (splitTargetUnavailable.visible
                         ? " " + splitTargetUnavailable.text : "")

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        ButtonGroup {
                            id: preparationTargetButtons
                        }

                        GridLayout {
                            id: preparationTargetChoices

                            objectName:
                                "mediaPreparationTargetChoices"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 0
                            columns:
                                preparationTargetGroup.width >= 560
                                ? 2 : 1
                            columnSpacing: 10
                            rowSpacing: 4

                            RadioButton {
                                id: fullFramePreparationTarget

                                objectName:
                                    "fullFramePreparationTarget"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: qsTr("Full screen")
                                enabled:
                                    !root.controller
                                        .submissionPending
                                checked:
                                    root.controller
                                        .preparationTarget ===
                                    "FullFrame"
                                ButtonGroup.group:
                                    preparationTargetButtons
                                Accessible.name: text
                                Accessible.description:
                                    qsTr("Prepare media for the full display.")
                                onClicked:
                                    root.controller
                                        .preparationTarget =
                                        "FullFrame"
                            }

                            RadioButton {
                                id: splitAreaPreparationTarget

                                objectName:
                                    "splitAreaPreparationTarget"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: qsTr("Split area")
                                enabled:
                                    root.controller
                                        .splitTargetAvailable &&
                                    !root.controller
                                        .submissionPending
                                checked:
                                    root.controller
                                        .preparationTarget ===
                                    "SplitArea"
                                ButtonGroup.group:
                                    preparationTargetButtons
                                Accessible.name: text
                                Accessible.description:
                                    root.controller
                                        .splitTargetAvailable
                                    ? qsTr("Prepare media for either the left or right side of Split screen.")
                                    : root.splitTargetUnavailableText
                                onClicked:
                                    root.controller
                                        .preparationTarget =
                                        "SplitArea"
                            }
                        }

                        Label {
                            id: preparationTargetDescription

                            objectName:
                                "mediaPreparationTargetDescription"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: root.preparationTargetDescription
                            textFormat: Text.PlainText
                            color: "#b7bac7"
                            wrapMode: Text.WordWrap
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }

                        Label {
                            id: preparationTargetResetWarning

                            objectName:
                                "mediaPreparationTargetResetWarning"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: root.preparationTargetResetWarning
                            textFormat: Text.PlainText
                            color: "#f0c27b"
                            wrapMode: Text.WordWrap
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }

                        Label {
                            id: splitTargetUnavailable

                            objectName:
                                "splitAreaPreparationUnavailable"
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            visible:
                                !root.controller
                                    .splitTargetAvailable
                            text: root.splitTargetUnavailableText
                            textFormat: Text.PlainText
                            color: "#f0c27b"
                            wrapMode: Text.WordWrap
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }
                }

                Item {
                    id: previewViewport
                    Layout.fillWidth: true
                    Layout.preferredHeight:
                        Math.min(
                            420,
                            width / root.targetAspectRatio)
                    Layout.minimumHeight: 220

                    Rectangle {
                        id: canvas
                        objectName: "mediaPreviewCanvas"

                        anchors.centerIn: parent
                        width: Math.min(
                            parent.width,
                            parent.height *
                            root.targetAspectRatio)
                        height: width / root.targetAspectRatio
                        color: "#000000"
                        border.color: "#6b6f80"
                        radius: 6
                        clip: true

                        Image {
                            id: previewImage

                            anchors.fill: parent
                            source:
                                root.controller.previewUrl
                            asynchronous: false
                            cache: false
                            smooth: true
                            fillMode: Image.Stretch
                        }

                        MouseArea {
                            id: panArea

                            anchors.fill: parent
                            enabled:
                                root.controller.mode === "Crop" &&
                                root.controller.zoomPercent > 100 &&
                                root.controller.ready &&
                                !root.controller.submissionPending
                            cursorShape: enabled
                                         ? Qt.OpenHandCursor
                                         : Qt.ArrowCursor
                            property real previousX: 0
                            property real previousY: 0

                            onPressed: mouse => {
                                previousX = mouse.x
                                previousY = mouse.y
                                cursorShape =
                                    Qt.ClosedHandCursor
                            }
                            onReleased:
                                cursorShape =
                                    Qt.OpenHandCursor
                            onCanceled:
                                cursorShape =
                                    Qt.OpenHandCursor
                            onPositionChanged: mouse => {
                                if (!pressed)
                                    return
                                const dx =
                                    mouse.x - previousX
                                const dy =
                                    mouse.y - previousY
                                previousX = mouse.x
                                previousY = mouse.y
                                root.controller.focusX =
                                    Math.round(
                                        root.controller.focusX -
                                        dx / Math.max(
                                            1, canvas.width) *
                                        10000)
                                root.controller.focusY =
                                    Math.round(
                                        root.controller.focusY -
                                        dy / Math.max(
                                            1, canvas.height) *
                                        10000)
                            }
                        }

                        BusyIndicator {
                            anchors.centerIn: parent
                            running: root.controller.busy
                            visible: running
                        }

                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 40
                            visible:
                                !root.controller.busy &&
                                !root.controller.ready
                            text:
                                root.controller.error.length > 0
                                ? root.controller.error
                                : qsTr(
                                      "Select a supported media file")
                            color:
                                root.controller.error.length > 0
                                ? "#ef7784" : "#a7aabb"
                            wrapMode: Text.WordWrap
                            horizontalAlignment:
                                Text.AlignHCenter
                        }

                        Label {
                            objectName: "mediaTargetResolution"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 8
                            text: qsTr("%1: %2")
                                  .arg(root.preparationTargetLabel)
                                  .arg(root.targetResolution)
                            textFormat: Text.PlainText
                            color: "#b7bac7"
                            font.pixelSize: 11
                            elide: Text.ElideLeft
                            horizontalAlignment: Text.AlignRight
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label { text: qsTr("Sizing") }
                    ButtonGroup { id: modeGroup }
                    Repeater {
                        model: root.sizingModes
                        Button {
                            required property var modelData

                            text: modelData.label
                            checkable: true
                            enabled:
                                !root.controller
                                    .submissionPending
                            checked:
                                root.controller.mode ===
                                modelData.value
                            ButtonGroup.group: modeGroup
                            onClicked:
                                root.controller.mode =
                                    modelData.value
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Label { text: qsTr("Rotation") }
                    ComboBox {
                        model: ["0°", "90°", "180°", "270°"]
                        enabled:
                            !root.controller.submissionPending
                        currentIndex:
                            root.controller.rotation / 90
                        onActivated: index => {
                            root.controller.rotation =
                                index * 90
                        }
                    }
                }

                Label {
                    objectName: "mediaSizingDescription"
                    Layout.fillWidth: true
                    text:
                        root.modeDescription(
                            root.controller.mode)
                    color:
                        root.controller.mode === "Stretch"
                        ? "#efb85f" : "#aeb5bb"
                    wrapMode: Text.WordWrap
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 4
                    columnSpacing: 12

                    Label {
                        text: qsTr("Zoom")
                        enabled:
                            root.controller.mode === "Crop" &&
                            !root.controller.submissionPending
                    }
                    Slider {
                        Layout.fillWidth: true
                        from: 100
                        to: 400
                        stepSize: 1
                        value: root.controller.zoomPercent
                        enabled:
                            root.controller.mode === "Crop" &&
                            !root.controller.submissionPending
                        onMoved:
                            root.controller.zoomPercent =
                                Math.round(value)
                    }
                    Label {
                        text:
                            qsTr("%1%").arg(
                                root.controller.zoomPercent)
                        enabled:
                            root.controller.mode === "Crop" &&
                            !root.controller.submissionPending
                    }
                    Item { Layout.fillWidth: true }

                    Label {
                        text: qsTr("Horizontal position")
                        enabled:
                            root.controller.mode === "Crop" &&
                            root.controller.zoomPercent > 100 &&
                            !root.controller.submissionPending
                    }
                    SpinBox {
                        objectName:
                            "cropHorizontalPosition"
                        from: 0
                        to: 100
                        stepSize: 1
                        value: Math.round(
                            root.controller.focusX / 100)
                        enabled:
                            root.controller.mode === "Crop" &&
                            root.controller.zoomPercent > 100 &&
                            !root.controller.submissionPending
                        editable: true
                        textFromValue:
                            function(value, locale) {
                                return value + "%"
                            }
                        valueFromText:
                            function(text, locale) {
                                const parsed = parseInt(text)
                                return isNaN(parsed)
                                    ? 50 : parsed
                            }
                        onValueModified:
                            root.controller.focusX =
                                value * 100
                    }
                    Label {
                        text: qsTr("Vertical position")
                        enabled:
                            root.controller.mode === "Crop" &&
                            root.controller.zoomPercent > 100 &&
                            !root.controller.submissionPending
                    }
                    SpinBox {
                        objectName:
                            "cropVerticalPosition"
                        from: 0
                        to: 100
                        stepSize: 1
                        value: Math.round(
                            root.controller.focusY / 100)
                        enabled:
                            root.controller.mode === "Crop" &&
                            root.controller.zoomPercent > 100 &&
                            !root.controller.submissionPending
                        editable: true
                        textFromValue:
                            function(value, locale) {
                                return value + "%"
                            }
                        valueFromText:
                            function(text, locale) {
                                const parsed = parseInt(text)
                                return isNaN(parsed)
                                    ? 50 : parsed
                            }
                        onValueModified:
                            root.controller.focusY =
                                value * 100
                    }

                    Label {
                        text: qsTr("Fit background")
                        enabled:
                            root.controller.mode === "Fit" &&
                            !root.controller.submissionPending
                    }
                    Button {
                        id: backgroundButton

                        text:
                            root.controller.backgroundColor
                        enabled:
                            root.controller.mode === "Fit" &&
                            !root.controller.submissionPending
                        onClicked: colorDialog.open()
                        background: Rectangle {
                            radius: 5
                            color:
                                root.controller
                                    .backgroundColor
                            border.color: "#989baa"
                        }
                        contentItem: Label {
                            text: backgroundButton.text
                            color:
                                root.controller
                                    .backgroundColor ===
                                "#000000"
                                ? "#ffffff" : "#000000"
                            horizontalAlignment:
                                Text.AlignHCenter
                            verticalAlignment:
                                Text.AlignVCenter
                        }
                    }
                    Item {
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible:
                        root.controller.error.length > 0
                    text: root.controller.error
                    color: "#ef7784"
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            objectName: "mediaEditorActionRow"
            Layout.fillWidth: true
            Button {
                text: qsTr("Reset")
                enabled: !root.controller.submissionPending
                onClicked: root.controller.reset()
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "mediaEditorCancelButton"
                text: qsTr("Cancel")
                enabled: !root.controller.submissionPending
                onClicked: root.controller.cancel()
            }
            PrimaryButton {
                objectName: "mediaEditorUploadButton"
                visible: !root.controller.recoveredDeviceCopy
                text: root.controller.busy
                      ? qsTr("Please wait…")
                      : qsTr("Upload")
                enabled: root.controller.ready &&
                         !root.controller.busy
                onClicked: root.controller.submit()
            }
            Button {
                id: replaceButton

                objectName: "mediaEditorReplaceButton"
                visible: root.controller.recoveredDeviceCopy
                text: root.controller.submissionPending &&
                      root.controller.submissionAction === "Replace"
                      ? qsTr("Replacing…")
                      : qsTr("Replace original")
                enabled: root.controller.ready &&
                         root.controller.replaceAllowed &&
                         !root.controller.busy &&
                         !root.controller.submissionPending
                ToolTip.visible: hovered &&
                                 !root.controller.replaceAllowed &&
                                 root.controller
                                     .replaceBlockReason.length > 0
                ToolTip.text: root.controller.replaceBlockReason
                onClicked: root.controller.submitReplace()
            }
            PrimaryButton {
                objectName: "mediaEditorSaveAsNewButton"
                visible: root.controller.recoveredDeviceCopy
                text: root.controller.submissionPending &&
                      root.controller.submissionAction === "SaveAsNew"
                      ? qsTr("Saving…")
                      : qsTr("Save as new")
                enabled: root.controller.ready &&
                         !root.controller.busy &&
                         !root.controller.submissionPending
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Creates a new media item without changing the active display")
                onClicked: root.controller.submitSaveAsNew()
            }
        }
    }

    ColorDialog {
        id: colorDialog
        title: qsTr("Fit background color")
        selectedColor: root.controller.backgroundColor
        onAccepted: root.controller.backgroundColor =
                    selectedColor.toString()
    }
}
