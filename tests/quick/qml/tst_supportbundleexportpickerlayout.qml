pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase
    name: "SupportBundleExportPickerLayout"
    when: host.visible

    ApplicationWindow {
        id: host
        width: 900
        height: 700
        visible: true

        Button {
            id: launcher
            text: "Open"
        }

        QtObject {
            id: controller
            property bool busy: false
            property int exportCount: 0
            property url exportedFolder
            function exportToFolder(folder) {
                ++exportCount
                exportedFolder = folder
            }
        }

        Components.SupportBundleExportPicker {
            id: picker
            controller: controller
            homeFolder: Qt.resolvedUrl("..")
            focusReturnItem: launcher
        }
    }

    function init() {
        host.requestActivate()
        tryVerify(() => host.active)
        controller.busy = false
        controller.exportCount = 0
        controller.exportedFolder = ""
        picker.close()
        picker.currentFolder = picker.homeFolder
        wait(0)
    }

    function test_folderOnlyExportAndAccessibility() {
        launcher.forceActiveFocus()
        tryVerify(() => launcher.activeFocus)
        picker.openForExport()
        tryVerify(() => picker.opened)

        const path = findChild(picker, "supportBundleFolderPath")
        const list = findChild(picker, "supportBundleFolderList")
        const closeButton = findChild(
            picker, "supportBundleFolderCloseButton")
        const saveButton = findChild(
            picker, "supportBundleFolderSaveButton")
        const dialogContent = findChild(
            picker, "supportBundleDialogContent")
        verify(path !== null)
        verify(list !== null)
        verify(closeButton !== null)
        verify(saveButton !== null)
        verify(dialogContent !== null)
        verify(findChild(picker, "supportBundleFileName") === null)
        verify(String(path.text).length > 0)
        tryVerify(() => saveButton.enabled)
        verify(saveButton.activeFocusOnTab)
        verify(saveButton.Accessible.name.length > 0)
        verify(saveButton.Accessible.description.length > 0)
        compare(dialogContent.Accessible.role, Accessible.Dialog)

        tryVerify(() => list.count > 0 && list.itemAtIndex(0) !== null)
        const initialFolder = String(picker.currentFolder)
        const folderDelegate = list.itemAtIndex(0)
        folderDelegate.forceActiveFocus()
        tryVerify(() => folderDelegate.activeFocus)
        keyClick(Qt.Key_Return)
        tryVerify(() => String(picker.currentFolder) !== initialFolder)
        tryVerify(() => saveButton.enabled)

        saveButton.forceActiveFocus()
        tryVerify(() => saveButton.activeFocus)
        keyClick(Qt.Key_Space)
        tryCompare(controller, "exportCount", 1)
        compare(String(controller.exportedFolder),
                String(picker.currentFolder))
        tryVerify(() => launcher.activeFocus)
    }

    function test_busyDisablesSaveAndEscapeRestoresFocus() {
        launcher.forceActiveFocus()
        tryVerify(() => launcher.activeFocus)
        picker.openForExport()
        tryVerify(() => picker.opened)
        const saveButton = findChild(
            picker, "supportBundleFolderSaveButton")
        verify(saveButton !== null)

        picker.navigate(Qt.resolvedUrl("missing-support-folder"))
        tryVerify(() => !saveButton.enabled)
        saveButton.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(controller.exportCount, 0)

        picker.navigate(picker.homeFolder)
        tryVerify(() => saveButton.enabled)
        controller.busy = true
        tryVerify(() => !saveButton.enabled)
        verify(!saveButton.enabled)
        keyClick(Qt.Key_Escape)
        tryVerify(() => !picker.opened)
        tryVerify(() => launcher.activeFocus)
        compare(controller.exportCount, 0)
    }
}
