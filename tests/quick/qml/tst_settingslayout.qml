pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import "../../../qml/pages" as Pages

TestCase {
    id: testCase
    name: "SettingsLayout"
    when: windowShown
    width: 1000
    height: 760

    QtObject {
        id: runtimeMock

        property bool operationBusy: false
        property bool serviceAvailable: true
        property bool printerClassDevicePresent: true
        property bool displaySessionActive: true
        property string diagnostic: ""

        function refreshAll() {}
    }

    QtObject {
        id: settingsMock

        property string language: "en"
        property bool autostartEnabled: true
        property bool autostartAvailable: true
        property bool busy: false
        property string errorMessage: ""

        function setLanguage(code) {
            language = code
        }
        function setAutostartEnabled(enabled) {
            autostartEnabled = enabled
        }
        function refreshAutostart() {}
    }

    Component {
        id: settingsComponent

        Pages.SettingsPage {
            runtime: runtimeMock
            settings: settingsMock
        }
    }

    function test_englishAndAutostartControlsAreFunctional() {
        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 950, "height": 720})
        verify(page !== null)
        wait(0)

        const language =
            findChild(page, "languageCombo")
        const autostart =
            findChild(page, "autostartSwitch")
        const content =
            findChild(page, "settingsContent")
        const openGitHub =
            findChild(page, "openGitHubButton")
        verify(language !== null)
        verify(autostart !== null)
        verify(content !== null)
        verify(openGitHub !== null)
        compare(language.currentText, "English")
        verify(autostart.checked)

        const githubPosition =
            openGitHub.mapToItem(content, 0, 0)
        verify(content.width - githubPosition.x
               - openGitHub.width <= 30)

        settingsMock.language = "ru"
        wait(0)
        compare(language.currentText, "Russian")
    }
}
