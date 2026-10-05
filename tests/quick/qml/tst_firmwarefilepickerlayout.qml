pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase

    name: "FirmwareFilePickerLayout"
    when: windowShown
    width: 900
    height: 700

    QtObject {
        id: controllerMock

        property url homeFolder: Qt.resolvedUrl(".")
        property string packagePath: ""

        function setPackagePath(path) {
            packagePath = path
        }
    }

    Component {
        id: pickerComponent

        Components.FirmwareFilePicker {
            controller: controllerMock
        }
    }

    function test_pickerIsBoundedAndReturnsExplicitSelection() {
        const picker = createTemporaryObject(
            pickerComponent, testCase)
        verify(picker !== null)

        picker.openPicker()
        tryVerify(() => picker.opened)
        verify(picker.width <= testCase.width)
        verify(picker.height <= testCase.height)

        const selected = Qt.resolvedUrl("firmware-test.zip")
        picker.selectedFile = selected
        picker.selectedName = "firmware-test.zip"
        picker.acceptSelection()

        tryVerify(() => !picker.opened)
        compare(
            controllerMock.packagePath,
            String(selected))
    }
}
