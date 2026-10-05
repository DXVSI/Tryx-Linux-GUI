pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase

    name: "SavedLayoutsPanel"
    when: hostWindow.visible

    ApplicationWindow {
        id: hostWindow

        width: 900
        height: 700
        visible: true
    }

    QtObject {
        id: layoutState

        property list<QtObject> rows: []
    }

    Component {
        id: layoutRowComponent

        QtObject {
            required property string layoutId
            required property string layoutRevision
            required property string layoutName
            required property string screenMode
            required property var mediaNames
        }
    }

    QtObject {
        id: runtimeMock

        property bool capabilitiesReady: true
        property bool startBusyOnSave: false
        property bool savedLayoutsSupported: true
        property bool savedLayoutsReady: true
        property bool savedLayoutsBusy: false
        property string savedLayoutsStatus: "Ready"
        property string savedLayoutsDiagnostic: ""
        property var savedLayoutModel: layoutState.rows
        signal savedLayoutsChanged()

        function savedLayoutIdForName(name) {
            const normalized = String(name).toLowerCase()
            for (let index = 0; index < layoutState.rows.length; ++index) {
                const layout = layoutState.rows[index]
                if (String(layout.layoutName).toLowerCase() === normalized)
                    return String(layout.layoutId)
            }
            return ""
        }
    }

    Component {
        id: panelComponent

        Components.SavedLayoutsPanel {
            runtime: runtimeMock
            actionsAllowed: true
            saveAllowed: true
            onSaveRequested: {
                if (runtimeMock.startBusyOnSave)
                    runtimeMock.savedLayoutsBusy = true
            }
        }
    }

    Component {
        id: signalSpyComponent

        SignalSpy {}
    }

    function addLayout(layoutId, name, split) {
        const row = createTemporaryObject(layoutRowComponent, testCase, {
            "layoutId": layoutId,
            "layoutRevision": "7",
            "layoutName": name,
            "screenMode": split
                          ? "Screen Splitting" : "Full Screen",
            "mediaNames": split
                          ? ["left.mp4.h264_1120x1080",
                             "right.mp4.h264_1120x1080"]
                          : ["full.mp4.h264_2240x1080"]
        })
        verify(row !== null)
        const next = Array.from(layoutState.rows)
        next.push(row)
        layoutState.rows = next
    }

    function createPanel(properties) {
        const panel = createTemporaryObject(
            panelComponent, hostWindow.contentItem,
            Object.assign({"width": 680, "height": 560},
                          properties || {}))
        verify(panel !== null)
        wait(0)
        return panel
    }

    function spy(target, signalName) {
        const created = createTemporaryObject(
            signalSpyComponent, testCase,
            {"target": target, "signalName": signalName})
        verify(created !== null)
        return created
    }

    function dialogButton(dialog, standardButton) {
        const button = dialog.standardButton(standardButton)
        verify(button !== null)
        return button
    }

    function init() {
        layoutState.rows = []
        runtimeMock.capabilitiesReady = true
        runtimeMock.startBusyOnSave = false
        runtimeMock.savedLayoutsSupported = true
        runtimeMock.savedLayoutsReady = true
        runtimeMock.savedLayoutsBusy = false
        runtimeMock.savedLayoutsStatus = "Ready"
        runtimeMock.savedLayoutsDiagnostic = ""
        hostWindow.width = 900
        hostWindow.height = 700
        hostWindow.requestActivate()
        tryVerify(() => hostWindow.active)
        wait(0)
    }

    function cleanup() {
        // Detach delegates before QtTest destroys the temporary model objects.
        layoutState.rows = []
        wait(0)
    }

    function test_readyEmptyIsDistinctFromUnsupportedAndUnavailable() {
        const panel = createPanel()
        const status = findChild(panel, "savedLayoutsStatusText")
        const list = findChild(panel, "savedLayoutsList")
        verify(status !== null)
        verify(list !== null)

        compare(status.text, "No layouts saved for this display.")
        verify(status.visible)
        verify(!list.visible)

        runtimeMock.capabilitiesReady = false
        wait(0)
        compare(status.text,
                "Waiting for the current display handshake…")

        runtimeMock.capabilitiesReady = true

        runtimeMock.savedLayoutsSupported = false
        runtimeMock.savedLayoutsReady = false
        runtimeMock.savedLayoutsStatus = "Unavailable"
        wait(0)
        compare(status.text,
                "Saved layouts are not supported by this runtime.")
        verify(status.visible)
        verify(!list.visible)

        runtimeMock.savedLayoutsSupported = true
        runtimeMock.savedLayoutsStatus = "Unavailable"
        runtimeMock.savedLayoutsDiagnostic =
            "Saved layout store failed its safety checks."
        wait(0)
        compare(status.text,
                "Saved layout store failed its safety checks.")
        verify(status.visible)
        compare(status.Accessible.role, Accessible.StatusBar)
        compare(status.Accessible.name, status.text)
    }

    function test_readyListReplacesEmptyState() {
        addLayout("layout-a", "Desk", false)
        const panel = createPanel()
        const status = findChild(panel, "savedLayoutsStatusText")
        const list = findChild(panel, "savedLayoutsList")
        verify(status !== null)
        verify(list !== null)

        compare(status.text, "")
        verify(!status.visible)
        verify(list.visible)
        compare(list.count, 1)
        tryVerify(() => list.itemAtIndex(0) !== null)
        compare(list.itemAtIndex(0).objectName,
                "savedLayoutRow-layout-a")
    }

    function test_nameIsBoundedAndExistingNameRequiresOverwrite() {
        addLayout("layout-a", "Desk", false)
        const panel = createPanel()
        const saveSpy = spy(panel, "saveRequested")
        const nameField = findChild(panel, "savedLayoutNameField")
        const saveButton = findChild(panel, "saveCurrentLayoutButton")
        verify(nameField !== null)
        verify(saveButton !== null)

        const eightyCharacters = Array(81).join("x")
        nameField.text = eightyCharacters + "ignored"
        compare(nameField.text.length, 80)
        compare(nameField.maximumLength, 80)
        mouseClick(saveButton)
        compare(saveSpy.count, 1)
        compare(saveSpy.signalArguments[0][0], eightyCharacters)
        compare(saveSpy.signalArguments[0][1], "")

        saveSpy.clear()
        nameField.text = "desk"
        saveButton.forceActiveFocus()
        tryVerify(() => saveButton.activeFocus)
        mouseClick(saveButton)
        const dialog = findChild(
            hostWindow.contentItem, "savedLayoutOverwriteDialog")
        verify(dialog !== null)
        tryVerify(() => dialog.opened)
        compare(saveSpy.count, 0)

        const accept = dialogButton(dialog, Dialog.Ok)
        verify(accept.activeFocusOnTab)
        keyClick(Qt.Key_Escape)
        tryVerify(() => !dialog.opened)
        tryVerify(() => saveButton.activeFocus)
        compare(saveSpy.count, 0)

        mouseClick(saveButton)
        tryVerify(() => dialog.opened)
        mouseClick(dialogButton(dialog, Dialog.Ok))
        tryVerify(() => !dialog.opened)
        tryVerify(() => saveButton.activeFocus)
        compare(saveSpy.count, 1)
        compare(saveSpy.signalArguments[0][0], "desk")
        compare(saveSpy.signalArguments[0][1], "layout-a")
    }

    function test_enterCannotBypassDisabledSaveAction() {
        const panel = createPanel({"saveAllowed": false})
        const saveSpy = spy(panel, "saveRequested")
        const nameField = findChild(panel, "savedLayoutNameField")
        verify(nameField !== null)

        nameField.text = "Blocked"
        nameField.forceActiveFocus()
        keyClick(Qt.Key_Return)
        compare(saveSpy.count, 0)

        panel.saveAllowed = true
        runtimeMock.savedLayoutsBusy = true
        keyClick(Qt.Key_Return)
        compare(saveSpy.count, 0)
    }

    function test_backgroundRefreshDoesNotStealFocus() {
        const panel = createPanel()
        const nameField = findChild(panel, "savedLayoutNameField")
        const saveButton = findChild(panel, "saveCurrentLayoutButton")
        verify(nameField !== null)
        verify(saveButton !== null)
        nameField.text = "Ready"
        saveButton.forceActiveFocus()
        tryVerify(() => saveButton.activeFocus)

        runtimeMock.savedLayoutsChanged()
        wait(0)
        verify(saveButton.activeFocus)
    }

    function test_dirtyLoadRequiresConfirmationAndEscapeRestoresFocus() {
        addLayout("layout-a", "Desk", false)
        const panel = createPanel()
        const loadSpy = spy(panel, "loadRequested")
        const list = findChild(panel, "savedLayoutsList")
        tryVerify(() => list.itemAtIndex(0) !== null)
        const loadButton = findChild(panel, "loadSavedLayout-layout-a")
        verify(loadButton !== null)

        mouseClick(loadButton)
        compare(loadSpy.count, 1)
        compare(loadSpy.signalArguments[0][0], "layout-a")

        loadSpy.clear()
        panel.draftDirty = true
        loadButton.forceActiveFocus()
        tryVerify(() => loadButton.activeFocus)
        panel.draftDirty = true
        compare(panel.actionsAllowed, true)
        compare(runtimeMock.savedLayoutsReady, true)
        compare(runtimeMock.savedLayoutsBusy, false)
        compare(panel.draftDirty, true)
        panel.requestLoad(loadButton, "layout-a")
        compare(panel.pendingLayoutId, "layout-a")
        const dialog = findChild(
            hostWindow.contentItem, "savedLayoutReplaceDraftDialog")
        verify(dialog !== null)
        tryVerify(() => dialog.opened)
        compare(loadSpy.count, 0)

        keyClick(Qt.Key_Escape)
        tryVerify(() => !dialog.opened)
        tryVerify(() => loadButton.activeFocus)
        compare(loadSpy.count, 0)

        mouseClick(loadButton)
        tryVerify(() => dialog.opened)
        mouseClick(dialogButton(dialog, Dialog.Ok))
        tryVerify(() => !dialog.opened)
        tryVerify(() => loadButton.activeFocus)
        compare(loadSpy.count, 1)
        compare(loadSpy.signalArguments[0][0], "layout-a")
    }

    function test_mutationBusyDefersFocusRestoreUntilCompletion() {
        addLayout("layout-a", "Desk", false)
        const panel = createPanel()
        const saveSpy = spy(panel, "saveRequested")
        const nameField = findChild(panel, "savedLayoutNameField")
        const saveButton = findChild(panel, "saveCurrentLayoutButton")
        verify(nameField !== null)
        verify(saveButton !== null)

        nameField.text = "desk"
        saveButton.forceActiveFocus()
        mouseClick(saveButton)
        const dialog = findChild(
            hostWindow.contentItem, "savedLayoutOverwriteDialog")
        verify(dialog !== null)
        tryVerify(() => dialog.opened)

        runtimeMock.startBusyOnSave = true
        mouseClick(dialogButton(dialog, Dialog.Ok))
        tryVerify(() => !dialog.opened)
        compare(saveSpy.count, 1)
        verify(!saveButton.enabled)

        runtimeMock.savedLayoutsBusy = false
        tryVerify(() => saveButton.activeFocus)
    }

    function test_confirmationCannotDispatchAfterRuntimeInvalidation() {
        addLayout("layout-a", "Desk", false)
        const panel = createPanel({"draftDirty": true})
        const saveSpy = spy(panel, "saveRequested")
        const loadSpy = spy(panel, "loadRequested")
        const deleteSpy = spy(panel, "deleteRequested")
        const nameField = findChild(panel, "savedLayoutNameField")
        const saveButton = findChild(panel, "saveCurrentLayoutButton")
        const list = findChild(panel, "savedLayoutsList")
        verify(nameField !== null)
        verify(saveButton !== null)
        verify(list !== null)
        tryVerify(() => list.itemAtIndex(0) !== null)
        let loadButton = findChild(panel, "loadSavedLayout-layout-a")
        let deleteButton = findChild(
            panel, "deleteSavedLayout-layout-a")
        verify(loadButton !== null)
        verify(deleteButton !== null)

        nameField.text = "desk"
        saveButton.forceActiveFocus()
        mouseClick(saveButton)
        const overwriteDialog = findChild(
            hostWindow.contentItem, "savedLayoutOverwriteDialog")
        verify(overwriteDialog !== null)
        tryVerify(() => overwriteDialog.opened)

        runtimeMock.capabilitiesReady = false
        runtimeMock.savedLayoutsReady = false
        runtimeMock.savedLayoutsChanged()
        tryVerify(() => !overwriteDialog.opened)
        tryVerify(() => !overwriteDialog.visible)
        compare(panel.pendingName, "")
        compare(panel.pendingLayoutId, "")
        compare(saveSpy.count, 0)
        tryVerify(() => nameField.activeFocus)

        runtimeMock.capabilitiesReady = true
        runtimeMock.savedLayoutsReady = true
        runtimeMock.savedLayoutsChanged()
        panel.confirmOverwrite()
        compare(saveSpy.count, 0)
        tryVerify(() => list.itemAtIndex(0) !== null)
        loadButton = findChild(panel, "loadSavedLayout-layout-a")
        verify(loadButton !== null)
        tryVerify(() => loadButton.visible && loadButton.enabled)

        loadButton.forceActiveFocus()
        tryVerify(() => loadButton.activeFocus)
        mouseClick(loadButton)
        const loadDialog = findChild(
            hostWindow.contentItem, "savedLayoutReplaceDraftDialog")
        verify(loadDialog !== null)
        tryVerify(() => loadDialog.opened)

        runtimeMock.capabilitiesReady = false
        runtimeMock.savedLayoutsReady = false
        panel.actionsAllowed = false
        runtimeMock.savedLayoutsChanged()
        tryVerify(() => !loadDialog.opened)
        tryVerify(() => !loadDialog.visible)
        compare(panel.pendingLayoutId, "")
        compare(loadSpy.count, 0)
        tryVerify(() => nameField.activeFocus)

        runtimeMock.capabilitiesReady = true
        runtimeMock.savedLayoutsReady = true
        panel.actionsAllowed = true
        runtimeMock.savedLayoutsChanged()
        panel.confirmLoad()
        compare(loadSpy.count, 0)
        tryVerify(() => list.itemAtIndex(0) !== null)
        deleteButton = findChild(panel, "deleteSavedLayout-layout-a")
        verify(deleteButton !== null)
        tryVerify(() => deleteButton.visible && deleteButton.enabled)

        deleteButton.forceActiveFocus()
        tryVerify(() => deleteButton.activeFocus)
        panel.requestDelete(deleteButton, "layout-a")
        const deleteDialog = findChild(
            hostWindow.contentItem, "savedLayoutDeleteDialog")
        verify(deleteDialog !== null)
        tryVerify(() => deleteDialog.opened)

        runtimeMock.capabilitiesReady = false
        runtimeMock.savedLayoutsReady = false
        panel.actionsAllowed = false
        runtimeMock.savedLayoutsChanged()
        tryVerify(() => !deleteDialog.opened)
        compare(panel.pendingLayoutId, "")
        compare(deleteSpy.count, 0)
        tryVerify(() => nameField.activeFocus)

        runtimeMock.capabilitiesReady = true
        runtimeMock.savedLayoutsReady = true
        panel.actionsAllowed = true
        runtimeMock.savedLayoutsChanged()
        panel.confirmDelete()
        compare(deleteSpy.count, 0)
    }

    function test_deleteRequiresConfirmationAndNeverLoadsOrDeletesMedia() {
        addLayout("layout-a", "Desk", false)
        const panel = createPanel()
        const deleteSpy = spy(panel, "deleteRequested")
        const loadSpy = spy(panel, "loadRequested")
        const list = findChild(panel, "savedLayoutsList")
        tryVerify(() => list.itemAtIndex(0) !== null)
        const deleteButton = findChild(
            panel, "deleteSavedLayout-layout-a")
        verify(deleteButton !== null)

        deleteButton.forceActiveFocus()
        tryVerify(() => deleteButton.activeFocus)
        mouseClick(deleteButton)
        const dialog = findChild(
            hostWindow.contentItem, "savedLayoutDeleteDialog")
        verify(dialog !== null)
        tryVerify(() => dialog.opened)
        compare(deleteSpy.count, 0)

        keyClick(Qt.Key_Escape)
        tryVerify(() => !dialog.opened)
        tryVerify(() => deleteButton.activeFocus)
        compare(deleteSpy.count, 0)

        mouseClick(deleteButton)
        tryVerify(() => dialog.opened)
        mouseClick(dialogButton(dialog, Dialog.Ok))
        tryVerify(() => !dialog.opened)
        tryVerify(() => deleteButton.activeFocus)
        compare(deleteSpy.count, 1)
        compare(deleteSpy.signalArguments[0][0], "layout-a")
        compare(loadSpy.count, 0)
    }

    function test_narrowActionsReflowWithoutOverflowAndRemainAccessible() {
        addLayout(
            "layout-a",
            "A very long saved layout name that must not overflow the panel",
            true)
        const panel = createPanel({"width": 320, "height": 620})
        const nameField = findChild(panel, "savedLayoutNameField")
        const saveButton = findChild(panel, "saveCurrentLayoutButton")
        const list = findChild(panel, "savedLayoutsList")
        verify(nameField !== null)
        verify(saveButton !== null)
        verify(list !== null)
        tryVerify(() => list.itemAtIndex(0) !== null)
        const row = list.itemAtIndex(0)
        const loadButton = findChild(panel, "loadSavedLayout-layout-a")
        const deleteButton = findChild(
            panel, "deleteSavedLayout-layout-a")
        verify(loadButton !== null)
        verify(deleteButton !== null)

        verify(saveButton.y > nameField.y)
        verify(deleteButton.y > loadButton.y)
        verify(nameField.width <= panel.availableWidth)
        verify(saveButton.width <= panel.availableWidth)
        verify(row.width <= list.width)
        verify(loadButton.width <= row.width)
        verify(deleteButton.width <= row.width)

        const controls = [nameField, saveButton,
                          loadButton, deleteButton]
        for (const control of controls) {
            verify(control.activeFocusOnTab)
            verify(control.Accessible.name.length > 0)
        }
        verify(saveButton.height >= 44)
        verify(loadButton.height >= 44)
        verify(deleteButton.height >= 44)
        compare(nameField.Accessible.name, "Saved layout name")
    }

    function test_savedLayoutsRussianTranslationAndNarrowAccessibility() {
        const panel = createPanel({"width": 320, "height": 620})
        const status = findChild(panel, "savedLayoutsStatusText")
        const nameField = findChild(panel, "savedLayoutNameField")
        const saveButton = findChild(panel, "saveCurrentLayoutButton")
        verify(status !== null)
        verify(nameField !== null)
        verify(saveButton !== null)
        if (panel.title === "Saved layouts")
            skip("Russian translation catalog is not loaded")

        compare(panel.title, "Сохранённые раскладки")
        compare(status.text,
                "Для этого дисплея нет сохранённых раскладок.")
        compare(status.Accessible.name, status.text)
        compare(nameField.placeholderText, "Имя раскладки")
        compare(nameField.Accessible.name,
                "Имя сохранённой раскладки")
        compare(saveButton.text, "Сохранить текущую")
        compare(saveButton.Accessible.name, saveButton.text)

        addLayout("layout-a", "Desk", false)
        wait(0)
        const list = findChild(panel, "savedLayoutsList")
        verify(list !== null)
        tryVerify(() => list.itemAtIndex(0) !== null)
        const loadButton = findChild(
            panel, "loadSavedLayout-layout-a")
        const deleteButton = findChild(
            panel, "deleteSavedLayout-layout-a")
        verify(loadButton !== null)
        verify(deleteButton !== null)
        compare(loadButton.text, "Загрузить в черновик")
        compare(deleteButton.text, "Удалить")
        compare(loadButton.Accessible.name, loadButton.text)
        compare(deleteButton.Accessible.name, deleteButton.text)
        tryVerify(() => deleteButton.y > loadButton.y)
        verify(loadButton.width <= list.width)
        verify(deleteButton.width <= list.width)

        nameField.text = "desk"
        mouseClick(saveButton)
        const dialog = findChild(
            hostWindow.contentItem, "savedLayoutOverwriteDialog")
        const prompt = findChild(
            hostWindow.contentItem, "savedLayoutOverwritePrompt")
        verify(dialog !== null)
        verify(prompt !== null)
        tryVerify(() => dialog.opened)
        compare(dialog.title,
                "Перезаписать сохранённую раскладку?")
        compare(
            prompt.text,
            "Раскладка с таким именем уже существует. Заменить её сохранённое состояние?")
        compare(prompt.Accessible.name, prompt.text)
        keyClick(Qt.Key_Escape)
        tryVerify(() => !dialog.opened)
    }
}
