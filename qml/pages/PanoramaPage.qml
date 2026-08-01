pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../components"

ScrollView {
    id: root

    required property var runtime
    required property var editor
    required property var deviceMedia

    clip: true

    property bool splitMode: false
    property var selectedMedia: []
    property var fullMetrics: []
    property var leftMetrics: []
    property var rightMetrics: []
    property var fullBadges: []
    property var leftBadges: []
    property var rightBadges: []
    property int brightnessDraft: runtime.brightness
    property bool mirrorDraft: runtime.mirrorMode
    property bool waterfallDraft: runtime.waterfallMode
    property string playMode: "Single"
    property string metricsAlignment: runtime.metricsAlignment || "Left"
    property string metricsColor: runtime.metricsColor || "#dcdcdc"
    property string pendingDeleteName: ""
    readonly property var alignmentOptions: [
        {"value": "Left", "label": qsTr("Left")},
        {"value": "Center", "label": qsTr("Center")},
        {"value": "Right", "label": qsTr("Right")}
    ]

    function toggled(list, value, limit) {
        const next = list.slice(0)
        const existing = next.indexOf(value)
        if (existing >= 0) {
            next.splice(existing, 1)
            return next
        }
        if (next.length >= limit)
            next.shift()
        next.push(value)
        return next
    }

    function toggleMedia(value) {
        selectedMedia = toggled(selectedMedia, value,
                                splitMode ? 2 : 1)
    }

    function toggleMetric(group, value) {
        if (group === "full")
            fullMetrics = toggled(fullMetrics, value, 3)
        else if (group === "left")
            leftMetrics = toggled(leftMetrics, value, 3)
        else
            rightMetrics = toggled(rightMetrics, value, 3)
    }

    function toggleBadge(group, value) {
        if (group === "full")
            fullBadges = toggled(fullBadges, value, 2)
        else if (group === "left")
            leftBadges = toggled(leftBadges, value, 2)
        else
            rightBadges = toggled(rightBadges, value, 2)
    }

    function requestDelete(mediaName) {
        if (!runtime.mediaModel.canDelete(mediaName))
            return
        pendingDeleteName = mediaName
        deleteConfirmation.open()
    }

    function metricLabel(value) {
        switch (value) {
        case "CPU Temperature":
            return qsTr("CPU temperature")
        case "CPU Frequency":
            return qsTr("CPU frequency")
        case "CPU Usage":
            return qsTr("CPU usage")
        case "CPU Power":
            return qsTr("CPU power")
        case "GPU Temperature":
            return qsTr("GPU temperature")
        case "GPU Frequency":
            return qsTr("GPU frequency")
        case "GPU Usage":
            return qsTr("GPU usage")
        case "GPU Power":
            return qsTr("GPU power")
        case "Memory Usage":
            return qsTr("Memory usage")
        case "DateTime":
            return qsTr("Date and time")
        default:
            return value
        }
    }

    function selected(group, value) {
        if (group === "full")
            return fullMetrics.indexOf(value) >= 0
        if (group === "left")
            return leftMetrics.indexOf(value) >= 0
        return rightMetrics.indexOf(value) >= 0
    }

    function badgeSelected(group, value) {
        if (group === "full")
            return fullBadges.indexOf(value) >= 0
        if (group === "left")
            return leftBadges.indexOf(value) >= 0
        return rightBadges.indexOf(value) >= 0
    }

    function synchronizeDisplayDraft() {
        brightnessDraft = runtime.brightness
        mirrorDraft = runtime.mirrorMode
        waterfallDraft = runtime.waterfallMode
        splitMode = runtime.currentScreenMode ===
                    "Screen Splitting"
        const media = runtime.displayedMedia || []
        selectedMedia = media.slice(
            0, splitMode ? 2 : 1)
        if (runtime.currentPlayMode.length > 0)
            playMode = runtime.currentPlayMode
        fullMetrics =
            (runtime.displayLeftMetrics || []).slice(0)
        leftMetrics =
            (runtime.displayLeftMetrics || []).slice(0)
        rightMetrics =
            (runtime.displayRightMetrics || []).slice(0)
        fullBadges =
            (runtime.displayLeftBadges || []).slice(0)
        leftBadges =
            (runtime.displayLeftBadges || []).slice(0)
        rightBadges =
            (runtime.displayRightBadges || []).slice(0)
    }

    Component.onCompleted: {
        synchronizeDisplayDraft()
        fullMetrics =
            (runtime.activeMetrics || fullMetrics).slice(0)
    }

    Connections {
        target: root.runtime
        function onDisplayChanged() {
            root.synchronizeDisplayDraft()
        }
        function onMetricsChanged() {
            root.metricsAlignment =
                root.runtime.metricsAlignment || "Left"
            root.metricsColor =
                root.runtime.metricsColor || "#dcdcdc"
            if (!root.splitMode) {
                root.fullMetrics =
                    (root.runtime.activeMetrics || []).slice(0)
            }
        }
    }

    ColumnLayout {
        objectName: "panoramaContent"
        x: 24
        y: 24
        width: Math.max(0, root.availableWidth - 48)
        spacing: 16

        OperationBanner {
            Layout.fillWidth: true
            runtime: root.runtime
        }

        GridLayout {
            id: displayWorkspace

            objectName: "displayWorkspace"
            Layout.fillWidth: true
            columns: root.availableWidth >= 1120 ? 2 : 1
            columnSpacing: 16
            rowSpacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.minimumWidth:
                    displayWorkspace.columns === 2 ? 600 : 0
                Layout.preferredWidth:
                    displayWorkspace.columns === 2 ? 700 : 0
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    PrimaryButton {
                        objectName: "uploadMediaButton"
                        text: qsTr("Upload media…")
                        enabled: root.runtime.displaySessionActive &&
                                 !root.runtime.operationBusy
                        onClicked: mediaPicker.openPicker()
                    }
                    Button {
                        text: qsTr("Reload media library")
                        enabled: root.runtime.compatible &&
                                 !root.runtime.operationBusy
                        onClicked: root.runtime.refreshMedia()
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: qsTr("%1 selected")
                              .arg(root.selectedMedia.length)
                        color: "#9da1b3"
                    }
                }

                Frame {
                    id: dropFrame

                    Layout.fillWidth: true
                    Layout.preferredHeight: 72
                    background: Rectangle {
                        radius: 8
                        color: dropArea.containsDrag
                               ? "#30372d" : "#23282d"
                        border.width: 2
                        border.color: dropArea.containsDrag
                                      ? "#def750" : "#4a535a"
                    }
                    Label {
                        anchors.centerIn: parent
                        width: parent.width - 24
                        text: qsTr("Drop one MP4, WebM, MKV, AVI, MOV, GIF, JPG, PNG, BMP or WebP file here")
                        color: "#b8bbca"
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                    }
                    DropArea {
                        id: dropArea
                        anchors.fill: parent
                        enabled: !root.runtime.operationBusy
                        onDropped: drop => {
                            root.editor.beginDropped(drop.urls)
                            drop.acceptProposedAction()
                        }
                    }
                }

                GroupBox {
                    objectName: "mediaLibraryGroup"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(
                        470,
                        Math.max(
                            235,
                            Math.ceil(
                                mediaGrid.count /
                                Math.max(
                                    1,
                                    Math.floor(
                                        mediaGrid.width /
                                        mediaGrid.cellWidth))) *
                            mediaGrid.cellHeight + 45))
                    title: qsTr("Media Library")

                    GridView {
                        id: mediaGrid

                        anchors.fill: parent
                        cellWidth: 220
                        cellHeight: 158
                        clip: true
                        model: root.runtime.mediaModel

                        delegate: ItemDelegate {
                            id: mediaDelegate

                            required property string mediaName
                            required property string mediaId
                            required property var thumbnailUrl
                            required property bool deleteAllowed
                            required property string deleteBlockReason
                            required property bool deviceCopyAllowed
                            required property string deviceCopyBlockReason
                            required property double mediaSize

                            width: mediaGrid.cellWidth - 10
                            height: mediaGrid.cellHeight - 10
                            highlighted:
                                root.selectedMedia.indexOf(mediaName) >= 0
                            onClicked: root.toggleMedia(mediaName)
                            ToolTip.visible: hovered &&
                                             !deleteAllowed &&
                                             deleteBlockReason.length > 0
                            ToolTip.text: deleteBlockReason

                            contentItem: ColumnLayout {
                                spacing: 6
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 92
                                    color: "#15181b"
                                    radius: 5
                                    Image {
                                        anchors.fill: parent
                                        anchors.margins: 3
                                        source: mediaDelegate.thumbnailUrl
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                    }
                                    Label {
                                        anchors.centerIn: parent
                                        visible:
                                            String(mediaDelegate.thumbnailUrl)
                                            .length === 0
                                        text: qsTr("No preview")
                                        color: "#6f7383"
                                    }
                                    ToolButton {
                                        id: mediaActionButton

                                        objectName: "mediaActionButton"
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        width: 30
                                        height: 30
                                        padding: 0
                                        leftPadding: 0
                                        rightPadding: 0
                                        topPadding: 0
                                        bottomPadding: 0
                                        leftInset: 0
                                        rightInset: 0
                                        topInset: 0
                                        bottomInset: 0
                                        text: "⋯"
                                        Accessible.name:
                                            qsTr("Media actions")
                                        enabled:
                                            !root.runtime.operationBusy &&
                                            !root.deviceMedia.busy
                                        ToolTip.visible: hovered
                                        ToolTip.text:
                                            !mediaDelegate.deviceCopyAllowed &&
                                            mediaDelegate
                                                .deviceCopyBlockReason
                                                .length > 0
                                            ? mediaDelegate
                                                .deviceCopyBlockReason
                                            : qsTr("Media actions")
                                        background: Rectangle {
                                            anchors.fill: parent
                                            radius: width / 2
                                            color:
                                                mediaActionButton.hovered
                                                ? "#485057"
                                                : "#343a3f"
                                            border.width: 1
                                            border.color:
                                                mediaActionButton.hovered
                                                ? "#8d979f"
                                                : "#515960"
                                        }
                                        contentItem: Label {
                                            text: mediaActionButton.text
                                            color: "#f4f6f7"
                                            font.pixelSize: 20
                                            horizontalAlignment:
                                                Text.AlignHCenter
                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }
                                        onClicked: mediaActions.open()

                                        Menu {
                                            id: mediaActions

                                            y: mediaActionButton.height + 4

                                            MenuItem {
                                                objectName:
                                                    "editDeviceMediaAction"
                                                text: qsTr("Edit")
                                                enabled:
                                                    mediaDelegate
                                                        .deviceCopyAllowed
                                                onTriggered:
                                                    root.deviceMedia
                                                        .beginEdit(
                                                            mediaDelegate
                                                                .mediaId,
                                                            mediaDelegate
                                                                .mediaName)
                                            }

                                            MenuItem {
                                                objectName:
                                                    "exportDeviceMediaAction"
                                                text: qsTr("Export copy…")
                                                enabled:
                                                    mediaDelegate
                                                        .deviceCopyAllowed
                                                onTriggered:
                                                    exportPicker.openFor(
                                                        mediaDelegate.mediaId,
                                                        mediaDelegate
                                                            .mediaName)
                                            }

                                            MenuSeparator {}

                                            MenuItem {
                                                objectName:
                                                    "deleteDeviceMediaAction"
                                                text: qsTr("Delete")
                                                enabled:
                                                    mediaDelegate
                                                        .deleteAllowed
                                                onTriggered:
                                                    root.requestDelete(
                                                        mediaDelegate
                                                            .mediaName)
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: mediaDelegate.mediaName
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 12
                                }
                                Label {
                                    text: qsTr("%1 MiB").arg(
                                        (mediaDelegate.mediaSize /
                                         1024 / 1024).toFixed(1))
                                    color: "#85899a"
                                    font.pixelSize: 10
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: mediaGrid.count === 0
                            text: qsTr("No uploaded media")
                            color: "#8d91a1"
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.minimumWidth:
                    displayWorkspace.columns === 2 ? 400 : 0
                Layout.preferredWidth:
                    displayWorkspace.columns === 2 ? 430 : 0
                spacing: 16

            GroupBox {
            objectName: "displayLayoutGroup"
            Layout.fillWidth: true
            title: qsTr("Display layout")

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 8

                    RadioButton {
                        Layout.fillWidth: true
                        text: qsTr("Full screen")
                        checked: !root.splitMode
                        onClicked: {
                            root.splitMode = false
                            if (root.selectedMedia.length > 1)
                                root.selectedMedia =
                                    [root.selectedMedia[0]]
                        }
                    }
                    RadioButton {
                        Layout.fillWidth: true
                        text: qsTr("Split screen")
                        checked: root.splitMode
                        onClicked: root.splitMode = true
                    }
                    Label { text: qsTr("Play mode") }
                    ComboBox {
                        id: playModeCombo
                        objectName: "playModeCombo"
                        Layout.fillWidth: true

                        readonly property var playModes:
                            root.splitMode
                            ? [
                                {"value": "Single",
                                 "label": qsTr("Single")}
                              ]
                            : [
                                {"value": "Single",
                                 "label": qsTr("Single")},
                                {"value": "Loop",
                                 "label": qsTr("Loop")},
                                {"value": "Shuffle",
                                 "label": qsTr("Shuffle")}
                              ]
                        model: playModes
                        textRole: "label"
                        currentIndex: Math.max(
                            0,
                            playModes.findIndex(
                                item => item.value ===
                                (root.splitMode
                                 ? "Single"
                                 : root.playMode)))
                        onActivated: index => {
                            root.playMode =
                                playModes[index].value
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: root.splitMode
                          ? qsTr("Left: %1    Right: %2")
                            .arg(root.selectedMedia[0] || qsTr("not selected"))
                            .arg(root.selectedMedia[1] || qsTr("not selected"))
                          : qsTr("Media: %1")
                            .arg(root.selectedMedia[0] ||
                                 qsTr("not selected"))
                    color: "#b8bbca"
                    elide: Text.ElideMiddle
                }

                Label {
                    text: root.splitMode
                          ? qsTr("Select up to three metrics per side")
                          : qsTr("Select up to three overlay metrics")
                    font.bold: true
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: !root.splitMode
                    Repeater {
                        model: root.runtime.availableMetrics
                        CheckBox {
                            required property string modelData
                            text: root.metricLabel(modelData)
                            checked: root.selected("full", modelData)
                            onClicked:
                                root.toggleMetric("full", modelData)
                        }
                    }
                }

                Label {
                    visible: !root.splitMode
                    text: qsTr("Hardware badges")
                    font.bold: true
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    visible: !root.splitMode

                    CheckBox {
                        objectName: "fullCpuBadge"
                        text: qsTr("CPU Badge")
                        checked:
                            root.badgeSelected(
                                "full", "CPU Badge")
                        onClicked:
                            root.toggleBadge(
                                "full", "CPU Badge")
                    }
                    CheckBox {
                        objectName: "fullGpuBadge"
                        text: qsTr("GPU Badge")
                        checked:
                            root.badgeSelected(
                                "full", "GPU Badge")
                        onClicked:
                            root.toggleBadge(
                                "full", "GPU Badge")
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.splitMode
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Left metrics") }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: root.runtime.availableMetrics
                                CheckBox {
                                    required property string modelData
                                    text: root.metricLabel(modelData)
                                    checked:
                                        root.selected("left", modelData)
                                    onClicked:
                                        root.toggleMetric("left",
                                                          modelData)
                                }
                            }
                        }
                        Label {
                            text: qsTr("Left badges")
                            font.bold: true
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6

                            CheckBox {
                                objectName: "leftCpuBadge"
                                text: qsTr("CPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "left", "CPU Badge")
                                onClicked:
                                    root.toggleBadge(
                                        "left", "CPU Badge")
                            }
                            CheckBox {
                                objectName: "leftGpuBadge"
                                text: qsTr("GPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "left", "GPU Badge")
                                onClicked:
                                    root.toggleBadge(
                                        "left", "GPU Badge")
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Right metrics") }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: root.runtime.availableMetrics
                                CheckBox {
                                    required property string modelData
                                    text: root.metricLabel(modelData)
                                    checked:
                                        root.selected("right", modelData)
                                    onClicked:
                                        root.toggleMetric("right",
                                                          modelData)
                                }
                            }
                        }
                        Label {
                            text: qsTr("Right badges")
                            font.bold: true
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: 6

                            CheckBox {
                                objectName: "rightCpuBadge"
                                text: qsTr("CPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "right", "CPU Badge")
                                onClicked:
                                    root.toggleBadge(
                                        "right", "CPU Badge")
                            }
                            CheckBox {
                                objectName: "rightGpuBadge"
                                text: qsTr("GPU Badge")
                                checked:
                                    root.badgeSelected(
                                        "right", "GPU Badge")
                                onClicked:
                                    root.toggleBadge(
                                        "right", "GPU Badge")
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343a40"
                }

                ColumnLayout {
                    objectName: "metricsOverlayControls"
                    Layout.fillWidth: true
                    spacing: 10

                    Label {
                        text: qsTr("Live metrics")
                        font.bold: true
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 10
                        rowSpacing: 8

                        Label { text: qsTr("Alignment") }
                        ComboBox {
                            Layout.fillWidth: true
                            model: root.alignmentOptions
                            textRole: "label"
                            currentIndex:
                                Math.max(
                                    0,
                                    root.alignmentOptions.findIndex(
                                        item => item.value ===
                                                root.metricsAlignment))
                            onActivated: index => {
                                root.metricsAlignment =
                                    root.alignmentOptions[index].value
                            }
                        }
                        Label { text: qsTr("Text color") }
                        TextField {
                            Layout.fillWidth: true
                            text: root.metricsColor
                            placeholderText: "#dcdcdc"
                            onEditingFinished:
                                root.metricsColor = text
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            text: root.runtime.samplingActive
                                  ? qsTr("Sampling is active")
                                  : qsTr("Sampling is inactive")
                            color: root.runtime.samplingActive
                                   ? "#4bd98d" : "#8d91a1"
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            text: root.runtime.metricsEnabled
                                  ? qsTr("Stop metrics overlay")
                                  : qsTr("Start metrics overlay")
                            enabled: !root.runtime.operationBusy
                            onClicked:
                                root.runtime.configureMetrics(
                                    !root.runtime.metricsEnabled,
                                    root.fullMetrics,
                                    root.metricsAlignment,
                                    root.metricsColor)
                        }
                    }
                }

                PrimaryButton {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Apply to display")
                    enabled: !root.runtime.operationBusy &&
                             ((!root.splitMode &&
                               root.selectedMedia.length === 1) ||
                              (root.splitMode &&
                               root.selectedMedia.length === 2))
                    onClicked: {
                        if (root.splitMode) {
                            root.runtime.applySplitScreen(
                                root.selectedMedia[0],
                                root.selectedMedia[1],
                                "Single",
                                root.leftMetrics,
                                root.rightMetrics,
                                root.leftBadges,
                                root.rightBadges)
                        } else {
                            root.runtime.applyFullScreen(
                                [root.selectedMedia[0]],
                                root.playMode,
                                root.fullMetrics,
                                root.fullBadges)
                        }
                    }
                }
            }
        }
            }
        }

        GroupBox {
            Layout.fillWidth: true
            title: qsTr("Screen controls")

            ColumnLayout {
                anchors.fill: parent
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Brightness")
                    }
                    Slider {
                        objectName: "brightnessSlider"
                        Layout.preferredWidth: 320
                        Layout.maximumWidth: 380
                        from: 0
                        to: 100
                        stepSize: 1
                        value: root.brightnessDraft
                        enabled: root.runtime.displayStateValid &&
                                 !root.runtime.operationBusy
                        onMoved:
                            root.brightnessDraft = Math.round(value)
                    }
                    Label {
                        Layout.preferredWidth: 34
                        text: root.brightnessDraft.toString()
                    }
                    Button {
                        text: qsTr("Apply brightness")
                        enabled: root.runtime.displayStateValid &&
                                 !root.runtime.operationBusy
                        onClicked:
                            root.runtime.setBrightness(
                                root.brightnessDraft)
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Backlight")
                    }
                    Label {
                        Layout.preferredWidth: 74
                        text: root.runtime.backlightEnabled
                              ? qsTr("On") : qsTr("Off")
                        color: root.runtime.backlightEnabled
                               ? "#66d18f" : "#9ca4ac"
                    }
                    Button {
                        text: root.runtime.backlightEnabled
                              ? qsTr("Turn display off")
                              : qsTr("Turn display on")
                        enabled: root.runtime.displayStateValid &&
                                 !root.runtime.operationBusy
                        onClicked: root.runtime.setBacklight(
                            !root.runtime.backlightEnabled)
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.preferredWidth: 92
                        text: qsTr("Orientation")
                    }
                    CheckBox {
                        text: qsTr("Mirror")
                        checked: root.mirrorDraft
                        onClicked: root.mirrorDraft = checked
                    }
                    CheckBox {
                        text: qsTr("Waterfall")
                        checked: root.waterfallDraft
                        onClicked: root.waterfallDraft = checked
                    }
                    Button {
                        text: qsTr("Apply orientation")
                        enabled: root.runtime.displayStateValid &&
                                 !root.runtime.operationBusy
                        onClicked: root.runtime.setOrientation(
                            root.mirrorDraft,
                            root.waterfallDraft)
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }

        GroupBox {
            objectName: "recentOperationsGroup"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(
                310, Math.max(115, operationList.count * 64 + 45))
            title: qsTr("Recent operations")

            ListView {
                id: operationList

                anchors.fill: parent
                clip: true
                model: root.runtime.operationModel
                delegate: ItemDelegate {
                    id: operationDelegate

                    required property string operationId
                    required property string subject
                    required property string operationState
                    required property string message
                    required property bool canRetry

                    width: ListView.view.width
                    height: 62
                    contentItem: RowLayout {
                        Label {
                            Layout.fillWidth: true
                            text: (operationDelegate.subject ||
                                   operationDelegate.operationId) +
                                  "\n" +
                                  operationDelegate.operationState +
                                  ": " + operationDelegate.message
                            elide: Text.ElideRight
                        }
                        Button {
                            text: qsTr("Retry")
                            visible: operationDelegate.canRetry
                            enabled: !root.runtime.operationBusy
                            onClicked:
                                root.runtime.retryOperation(
                                    operationDelegate.operationId)
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 20
        }
    }

    MediaFilePicker {
        id: mediaPicker
        editor: root.editor
    }

    MediaExportPicker {
        id: exportPicker
        workflow: root.deviceMedia
        homeFolder: root.editor.homeFolder
    }

    Connections {
        target: root.deviceMedia

        function onStateChanged() {
            if (root.deviceMedia.overwriteConfirmationPending) {
                if (!overwriteConfirmation.opened)
                    overwriteConfirmation.open()
            } else if (overwriteConfirmation.opened) {
                overwriteConfirmation.close()
            }
        }
    }

    Dialog {
        id: overwriteConfirmation

        objectName: "deviceMediaOverwriteConfirmation"
        title: qsTr("Replace exported file?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(520, root.width - 48)
        standardButtons: Dialog.Yes | Dialog.Cancel

        contentItem: Label {
            text: qsTr("“%1” already exists. Replace it with the device copy?")
                  .arg(root.deviceMedia.overwriteFileName)
            color: "#f4f6f7"
            wrapMode: Text.WordWrap
        }

        onAccepted: root.deviceMedia.confirmOverwrite()
        onRejected: {
            if (root.deviceMedia.overwriteConfirmationPending)
                root.deviceMedia.cancelOverwrite()
        }
    }

    Dialog {
        id: deleteConfirmation

        objectName: "deleteConfirmation"
        title: qsTr("Delete media")
        modal: true
        anchors.centerIn: parent
        width: Math.min(520, root.width - 48)
        standardButtons: Dialog.Yes | Dialog.Cancel

        contentItem: Label {
            text: qsTr("Delete “%1” from the device? This cannot be undone.")
                  .arg(root.pendingDeleteName)
            color: "#f4f6f7"
            wrapMode: Text.WordWrap
        }

        onAccepted: {
            const target = root.pendingDeleteName
            if (target.length > 0) {
                root.runtime.deleteMedia([target])
                root.selectedMedia =
                    root.selectedMedia.filter(
                        name => name !== target)
            }
            root.pendingDeleteName = ""
        }
        onRejected: root.pendingDeleteName = ""
    }
}
