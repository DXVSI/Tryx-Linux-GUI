import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: root

    required property var runtime

    visible: root.runtime.operationBusy
    padding: 12

    background: Rectangle {
        radius: 8
        color: "#23282d"
        border.color: "#7f8d36"
    }

    RowLayout {
        anchors.fill: parent
        spacing: 12

        BusyIndicator {
            running: root.runtime.operationBusy
            visible: root.runtime.operationProgress <= 0
        }
        ColumnLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: root.runtime.operationSummary
                wrapMode: Text.WordWrap
            }
            ProgressBar {
                Layout.fillWidth: true
                visible: root.runtime.operationProgress > 0
                value: root.runtime.operationProgress
            }
        }
        Button {
            text: qsTr("Cancel")
            onClicked: root.runtime.cancelActiveOperation()
        }
    }
}
