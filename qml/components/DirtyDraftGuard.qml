pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "dirtyDraftGuard"

    enum Intent {
        NoIntent,
        RouteIntent,
        HideToTrayIntent,
        WindowCloseIntent,
        ExplicitQuitIntent
    }

    property bool applyEnabled: false
    property bool applying: false
    property bool unresolved: false
    property string unresolvedMessage: ""

    signal applyRequested(int intent, var target)
    signal discardRequested(int intent, var target)
    signal stayRequested()

    property int _pendingIntent: DirtyDraftGuard.NoIntent
    property var _pendingTarget
    property var _focusReturnItem
    readonly property bool opened: dialog.opened
    readonly property Item contentItem: dialog.contentItem

    x: dialog.x
    y: dialog.y
    width: dialog.width
    height: dialog.height

    Accessible.ignored: !root.opened
    Accessible.role: Accessible.Dialog
    Accessible.name: dialog.title
    Accessible.description: explanation.text

    function request(intent, target) {
        if (intent <= DirtyDraftGuard.NoIntent
                || intent > DirtyDraftGuard.ExplicitQuitIntent
                || dialog.opened
                || root._pendingIntent !== DirtyDraftGuard.NoIntent) {
            return false
        }

        const window = root.Window.window
        root._focusReturnItem = window
                ? window.activeFocusItem : null
        root._pendingIntent = intent
        root._pendingTarget = target
        dialog.open()
        return true
    }

    function stay() {
        if (!dialog.opened)
            return
        root.stayRequested()
        dialog.close()
    }

    function complete() {
        if (dialog.visible)
            dialog.close()
    }

    function discard() {
        if (!dialog.opened || root.applying)
            return
        root.discardRequested(root._pendingIntent,
                              root._pendingTarget)
        dialog.close()
    }

    function apply() {
        if (!dialog.opened || root.applying || root.unresolved
                || !root.applyEnabled) {
            return
        }
        root.applyRequested(root._pendingIntent,
                            root._pendingTarget)
    }

    Dialog {
        id: dialog

        parent: Overlay.overlay
        x: parent ? Math.max(16, Math.round(
                                (parent.width - width) / 2)) : 0
        y: parent ? Math.max(16, Math.round(
                                (parent.height - height) / 2)) : 0
        width: parent ? Math.min(560, parent.width - 32) : 560
        height: parent ? Math.min(implicitHeight,
                                  parent.height - 32) : implicitHeight
        modal: true
        focus: true
        padding: 20
        closePolicy: Popup.NoAutoClose
        title: qsTr("Unsaved display changes")

        onOpened: stayButton.forceActiveFocus()
        onClosed: {
            const focusItem = root._focusReturnItem
            root._pendingIntent = DirtyDraftGuard.NoIntent
            root._pendingTarget = undefined
            root._focusReturnItem = null
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
            spacing: 16
            Keys.onEscapePressed: event => {
                root.stay()
                event.accepted = true
            }

            Label {
                id: explanation

                Layout.fillWidth: true
                text: qsTr("Apply or discard your display changes before continuing.")
                color: "#d8dde1"
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                visible: root.applying
                text: qsTr("Applying display changes…")
                color: "#f0d27a"
                wrapMode: Text.WordWrap
                Accessible.name: text
            }

            Label {
                id: status

                objectName: "dirtyDraftStatus"
                Layout.fillWidth: true
                visible: root.unresolved
                         || root.unresolvedMessage.length > 0
                text: root.unresolvedMessage.length > 0
                      ? root.unresolvedMessage
                      : qsTr("The display result could not be confirmed. Stay to review the device state, or discard only the local draft.")
                color: "#f0d27a"
                font.bold: true
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }

            Item {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                implicitHeight: actions.implicitHeight

                GridLayout {
                    id: actions

                    objectName: "dirtyDraftActionGrid"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    columns: root.width < 520 ? 1 : 3
                    columnSpacing: 10
                    rowSpacing: 10

                    Button {
                        id: stayButton

                        objectName: "dirtyDraftStayButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        text: qsTr("Stay")
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description: qsTr(
                            "Keep the draft and cancel this navigation or close request.")
                        onClicked: root.stay()
                    }

                    Button {
                        id: discardButton

                        objectName: "dirtyDraftDiscardButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        text: qsTr("Discard changes")
                        enabled: !root.applying
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description: qsTr(
                            "Discard only the local draft. This does not undo a device operation.")
                        onClicked: root.discard()
                    }

                    PrimaryButton {
                        id: applyButton

                        objectName: "dirtyDraftApplyButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        text: qsTr("Apply changes")
                        enabled: root.applyEnabled
                                 && !root.applying
                                 && !root.unresolved
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description: qsTr(
                            "Submit the current display draft and continue only after confirmation.")
                        onClicked: root.apply()
                    }
                }
            }
        }
    }
}
