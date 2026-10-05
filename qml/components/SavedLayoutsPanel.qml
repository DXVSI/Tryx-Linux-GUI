pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GroupBox {
    id: panel

    required property var runtime
    property bool actionsAllowed: false
    property bool saveAllowed: false
    property string actionsBlockReason: ""
    property bool draftDirty: false

    signal saveRequested(string name, string overwriteLayoutId)
    signal loadRequested(string layoutId)
    signal deleteRequested(string layoutId)

    title: qsTr("Saved layouts")

    property string pendingName: ""
    property string pendingLayoutId: ""
    property var focusRestoreItem: null
    property bool focusRestorePending: false

    function confirmationOpen() {
        return overwriteDialog.visible || replaceDraftDialog.visible ||
               deleteDialog.visible
    }

    function clearPendingConfirmation() {
        pendingName = ""
        pendingLayoutId = ""
    }

    function invalidatePendingConfirmation() {
        if (!confirmationOpen())
            return false
        if (overwriteDialog.opened)
            overwriteDialog.close()
        if (replaceDraftDialog.opened)
            replaceDraftDialog.close()
        if (deleteDialog.opened)
            deleteDialog.close()
        clearPendingConfirmation()
        return true
    }

    function handleRuntimeContextChanged() {
        if (!invalidatePendingConfirmation())
            restoreFocus()
    }

    function rememberFocus(item) {
        focusRestoreItem = item
        focusRestorePending = true
    }

    function restoreFocus() {
        if (!focusRestorePending || runtime.savedLayoutsBusy ||
            confirmationOpen())
            return
        const item = focusRestoreItem
        focusRestoreItem = null
        focusRestorePending = false
        const target = item && item.visible && item.enabled
                       ? item : nameField
        if (target && target.visible && target.enabled)
            Qt.callLater(function() { target.forceActiveFocus() })
    }

    function requestSave(item) {
        if (!runtime.capabilitiesReady ||
            !runtime.savedLayoutsReady || runtime.savedLayoutsBusy ||
            !saveAllowed)
            return
        const name = nameField.text.trim()
        if (name.length < 1 || name.length > 80)
            return
        const existingId = runtime.savedLayoutIdForName(name)
        if (existingId.length > 0) {
            pendingName = name
            pendingLayoutId = existingId
            rememberFocus(item)
            overwriteDialog.open()
            return
        }
        saveRequested(name, "")
    }

    function requestLoad(item, layoutId) {
        if (!actionsAllowed || !runtime.savedLayoutsReady ||
            runtime.savedLayoutsBusy || layoutId.length === 0)
            return
        if (draftDirty) {
            pendingLayoutId = layoutId
            rememberFocus(item)
            replaceDraftDialog.open()
            return
        }
        loadRequested(layoutId)
    }

    function requestDelete(item, layoutId) {
        if (!actionsAllowed || !runtime.savedLayoutsReady ||
            runtime.savedLayoutsBusy || layoutId.length === 0)
            return
        pendingLayoutId = layoutId
        rememberFocus(item)
        deleteDialog.open()
    }

    function confirmOverwrite() {
        if (!runtime.capabilitiesReady ||
            !runtime.savedLayoutsReady || runtime.savedLayoutsBusy ||
            !saveAllowed || pendingLayoutId.length === 0)
            return
        saveRequested(pendingName, pendingLayoutId)
    }

    function confirmLoad() {
        if (actionsAllowed && !runtime.savedLayoutsBusy &&
            pendingLayoutId.length > 0)
            loadRequested(pendingLayoutId)
    }

    function confirmDelete() {
        if (actionsAllowed && !runtime.savedLayoutsBusy &&
            pendingLayoutId.length > 0)
            deleteRequested(pendingLayoutId)
    }

    function statusText() {
        if (!runtime.capabilitiesReady)
            return qsTr("Waiting for the current display handshake…")
        if (!runtime.savedLayoutsSupported)
            return qsTr("Saved layouts are not supported by this runtime.")
        if (runtime.savedLayoutsBusy)
            return qsTr("Updating saved layouts…")
        switch (runtime.savedLayoutsStatus) {
        case "Disconnected":
            return qsTr("Connect a supported display to view its saved layouts.")
        case "Unsupported":
            return qsTr("Saved layouts are not available for this display model.")
        case "Unavailable":
            return runtime.savedLayoutsDiagnostic.length > 0
                   ? runtime.savedLayoutsDiagnostic
                   : qsTr("Saved layouts are temporarily unavailable.")
        case "Ready":
            return layoutList.count === 0
                   ? qsTr("No layouts saved for this display.")
                   : ""
        default:
            return qsTr("Waiting for the current display handshake…")
        }
    }

    Connections {
        target: panel.runtime
        ignoreUnknownSignals: true

        function onSavedLayoutsChanged() {
            panel.handleRuntimeContextChanged()
        }

        function onSavedLayoutsBusyChanged() {
            panel.handleRuntimeContextChanged()
        }

        function onCapabilitiesChanged() {
            panel.handleRuntimeContextChanged()
        }
    }

    onActionsAllowedChanged: {
        if (!actionsAllowed &&
            (replaceDraftDialog.opened || deleteDialog.opened))
            invalidatePendingConfirmation()
    }
    onSaveAllowedChanged: {
        if (!saveAllowed && overwriteDialog.opened)
            invalidatePendingConfirmation()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        GridLayout {
            Layout.fillWidth: true
            columns: panel.width < 430 ? 1 : 2
            columnSpacing: 8
            rowSpacing: 8

            TextField {
                id: nameField

                objectName: "savedLayoutNameField"
                Layout.fillWidth: true
                maximumLength: 80
                placeholderText: qsTr("Layout name")
                Accessible.name: qsTr("Saved layout name")
                onAccepted: panel.requestSave(saveButton)
            }

            Button {
                id: saveButton

                objectName: "saveCurrentLayoutButton"
                Layout.fillWidth: panel.width < 430
                Layout.minimumHeight: 44
                text: qsTr("Save current")
                enabled: panel.runtime.savedLayoutsReady &&
                         !panel.runtime.savedLayoutsBusy &&
                         panel.saveAllowed &&
                         nameField.text.trim().length > 0
                Accessible.name: text
                Accessible.description: panel.actionsBlockReason
                onClicked: panel.requestSave(this)
            }
        }

        Label {
            objectName: "savedLayoutsBlockReason"
            Layout.fillWidth: true
            visible: !panel.actionsAllowed &&
                     panel.actionsBlockReason.length > 0
            text: panel.actionsBlockReason
            color: "#f0d27a"
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        Label {
            objectName: "savedLayoutsStatusText"
            Layout.fillWidth: true
            visible: text.length > 0
            text: panel.statusText()
            color: panel.runtime.savedLayoutsStatus === "Unavailable"
                   ? "#f0a0a8" : "#8d91a1"
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.StatusBar
            Accessible.name: text
        }

        ListView {
            id: layoutList

            objectName: "savedLayoutsList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(
                260, Math.max(0, contentHeight))
            visible: panel.runtime.savedLayoutsReady && count > 0
            clip: true
            spacing: 8
            model: panel.runtime.savedLayoutModel

            delegate: Frame {
                id: layoutDelegate

                required property string layoutId
                required property string layoutRevision
                required property string layoutName
                required property string screenMode
                required property var mediaNames

                objectName: "savedLayoutRow-" + layoutId
                width: ListView.view.width
                implicitHeight: delegateLayout.implicitHeight + 20

                GridLayout {
                    id: delegateLayout

                    anchors.fill: parent
                    anchors.margins: 10
                    columns: panel.width < 430 ? 1 : 3
                    columnSpacing: 8
                    rowSpacing: 6

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Label {
                            Layout.fillWidth: true
                            text: layoutDelegate.layoutName
                            font.bold: true
                            elide: Text.ElideRight
                            Accessible.name: text
                        }
                        Label {
                            Layout.fillWidth: true
                            text: (layoutDelegate.screenMode ===
                                   "Screen Splitting"
                                   ? qsTr("Split") : qsTr("Full")) +
                                  ": " +
                                  layoutDelegate.mediaNames.join(" + ")
                            color: "#8d91a1"
                            elide: Text.ElideMiddle
                        }
                    }

                    Button {
                        objectName: "loadSavedLayout-" +
                                    layoutDelegate.layoutId
                        Layout.fillWidth: panel.width < 430
                        Layout.minimumHeight: 44
                        text: qsTr("Load into draft")
                        enabled: panel.actionsAllowed &&
                                 !panel.runtime.savedLayoutsBusy
                        Accessible.name: text
                        Accessible.description: panel.actionsBlockReason
                        onClicked: panel.requestLoad(
                            this, layoutDelegate.layoutId)
                    }

                    Button {
                        objectName: "deleteSavedLayout-" +
                                    layoutDelegate.layoutId
                        Layout.fillWidth: panel.width < 430
                        Layout.minimumHeight: 44
                        text: qsTr("Delete")
                        enabled: panel.actionsAllowed &&
                                 !panel.runtime.savedLayoutsBusy
                        Accessible.name: text
                        Accessible.description: panel.actionsBlockReason
                        onClicked: panel.requestDelete(
                            this, layoutDelegate.layoutId)
                    }
                }
            }
        }
    }

    Dialog {
        id: overwriteDialog

        objectName: "savedLayoutOverwriteDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Overwrite saved layout?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        onAccepted: panel.confirmOverwrite()
        onClosed: {
            panel.clearPendingConfirmation()
            panel.restoreFocus()
        }

        Label {
            objectName: "savedLayoutOverwritePrompt"
            width: Math.min(420, implicitWidth)
            text: qsTr("A layout with this name already exists. Replace its saved state?")
            wrapMode: Text.WordWrap
            Accessible.name: text
        }
    }

    Dialog {
        id: replaceDraftDialog

        objectName: "savedLayoutReplaceDraftDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Replace current draft?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        onAccepted: panel.confirmLoad()
        onClosed: {
            panel.clearPendingConfirmation()
            panel.restoreFocus()
        }

        Label {
            objectName: "savedLayoutReplaceDraftPrompt"
            width: Math.min(420, implicitWidth)
            text: qsTr("The current display draft has unsaved changes. Replace it with the selected layout?")
            wrapMode: Text.WordWrap
            Accessible.name: text
        }
    }

    Dialog {
        id: deleteDialog

        objectName: "savedLayoutDeleteDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Delete saved layout?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        onAccepted: panel.confirmDelete()
        onClosed: {
            panel.clearPendingConfirmation()
            panel.restoreFocus()
        }

        Label {
            objectName: "savedLayoutDeletePrompt"
            width: Math.min(420, implicitWidth)
            text: qsTr("This removes only the saved layout. Media on the display is not deleted.")
            wrapMode: Text.WordWrap
            Accessible.name: text
        }
    }
}
