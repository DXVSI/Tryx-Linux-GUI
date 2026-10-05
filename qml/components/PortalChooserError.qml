pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Dialog {
    id: root
    objectName: "portalChooserGuard"
    property var chooser: null
    property string message: ""
    readonly property bool pending: chooser !== null && chooser.busy
    parent: Overlay.overlay
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    width: parent ? Math.min(520, parent.width - 48) : 520
    title: qsTr("System file chooser")
    modal: true
    focus: true
    standardButtons: pending ? Dialog.Cancel : Dialog.Ok
    onPendingChanged: {
        if (pending) {
            message = qsTr("Complete the selection in the system file chooser, or cancel here.")
            open()
        } else {
            close()
        }
    }
    onRejected: {
        if (pending)
            chooser.cancel()
    }
    contentItem: Label {
        text: root.message
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        Accessible.name: text
    }
    Connections {
        target: root.chooser
        function onFailed(message) {
            root.message = message
            root.open()
        }
    }
}
