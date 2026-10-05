import QtQuick

MouseArea {
    id: root

    required property var chrome
    required property int edges

    acceptedButtons: Qt.LeftButton
    hoverEnabled: true
    z: 1000

    onPressed: mouse => {
        root.chrome.startResize(root.edges)
        mouse.accepted = true
    }
}
