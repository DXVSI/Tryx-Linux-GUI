pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase
    name: "MediaExportPickerLayout"
    when: host.visible

    ApplicationWindow {
        id: host
        width: 900
        height: 700
        visible: true

        QtObject {
            id: workflowMock

            property string exportedMediaId: ""
            property string exportedMediaName: ""
            property string exportedFileName: ""

            function suggestedExportFileName(mediaName) {
                return "suggested-device-copy.h264"
            }

            function beginExport(mediaId, mediaName,
                                 folder, fileName) {
                exportedMediaId = mediaId
                exportedMediaName = mediaName
                exportedFileName = fileName
            }
        }

        Components.MediaExportPicker {
            id: picker
            workflow: workflowMock
            homeFolder: Qt.resolvedUrl(".")
        }
        QtObject {
            id: chooser
            property bool busy: false
            signal selected(url folder)
            signal cancelled()
            signal failed(string message)
            function open(title, folder) { busy = true }
            function cancel() { busy = false; cancelled() }
        }
    }

    function init() {
        chooser.cancel()
        picker.portalChooser = null
        workflowMock.exportedMediaId = ""
        workflowMock.exportedMediaName = ""
        workflowMock.exportedFileName = ""
        picker.close()
        wait(0)
    }

    function test_portalFolderPreservesExportIdentityAndNeedsConfirmation() {
        picker.portalChooser = chooser
        picker.openFor("original-id", "original-media")
        picker.openFor("newer-id", "newer-media")
        compare(picker.mediaId, "original-id")
        verify(!picker.opened)
        chooser.busy = false
        chooser.selected(Qt.resolvedUrl("."))
        tryVerify(() => picker.opened)
        compare(workflowMock.exportedMediaId, "")
        picker.exportCopy()
        tryCompare(workflowMock, "exportedMediaId", "original-id")
        compare(workflowMock.exportedMediaName, "original-media")
    }

    function test_suggestedNameAndExplicitExport() {
        picker.openFor("media-id", "device-media")
        tryVerify(() => picker.opened)

        const fileName =
            findChild(picker, "mediaExportFileName")
        const exportButton =
            findChild(picker, "mediaExportSaveButton")
        verify(fileName !== null)
        verify(exportButton !== null)
        compare(fileName.text, "suggested-device-copy.h264")
        verify(exportButton.enabled)

        mouseClick(exportButton)
        tryCompare(workflowMock, "exportedMediaId", "media-id")
        compare(workflowMock.exportedMediaName, "device-media")
        compare(workflowMock.exportedFileName,
                "suggested-device-copy.h264")
    }
}
