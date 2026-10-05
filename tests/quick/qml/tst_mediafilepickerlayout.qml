pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase
    name: "MediaFilePickerLayout"
    when: host.visible

    ApplicationWindow {
        id: host
        width: 1060
        height: 700
        visible: true

        QtObject {
            id: controller

            property url homeFolder: Qt.resolvedUrl(".")
            property url lastOpened: ""
            property bool pickerWasOpenWhenBegin: false

            function begin(source) {
                lastOpened = source
                pickerWasOpenWhenBegin = picker.opened
            }
        }

        Components.MediaFilePicker {
            id: picker
            editor: controller
        }
        QtObject {
            id: chooser
            property bool busy: false
            property int opens: 0
            signal selected(url source)
            signal cancelled()
            signal failed(string message)
            function open(title, folder) { ++opens; busy = true }
            function cancel() { busy = false; cancelled() }
            function accept(source) {
                if (busy) { busy = false; selected(source) }
            }
        }
    }

    function init() {
        chooser.cancel()
        chooser.opens = 0
        picker.portalChooser = null
        controller.lastOpened = ""
        controller.pickerWasOpenWhenBegin = false
        picker.close()
        wait(0)
    }

    function test_portalIsModalAndCancelRejectsLateSelection() {
        picker.portalChooser = chooser
        picker.openPicker()
        compare(chooser.opens, 1)
        const guard = findChild(picker, "portalChooserGuard")
        verify(guard !== null)
        tryVerify(() => guard.opened)
        verify(guard.modal)
        verify(!picker.opened)
        guard.reject()
        tryVerify(() => !chooser.busy)
        tryVerify(() => !guard.opened)
        controller.begin(Qt.resolvedUrl("newer.png"))
        chooser.accept(Qt.resolvedUrl("old.png"))
        wait(0)
        compare(controller.lastOpened, Qt.resolvedUrl("newer.png"))
    }

    function test_portalSelectionStartsExistingEditorFlow() {
        picker.portalChooser = chooser
        picker.openPicker()
        chooser.accept(Qt.resolvedUrl("portal.png"))
        compare(controller.lastOpened, "")
        tryCompare(controller, "lastOpened", Qt.resolvedUrl("portal.png"))
        verify(!controller.pickerWasOpenWhenBegin)
    }

    function test_pickerHasVisibleBoundedContent() {
        picker.openPicker()
        tryVerify(() => picker.opened)

        const header =
            findChild(picker, "mediaFilePickerHeader")
        const list =
            findChild(picker, "mediaFilePickerList")
        const openButton =
            findChild(picker, "mediaFilePickerOpenButton")

        verify(header !== null)
        verify(list !== null)
        verify(openButton !== null)
        verify(header.visible)
        verify(list.visible)
        verify(picker.width <= host.width - 48)
        verify(picker.height <= host.height - 48)
        verify(!openButton.enabled)
    }

    function test_selectedFileOpensEditor() {
        picker.openPicker()
        tryVerify(() => picker.opened)

        const source = Qt.resolvedUrl("fixture.mp4")
        picker.selectedFile = source
        wait(0)

        const openButton =
            findChild(picker, "mediaFilePickerOpenButton")
        verify(openButton.enabled)

        picker.acceptSelection()
        tryVerify(() => !picker.opened)
        tryVerify(() => controller.lastOpened === source)
        verify(!controller.pickerWasOpenWhenBegin)
    }

    function test_navigationClearsStaleSelection() {
        picker.openPicker()
        tryVerify(() => picker.opened)

        picker.selectedFile = Qt.resolvedUrl("fixture.mp4")
        verify(String(picker.selectedFile).length > 0)

        picker.navigate(Qt.resolvedUrl(".."))
        compare(String(picker.selectedFile), "")
    }
}
