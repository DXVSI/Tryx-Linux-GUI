import QtQuick
import QtQuick.Controls

Button {
    id: control

    highlighted: true

    contentItem: Label {
        text: control.text
        font: control.font
        color: control.enabled ? "#171a1e" : "#6f7560"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
