import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: root

    property bool selected: false
    property color accentColor: "#def750"

    implicitHeight: 46
    leftPadding: 14
    rightPadding: 14
    hoverEnabled: true
    flat: true

    contentItem: RowLayout {
        spacing: 10

        Rectangle {
            Layout.preferredWidth: 3
            Layout.preferredHeight: 18
            radius: 2
            color: root.selected ? "#11140b" : "transparent"
        }

        Label {
            Layout.fillWidth: true
            text: root.text
            color: root.selected ? "#11140b" : "#d7dce0"
            font.pixelSize: 14
            font.bold: root.selected
            verticalAlignment: Text.AlignVCenter
        }
    }

    background: Rectangle {
        radius: 9
        color: root.selected
               ? root.accentColor
               : (root.hovered ? "#2a3036" : "transparent")
        border.width: root.activeFocus ? 1 : 0
        border.color: root.selected ? "#11140b" : root.accentColor
    }
}
