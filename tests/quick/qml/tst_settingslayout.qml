pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import "../../../qml/components" as Components
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
        property bool legacyConnected: false
        property bool printerClassDevicePresent: true
        property bool displaySessionActive: true
        property string diagnostic: ""

        function refreshAll() {}
        function connectDevice(port) {}
        function disconnectDevice() {}
        function startKeepalive(interval) {}
    }

    QtObject {
        id: settingsMock

        property string language: "en"
        property string devicePort: ""
        property int keepaliveInterval: 10
        property var serialPorts: ["/dev/ttyACM0"]
        property bool autostartEnabled: true
        property bool autostartAvailable: true
        property bool busy: false
        property string errorMessage: ""

        function setLanguage(code) {
            language = code
        }
        function setDevicePort(port) {
            devicePort = port
        }
        function setKeepaliveInterval(interval) {
            keepaliveInterval = interval
        }
        function refreshSerialPorts() {}
        function setAutostartEnabled(enabled) {
            autostartEnabled = enabled
        }
        function refreshAutostart() {}
    }

    QtObject {
        id: firmwareMock

        property bool serviceAvailable: true
        property bool compatible: true
        property bool ready: true
        property url homeFolder: Qt.resolvedUrl(".")
        property string packagePath: ""
        property bool busy: false
        property bool validationBusy: false
        property bool flashBusy: false
        property bool approvalAvailable: false
        property bool flashSupported: false
        property bool recoveryRequired: false
        property bool canValidate: packagePath.length > 0
        property bool canFlash: false
        property bool confirmationRequired: false
        property int progress: 0
        property string phase: "Idle"
        property string status: "Ready"
        property string kind: ""
        property string canonicalPath: ""
        property string sha256: ""
        property real sizeBytes: 0
        property string productCode: ""
        property string firmwareVersion: ""
        property string appVersion: ""
        property string errorMessage: ""
        property int recoveryAcknowledgementCount: 0

        function setPackagePath(path) {
            packagePath = path
        }
        function validatePackage() {}
        function requestFlashConfirmation() {}
        function cancelFlashConfirmation() {}
        function confirmFlash() {}
        function requestCancel() {}
        function acknowledgeFirmwareRecovery() {
            ++recoveryAcknowledgementCount
        }
        function refresh() {}
    }

    Component {
        id: settingsComponent

        Pages.SettingsPage {
            runtime: runtimeMock
            settings: settingsMock
            firmware: firmwareMock
        }
    }

    Component {
        id: firmwarePanelComponent

        Components.FirmwarePanel {
            controller: firmwareMock
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
        const firmwarePanel =
            findChild(page, "firmwarePanel")
        const firmwareChoose =
            findChild(page, "firmwareChooseButton")
        const firmwarePath =
            findChild(page, "firmwarePackagePath")
        const firmwareRecovery =
            findChild(page,
                      "firmwareRecoveryAcknowledgeButton")
        const serialPort =
            findChild(page, "serialPortCombo")
        const keepalive =
            findChild(page, "keepaliveSpin")
        const reconnect =
            findChild(page, "reconnectDeviceButton")
        verify(language !== null)
        verify(autostart !== null)
        verify(content !== null)
        verify(openGitHub !== null)
        verify(firmwarePanel !== null)
        verify(firmwareChoose !== null)
        verify(firmwarePath !== null)
        verify(firmwareRecovery !== null)
        verify(serialPort !== null)
        verify(keepalive !== null)
        verify(reconnect !== null)
        compare(serialPort.currentText, "Auto")
        compare(keepalive.value, 10)
        compare(language.currentText, "English")
        verify(autostart.checked)
        compare(firmwareRecovery.text,
                "I inspected the display; resume connection")
        verify(!firmwareRecovery.visible)
        verify(!firmwareRecovery.enabled)

        const githubPosition =
            openGitHub.mapToItem(content, 0, 0)
        verify(content.width - githubPosition.x
               - openGitHub.width <= 30)

        settingsMock.language = "ru"
        wait(0)
        compare(language.currentText, "Russian")
    }

    function test_firmwareRecoveryAcknowledgementVisibility() {
        firmwareMock.busy = false
        firmwareMock.recoveryRequired = false
        firmwareMock.recoveryAcknowledgementCount = 0

        const panel = createTemporaryObject(
            firmwarePanelComponent, testCase,
            {"width": 950, "height": 600,
             "visible": true})
        verify(panel !== null)
        wait(0)

        const recoveryButton = findChild(
            panel, "firmwareRecoveryAcknowledgeButton")
        verify(recoveryButton !== null)
        compare(
            recoveryButton.text,
            "I inspected the display; resume connection")
        verify(!recoveryButton.visible)
        verify(!recoveryButton.enabled)
        verify(!panel.recoveryActionAvailable)

        firmwareMock.recoveryRequired = true
        wait(0)
        verify(panel.recoveryActionAvailable)
        verify(recoveryButton.enabled)
        recoveryButton.clicked()
        compare(
            firmwareMock.recoveryAcknowledgementCount, 1)

        firmwareMock.busy = true
        wait(0)
        verify(!panel.recoveryActionAvailable)
        verify(!recoveryButton.visible)
        verify(!recoveryButton.enabled)
    }
}
