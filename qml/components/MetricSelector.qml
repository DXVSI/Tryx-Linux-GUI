pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GroupBox {
    id: root

    required property string areaTitle
    required property string fieldPrefix
    required property var catalog
    required property bool catalogReady
    required property var availableMetrics
    required property var selectedMetrics

    property bool editable: true
    property int maximum: 3
    readonly property bool replacementOpen: replacementDialog.opened
    readonly property bool replacementVisible: replacementDialog.visible
    readonly property var visibleMetricTokens:
        visibleDefinitions.map(definition => definition.token)
    readonly property real layoutContentWidth: contentColumn.width
    readonly property var metricDefinitions: [
        {"token": "CPU Temperature", "group": "CPU",
         "label": qsTr("CPU temperature"), "suffix": "CpuTemperature"},
        {"token": "CPU Frequency", "group": "CPU",
         "label": qsTr("CPU frequency"), "suffix": "CpuFrequency"},
        {"token": "CPU Usage", "group": "CPU",
         "label": qsTr("CPU usage"), "suffix": "CpuUsage"},
        {"token": "CPU Power", "group": "CPU",
         "label": qsTr("CPU power"), "suffix": "CpuPower"},
        {"token": "GPU Temperature", "group": "GPU",
         "label": qsTr("GPU temperature"), "suffix": "GpuTemperature"},
        {"token": "GPU Frequency", "group": "GPU",
         "label": qsTr("GPU frequency"), "suffix": "GpuFrequency"},
        {"token": "GPU Usage", "group": "GPU",
         "label": qsTr("GPU usage"), "suffix": "GpuUsage"},
        {"token": "GPU Power", "group": "GPU",
         "label": qsTr("GPU power"), "suffix": "GpuPower"},
        {"token": "Memory Frequency", "group": "Memory",
         "label": qsTr("Memory frequency"), "suffix": "MemoryFrequency"},
        {"token": "Memory Usage", "group": "Memory",
         "label": qsTr("Memory usage"), "suffix": "MemoryUsage"},
        {"token": "Date&Time", "group": "System",
         "label": qsTr("Date and time"), "suffix": "DateAndTime"}
    ]
    readonly property var visibleDefinitions:
        filteredDefinitions(searchField.text)

    property string pendingMetric: ""
    property var pendingOrigin: null

    signal selectionEdited(var metrics)

    title: areaTitle
    implicitWidth: 280
    Layout.minimumWidth: 0
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("%1 metric selector").arg(areaTitle)
    Accessible.description: qsTr(
        "Choose up to %1 ordered overlay metrics.").arg(maximum)

    function selectedCopy() {
        return selectedMetrics ? selectedMetrics.slice(0) : []
    }

    function isSelected(token) {
        return selectedMetrics && selectedMetrics.indexOf(token) >= 0
    }

    function metricAvailable(token) {
        return availableMetrics && availableMetrics.indexOf(token) >= 0
    }

    function metricDefinition(token) {
        return metricDefinitions.find(
            definition => definition.token === token)
    }

    function metricLabel(token) {
        const definition = metricDefinition(token)
        return definition ? definition.label : token
    }

    function candidateTokens() {
        const tokens = catalogReady && catalog ? catalog.slice(0) : []
        const selected = selectedCopy()
        for (const token of selected) {
            if (tokens.indexOf(token) < 0)
                tokens.push(token)
        }
        return tokens
    }

    function filteredDefinitions(query) {
        const candidates = candidateTokens()
        const normalizedQuery = String(query || "").toLocaleLowerCase()
        return metricDefinitions.filter(definition => {
            if (candidates.indexOf(definition.token) < 0)
                return false
            return normalizedQuery.length === 0 ||
                   definition.label.toLocaleLowerCase()
                       .indexOf(normalizedQuery) >= 0
        })
    }

    function definitionsForGroup(group) {
        return visibleDefinitions.filter(
            definition => definition.group === group)
    }

    function requestToggle(token, origin) {
        if (!editable || !metricDefinition(token))
            return
        const next = selectedCopy()
        const existing = next.indexOf(token)
        if (existing >= 0) {
            next.splice(existing, 1)
            selectionEdited(next)
            return
        }
        if (!catalogReady || !catalog || catalog.indexOf(token) < 0 ||
                !metricAvailable(token)) {
            return
        }
        if (next.length < maximum) {
            next.push(token)
            selectionEdited(next)
            return
        }
        if (next.length !== maximum || replacementDialog.opened)
            return
        pendingMetric = token
        pendingOrigin = origin
        replacementDialog.open()
    }

    function cancelReplacement() {
        if (replacementDialog.opened)
            replacementDialog.close()
    }

    function replacementItemAt(position) {
        return replacementPositions.itemAt(position)
    }

    function replacementCancelItem() {
        return replacementCancel
    }

    function confirmReplacement(position) {
        const next = selectedCopy()
        const valid = replacementDialog.opened &&
            position >= 0 && position < maximum &&
            next.length === maximum &&
            pendingMetric.length > 0 &&
            next.indexOf(pendingMetric) < 0 &&
            catalogReady && catalog &&
            catalog.indexOf(pendingMetric) >= 0 &&
            metricAvailable(pendingMetric)
        if (valid) {
            next[position] = pendingMetric
            selectionEdited(next)
        }
        replacementDialog.close()
    }

    onVisibleChanged: {
        if (!visible && replacementDialog.opened)
            replacementDialog.close()
    }
    onEditableChanged: {
        if (!editable && replacementDialog.opened)
            replacementDialog.close()
    }

    ColumnLayout {
        id: contentColumn

        anchors.fill: parent
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: searchField

                objectName: root.fieldPrefix + "MetricSearch"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                activeFocusOnTab: true
                placeholderText: qsTr("Search metrics")
                selectByMouse: true
                Accessible.name: qsTr("Search %1 metrics")
                                     .arg(root.areaTitle)
            }

            Label {
                objectName: root.fieldPrefix + "MetricCounter"
                text: qsTr("%1 %2/%3")
                      .arg(root.areaTitle)
                      .arg(root.selectedMetrics
                           ? root.selectedMetrics.length : 0)
                      .arg(root.maximum)
                font.bold: true
                Accessible.role: Accessible.StaticText
                Accessible.name: text
                Accessible.description: qsTr(
                    "Selected metric count and maximum.")
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !root.catalogReady
            text: qsTr(
                "The metric catalog is unavailable. Existing selections can still be removed.")
            color: "#f0d27a"
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        Repeater {
            model: [
                {"id": "CPU", "label": qsTr("CPU")},
                {"id": "GPU", "label": qsTr("GPU")},
                {"id": "Memory", "label": qsTr("Memory")},
                {"id": "System", "label": qsTr("System")}
            ]

            delegate: ColumnLayout {
                id: groupColumn

                required property var modelData
                Layout.fillWidth: true
                spacing: 4
                visible: root.definitionsForGroup(modelData.id).length > 0

                Label {
                    objectName: root.fieldPrefix + "MetricGroup" +
                                groupColumn.modelData.id
                    Layout.fillWidth: true
                    text: groupColumn.modelData.label
                    font.bold: true
                    color: "#cdd3d8"
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8

                    Repeater {
                        model: root.definitionsForGroup(
                            groupColumn.modelData.id)

                        delegate: CheckBox {
                            id: metricChoice

                            required property var modelData
                            objectName: root.fieldPrefix + "Metric" +
                                        modelData.suffix
                            text: root.metricAvailable(modelData.token)
                                  ? modelData.label
                                  : qsTr("%1 (Unavailable)")
                                    .arg(modelData.label)
                            checked: root.isSelected(modelData.token)
                            width: Math.min(
                                implicitWidth,
                                parent ? parent.width : implicitWidth)
                            implicitHeight: Math.max(
                                44, implicitContentHeight +
                                    topPadding + bottomPadding)
                            enabled: root.editable &&
                                     (root.isSelected(modelData.token) ||
                                      (root.catalogReady &&
                                       root.metricAvailable(
                                           modelData.token)))
                            activeFocusOnTab: true
                            Accessible.name: text
                            Accessible.description:
                                !root.metricAvailable(modelData.token)
                                ? root.isSelected(modelData.token)
                                  ? qsTr(
                                      "This selected metric is currently unavailable. It can be removed or replaced and will display two hyphens on the device.")
                                  : qsTr(
                                      "This metric is currently unavailable and cannot be selected.")
                                : checked
                                  ? qsTr("Selected. Activate to remove it.")
                                  : qsTr("Activate to select this metric.")
                            contentItem: Text {
                                leftPadding: !metricChoice.mirrored &&
                                             metricChoice.indicator
                                             ? metricChoice.indicator.width +
                                               metricChoice.spacing : 0
                                rightPadding: metricChoice.mirrored &&
                                              metricChoice.indicator
                                              ? metricChoice.indicator.width +
                                                metricChoice.spacing : 0
                                text: metricChoice.text
                                font: metricChoice.font
                                color: metricChoice.enabled
                                       ? "#e8eaed" : "#7d8389"
                                wrapMode: Text.WordWrap
                                verticalAlignment: Text.AlignVCenter
                            }
                            nextCheckState: function() {
                                return checkState
                            }
                            onClicked: root.requestToggle(
                                modelData.token, metricChoice)
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.visibleDefinitions.length === 0 &&
                     root.catalogReady
            text: qsTr("No metrics match the search.")
            color: "#9da4aa"
            wrapMode: Text.WordWrap
            Accessible.name: text
        }
    }

    Dialog {
        id: replacementDialog

        objectName: root.fieldPrefix + "MetricReplacementDialog"
        parent: Overlay.overlay
        x: parent ? Math.max(16, Math.round(
                                (parent.width - width) / 2)) : 0
        y: parent ? Math.max(16, Math.round(
                                (parent.height - height) / 2)) : 0
        width: parent ? Math.min(520, parent.width - 32) : 520
        height: parent ? Math.min(implicitHeight,
                                  parent.height - 32) : implicitHeight
        modal: true
        focus: true
        padding: 20
        closePolicy: Popup.NoAutoClose
        title: qsTr("Replace a metric")

        onOpened: {
            const first = replacementPositions.itemAt(0)
            if (first)
                first.forceActiveFocus()
        }
        onClosed: {
            const focusItem = root.pendingOrigin
            root.pendingMetric = ""
            root.pendingOrigin = null
            if (focusItem && focusItem.forceActiveFocus)
                focusItem.forceActiveFocus()
        }

        background: Rectangle {
            radius: 12
            color: "#1b1f23"
            border.width: 1
            border.color: "#7f8d36"
        }

        contentItem: ColumnLayout {
            spacing: 12
            Accessible.role: Accessible.Dialog
            Accessible.name: replacementDialog.title
            Accessible.description: replacementExplanation.text
            Keys.onEscapePressed: event => {
                root.cancelReplacement()
                event.accepted = true
            }

            Label {
                id: replacementExplanation

                Layout.fillWidth: true
                text: qsTr(
                    "%1 already has %2 metrics. Choose the exact position to replace with %3.")
                      .arg(root.areaTitle)
                      .arg(root.maximum)
                      .arg(root.metricLabel(root.pendingMetric))
                wrapMode: Text.WordWrap
            }

            Repeater {
                id: replacementPositions

                model: root.selectedMetrics
                    ? root.selectedMetrics.slice(0, root.maximum) : []

                delegate: Button {
                    id: replacementPosition

                    required property int index
                    required property string modelData
                    objectName: root.fieldPrefix +
                                "MetricReplacePosition" + index
                    Layout.fillWidth: true
                    Layout.minimumHeight: 44
                    activeFocusOnTab: true
                    text: qsTr("Replace position %1: %2")
                          .arg(index + 1)
                          .arg(root.metricLabel(modelData))
                    Accessible.name: text
                    Accessible.description: qsTr(
                        "Replace only this ordered metric position.")
                    contentItem: Text {
                        text: replacementPosition.text
                        font: replacementPosition.font
                        color: "#e8eaed"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                    }
                    onClicked: root.confirmReplacement(index)
                }
            }

            Button {
                id: replacementCancel

                objectName: root.fieldPrefix + "MetricReplaceCancel"
                Layout.fillWidth: true
                Layout.minimumHeight: 44
                activeFocusOnTab: true
                text: qsTr("Cancel")
                Accessible.name: text
                Accessible.description: qsTr(
                    "Close without changing the metric selection.")
                onClicked: root.cancelReplacement()
            }
        }
    }
}
