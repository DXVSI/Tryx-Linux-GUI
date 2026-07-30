import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: root

    required property string title
    required property string valueText
    property string subtitle: ""
    property string details: ""
    property real percentage: -1
    property color accentColor: "#def750"

    implicitHeight: 166
    padding: 18

    background: Rectangle {
        radius: 12
        color: "#23282d"
        border.width: 1
        border.color: "#343b42"
    }

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: root.title
            color: "#f4f6f7"
            font.pixelSize: 15
            font.bold: true
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            visible: root.subtitle.length > 0
            text: root.subtitle
            color: "#9ca4ac"
            font.pixelSize: 11
            elide: Text.ElideRight
        }

        Item { Layout.fillHeight: true }

        Label {
            text: root.valueText
            color: root.accentColor
            font.pixelSize: 30
            font.bold: true
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 6
            visible: root.percentage >= 0
            radius: 3
            color: "#15181b"

            Rectangle {
                width: parent.width *
                       Math.max(0, Math.min(100,
                                           root.percentage)) / 100
                height: parent.height
                radius: parent.radius
                color: root.accentColor
            }
        }

        Label {
            Layout.fillWidth: true
            text: root.details
            color: "#9ca4ac"
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }
}
