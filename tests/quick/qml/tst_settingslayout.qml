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
    visible: true

    QtObject {
        id: runtimeMock

        property bool operationBusy: false
        property bool serviceAvailable: true
        property bool legacyConnected: false
        property bool printerClassDevicePresent: true
        property bool displaySessionActive: true
        property string deviceModel: "PANORAMA SE"
        property string productId: "391a:1021"
        property string firmwareVersion: "PASE-FW-1"
        property string deviceAppVersion: "PASE-APP-2"
        property bool deviceSpecificationsSupported: true
        property string deviceSpecificationsStatus: "Ready"
        property bool deviceSpecificationsReady: true
        property string deviceReportedProductName: "PASE"
        property int deviceVideoOutputWidth: 2240
        property int deviceVideoOutputHeight: 1080
        property string deviceScreenType: "OLED"
        property bool deviceUsbAutoKeepalive: false
        property int mediaTargetWidth: 2240
        property int mediaTargetHeight: 1080
        property string diagnostic: ""
        property bool presentationPreferencesReady: false
        property bool presentationPreferencesBusy: false
        property string temperatureUnit: "Celsius"
        property string timeFormat: "24H"
        property int presentationPreferencesSetCount: 0
        property string lastTemperatureUnit: ""
        property string lastTimeFormat: ""

        function refreshAll() {}
        function connectDevice(port) {}
        function disconnectDevice() {}
        function startKeepalive(interval) {}
        function setPresentationPreferences(temperature, time) {
            ++presentationPreferencesSetCount
            lastTemperatureUnit = temperature
            lastTimeFormat = time
        }
    }

    QtObject {
        id: settingsMock

        property string language: "en"
        property bool hideToTrayOnClose: true
        property string devicePort: ""
        property int keepaliveInterval: 10
        property var serialPorts: ["/dev/ttyACM0"]
        property bool autostartEnabled: true
        property bool autostartAvailable: true
        property bool busy: false
        property string errorMessage: ""
        property int autostartSetCount: 0
        property bool lastAutostartValue: false

        function setLanguage(code) {
            language = code
        }
        function setHideToTrayOnClose(enabled) {
            hideToTrayOnClose = enabled
        }
        function setDevicePort(port) {
            devicePort = port
        }
        function setKeepaliveInterval(interval) {
            keepaliveInterval = interval
        }
        function refreshSerialPorts() {}
        function setAutostartEnabled(enabled) {
            ++autostartSetCount
            lastAutostartValue = enabled
            autostartEnabled = enabled
        }
        function refreshAutostart() {}
    }

    QtObject {
        id: windowChromeMock

        property bool trayAvailable: true
    }

    QtObject {
        id: supportBundleMock

        property bool busy: false
        property bool runtimeDetailsAvailable: true
        property string state: "idle"
        property string message: ""
        property string lastExportPath: ""
        property url homeFolder: Qt.resolvedUrl(".")
        property int exportCount: 0
        property url exportedFolder

        function exportToFolder(folder) {
            ++exportCount
            exportedFolder = folder
        }
    }

    QtObject {
        id: cacheManagementMock

        signal eligibilityChanged()

        property string state: "Ready"
        property string phase: "Idle"
        property string message: "Ready to remove unused temporary files."
        property bool canStart: true
        property bool canCancel: false
        property bool canRefresh: false
        property bool requiresAcknowledgment: false
        property real totalFiles: 0
        property real removedFiles: 0
        property real removedBytes: 0
        property real runtimeRemovedFiles: 0
        property real runtimeRemovedBytes: 0
        property real localRemovedFiles: 0
        property real localRemovedBytes: 0
        property real eligibilityRevision: 0
        property int startCount: 0
        property int cancelCount: 0
        property int refreshCount: 0
        property int acknowledgmentCount: 0

        function startCleanup(expectedEligibilityRevision) {
            if (expectedEligibilityRevision !== eligibilityRevision)
                return false
            ++startCount
            phase = "Runtime"
            state = "Running"
            canStart = false
            canCancel = true
            message = "Removing unused temporary files..."
            return true
        }
        function cancelCleanup() {
            ++cancelCount
            return true
        }
        function refresh() {
            ++refreshCount
            return true
        }
        function acknowledgeUnresolved() {
            ++acknowledgmentCount
            state = "Ready"
            canStart = true
            canRefresh = false
            requiresAcknowledgment = false
        }
        function publishStateChange() {
            stateChanged()
        }
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
            windowChrome: windowChromeMock
            supportBundle: supportBundleMock
            cacheManagement: cacheManagementMock
        }
    }

    Component {
        id: firmwarePanelComponent

        Components.FirmwarePanel {
            controller: firmwareMock
        }
    }

    Component {
        id: releaseOpenSpyComponent
        SignalSpy { signalName: "openReleaseRequested" }
    }

    function test_releaseUpdateIsConditionalAndKeyboardAccessible() {
        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720, "visible": true})
        verify(page !== null)
        const row = findChild(page, "releaseUpdateRow")
        const label = findChild(page, "availableReleaseLabel")
        const button = findChild(page, "openReleaseButton")
        verify(row !== null)
        verify(label !== null)
        verify(button !== null)
        verify(!row.visible)
        verify(!button.enabled)
        const opened = createTemporaryObject(releaseOpenSpyComponent, testCase,
                                             {"target": page})
        verify(opened !== null)
        page.availableVersion = "2.10.0"
        page.releaseUrl = "https://github.com/DXVSI/Tryx-Linux-GUI/releases/tag/v2.10.0"
        page.updateAvailable = true
        wait(0)
        verify(row.visible)
        compare(label.text, "Version 2.10.0 is available")
        compare(button.text, "Open release")
        compare(button.Accessible.name, button.text)
        compare(button.Accessible.description, label.text)
        compare(opened.count, 0) // A confirmed release never opens a browser itself.
        const position = button.mapToItem(page, 0, 0)
        page.contentItem.contentY += position.y - 200
        button.forceActiveFocus()
        tryVerify(function() { return button.activeFocus })
        keyClick(Qt.Key_Space)
        compare(opened.count, 1)
        const content = findChild(page, "settingsContent")
        for (const item of [label, button]) {
            const location = item.mapToItem(content, 0, 0)
            verify(location.x >= 0)
            verify(location.x + item.width <= content.width + 1)
        }
        page.updateAvailable = false
        wait(0)
        verify(!row.visible)
        verify(!button.enabled)
        verify(findChild(page, "openGitHubButton").activeFocus)
    }

    function test_releaseUpdateRussian() {
        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720, "visible": true})
        verify(page !== null)
        const button = findChild(page, "openReleaseButton")
        verify(button !== null)
        if (button.text === "Open release") {
            skip("Russian catalog is checked in the separate translated baseline run")
            return
        }
        page.availableVersion = "2.10.0"
        page.releaseUrl = "https://github.com/DXVSI/Tryx-Linux-GUI/releases/tag/2.10.0"
        page.updateAvailable = true
        wait(0)
        compare(button.text, "Открыть релиз")
        const label = findChild(page, "availableReleaseLabel")
        compare(label.text, "Доступна версия 2.10.0")
        compare(button.Accessible.description, label.text)
    }

    function test_englishAndAutostartControlsAreFunctional() {
        runtimeMock.deviceModel = "PANORAMA SE"
        runtimeMock.productId = "391a:1021"
        runtimeMock.firmwareVersion = "PASE-FW-1"
        runtimeMock.deviceAppVersion = "PASE-APP-2"
        settingsMock.language = "en"
        settingsMock.autostartEnabled = true
        settingsMock.autostartAvailable = true
        settingsMock.busy = false
        settingsMock.autostartSetCount = 0

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 950, "height": 720})
        verify(page !== null)
        wait(0)

        const language =
            findChild(page, "languageCombo")
        const autostart =
            findChild(page, "autostartSwitch")
        const autostartTitle =
            findChild(page, "autostartTitle")
        const autostartDescription =
            findChild(page, "autostartDescription")
        const settingsError =
            findChild(page, "settingsErrorMessage")
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
        const deviceModel =
            findChild(page, "deviceModelValue")
        const productId =
            findChild(page, "deviceProductIdValue")
        const firmwareVersion =
            findChild(page, "deviceFirmwareVersionValue")
        const deviceAppVersion =
            findChild(page, "deviceAppVersionValue")
        verify(language !== null)
        verify(autostart !== null)
        verify(autostartTitle !== null)
        verify(autostartDescription !== null)
        verify(settingsError !== null)
        verify(content !== null)
        verify(openGitHub !== null)
        verify(firmwarePanel !== null)
        verify(firmwareChoose !== null)
        verify(firmwarePath !== null)
        verify(firmwareRecovery !== null)
        verify(serialPort !== null)
        verify(keepalive !== null)
        verify(reconnect !== null)
        verify(deviceModel !== null)
        verify(productId !== null)
        verify(firmwareVersion !== null)
        verify(deviceAppVersion !== null)
        compare(serialPort.currentText, "Auto")
        compare(keepalive.value, 10)
        compare(language.currentText, "English")
        verify(autostart.checked)
        compare(
            autostartTitle.text,
            "Start TRYX Panorama Manager when you sign in")
        compare(
            autostartDescription.text,
            "Starts hidden only when Hide to tray is selected and a system tray is available. The background service is unchanged.")
        verify(!autostartTitle.text.includes("background service"))
        verify(!autostartTitle.text.includes("systemd"))
        compare(
            autostart.Accessible.name,
            "Start TRYX Panorama Manager when you sign in")
        compare(
            autostart.Accessible.description,
            autostartDescription.text)

        settingsMock.busy = true
        wait(0)
        compare(
            autostartDescription.text,
            "Updating GUI autostart…")
        compare(
            autostart.Accessible.description,
            autostartDescription.text)
        verify(!autostart.enabled)
        settingsMock.busy = false

        settingsMock.errorMessage = "Autostart update failed"
        wait(0)
        verify(settingsError.visible)
        compare(settingsError.Accessible.role, Accessible.AlertMessage)
        compare(
            settingsError.Accessible.name,
            "Autostart update failed")
        settingsMock.errorMessage = ""

        settingsMock.autostartEnabled = false
        wait(0)
        verify(!autostart.checked)
        const autostartPosition = autostart.mapToItem(page, 0, 0)
        page.contentItem.contentY = Math.max(
            0,
            page.contentItem.contentY + autostartPosition.y - 200)
        wait(0)
        mouseClick(
            autostart,
            Math.floor(autostart.width / 2),
            Math.floor(autostart.height / 2),
            Qt.LeftButton)
        wait(0)
        compare(settingsMock.autostartSetCount, 1)
        verify(settingsMock.lastAutostartValue)
        verify(settingsMock.autostartEnabled)

        settingsMock.autostartAvailable = false
        wait(0)
        verify(!autostart.enabled)
        compare(
            autostartDescription.text,
            "GUI autostart state is unavailable. The background service is unchanged.")
        settingsMock.autostartAvailable = true
        compare(firmwareRecovery.text,
                "I inspected the display; resume connection")
        verify(!firmwareRecovery.visible)
        verify(!firmwareRecovery.enabled)
        compare(deviceModel.text, "PANORAMA SE")
        compare(productId.text, "391a:1021")
        compare(firmwareVersion.text, "PASE-FW-1")
        compare(deviceAppVersion.text, "PASE-APP-2")

        runtimeMock.deviceModel = "PANORAMA"
        runtimeMock.productId = "391a:1011"
        runtimeMock.firmwareVersion = ""
        runtimeMock.deviceAppVersion = ""
        wait(0)
        compare(deviceModel.text, "PANORAMA")
        compare(productId.text, "391a:1011")
        compare(firmwareVersion.text, "Not reported")
        compare(deviceAppVersion.text, "Not reported")

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

    function test_closeBehaviorRequiresStatusNotifierHost() {
        settingsMock.hideToTrayOnClose = true
        windowChromeMock.trayAvailable = true

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720,
             "visible": true})
        verify(page !== null)
        wait(0)

        const hideToTray = findChild(page, "hideToTrayRadio")
        const quitGui = findChild(page, "quitGuiRadio")
        const unavailable = findChild(
            page, "trayUnavailableExplanation")
        verify(hideToTray !== null)
        verify(quitGui !== null)
        verify(unavailable !== null)
        verify(hideToTray.enabled)
        verify(hideToTray.checked)
        verify(!quitGui.checked)
        verify(!unavailable.visible)

        windowChromeMock.trayAvailable = false
        wait(0)
        verify(!hideToTray.enabled)
        verify(hideToTray.checked)
        verify(!quitGui.checked)
        verify(unavailable.visible)
        compare(
            unavailable.text,
            "The desktop system tray is unavailable. Closing the window will quit the GUI.")

        windowChromeMock.trayAvailable = true
        wait(0)
        verify(hideToTray.enabled)
        verify(hideToTray.checked)
        verify(!unavailable.visible)

        quitGui.clicked()
        wait(0)
        verify(!settingsMock.hideToTrayOnClose)
        verify(!hideToTray.checked)
        verify(quitGui.checked)
    }

    function test_presentationPreferencesUseConfirmedRuntimeState() {
        runtimeMock.presentationPreferencesReady = false
        runtimeMock.presentationPreferencesBusy = false
        runtimeMock.temperatureUnit = "Celsius"
        runtimeMock.timeFormat = "24H"
        runtimeMock.presentationPreferencesSetCount = 0
        runtimeMock.lastTemperatureUnit = ""
        runtimeMock.lastTimeFormat = ""

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720,
             "visible": true})
        verify(page !== null)
        wait(0)

        const content = findChild(page, "settingsContent")
        const language = findChild(page, "languageCombo")
        const temperature = findChild(
            page, "temperatureUnitCombo")
        const time = findChild(page, "timeFormatCombo")
        const closeBehavior = findChild(page, "hideToTrayRadio")
        const explanation = findChild(
            page, "presentationPreferencesExplanation")
        verify(content !== null)
        verify(language !== null)
        verify(temperature !== null)
        verify(time !== null)
        verify(closeBehavior !== null)
        verify(explanation !== null)
        verify(explanation.visible)
        verify(!temperature.enabled)
        verify(!time.enabled)
        compare(temperature.currentText, "Celsius (°C)")
        compare(time.currentText, "24-hour")
        compare(temperature.Accessible.name, "Temperature unit")
        compare(time.Accessible.name, "Time format")
        verify(temperature.Accessible.description.length > 0)
        verify(time.Accessible.description.length > 0)
        const languagePosition = language.mapToItem(content, 0, 0)
        const temperaturePosition = temperature.mapToItem(content, 0, 0)
        const timePosition = time.mapToItem(content, 0, 0)
        const closeBehaviorPosition = closeBehavior.mapToItem(
            content, 0, 0)
        verify(languagePosition.y < temperaturePosition.y)
        verify(temperaturePosition.y < timePosition.y)
        verify(timePosition.y < closeBehaviorPosition.y)

        runtimeMock.presentationPreferencesReady = true
        runtimeMock.temperatureUnit = "Fahrenheit"
        runtimeMock.timeFormat = "12H"
        wait(0)
        verify(temperature.enabled)
        verify(time.enabled)
        compare(temperature.currentText, "Fahrenheit (°F)")
        compare(time.currentText, "12-hour (AM/PM)")

        temperature.activated(0)
        wait(0)
        compare(runtimeMock.presentationPreferencesSetCount, 1)
        compare(runtimeMock.lastTemperatureUnit, "Celsius")
        compare(runtimeMock.lastTimeFormat, "12H")
        compare(temperature.currentText, "Fahrenheit (°F)")

        runtimeMock.presentationPreferencesBusy = true
        wait(0)
        verify(!temperature.enabled)
        verify(!time.enabled)
        verify(explanation.text.includes("Saving"))

        for (const control of [temperature, time]) {
            const position = control.mapToItem(content, 0, 0)
            verify(position.x >= 0)
            verify(position.x + control.width <= content.width + 1)
        }
    }

    function test_aboutShowsOnlyExistingLegalLinks() {
        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720,
             "visible": true})
        verify(page !== null)
        wait(0)

        const content = findChild(page, "settingsContent")
        const aboutActions = findChild(page, "aboutActions")
        const openGitHub = findChild(page, "openGitHubButton")
        const openLicense = findChild(page, "openLicenseButton")
        verify(content !== null)
        verify(aboutActions !== null)
        verify(openGitHub !== null)
        verify(openLicense !== null)
        compare(openLicense.text, "View License")
        compare(aboutActions.children.length, 2)
        const allowedDestinations = [
            "https://github.com/DXVSI/Tryx-Linux-GUI",
            "https://github.com/DXVSI/Tryx-Linux-GUI/blob/production/LICENSE"
        ]
        for (let index = 0;
             index < aboutActions.children.length; ++index) {
            compare(
                String(aboutActions.children[index].destinationUrl),
                allowedDestinations[index])
        }

        for (const button of [openGitHub, openLicense]) {
            const position = button.mapToItem(content, 0, 0)
            verify(position.x >= 0)
            verify(position.x + button.width <= content.width + 1)
        }
    }

    function test_supportBundleStatesRemainAccessible() {
        supportBundleMock.busy = false
        supportBundleMock.runtimeDetailsAvailable = true
        supportBundleMock.state = "idle"
        supportBundleMock.message = ""

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720,
             "visible": true})
        verify(page !== null)
        wait(0)

        const button = findChild(page, "supportBundleExportButton")
        const description = findChild(
            page, "supportBundleDescription")
        const result = findChild(page, "supportBundleResult")
        const picker = findChild(page, "supportBundleFolderPicker")
        verify(button !== null)
        verify(description !== null)
        verify(result !== null)
        verify(picker !== null)
        verify(button.enabled)
        verify(button.activeFocusOnTab)
        verify(button.Accessible.name.length > 0)
        compare(button.Accessible.description, description.text)
        verify(description.text.includes("runtime"))
        verify(!result.visible)

        supportBundleMock.runtimeDetailsAvailable = false
        wait(0)
        verify(button.enabled)
        verify(description.text.toLowerCase().includes("host"))

        supportBundleMock.busy = true
        supportBundleMock.state = "collecting"
        supportBundleMock.message = "Collecting support information…"
        wait(0)
        verify(!button.enabled)
        verify(description.text.includes("Collecting"))
        verify(result.visible)
        compare(result.Accessible.role, Accessible.StaticText)

        supportBundleMock.busy = false
        supportBundleMock.state = "saved"
        supportBundleMock.message =
            "Support report saved to /tmp/report.json"
        wait(0)
        verify(button.enabled)
        verify(result.visible)
        compare(result.text,
                "Support report saved to /tmp/report.json")
        compare(result.textFormat, Text.PlainText)
        compare(result.Accessible.role, Accessible.AlertMessage)

        supportBundleMock.state = "error"
        supportBundleMock.message = "Could not save the support report"
        wait(0)
        verify(result.visible)
        compare(result.Accessible.role, Accessible.AlertMessage)
    }

    function test_temporaryFilesPanelIsSafeResponsiveAndAccessible() {
        cacheManagementMock.state = "Ready"
        cacheManagementMock.phase = "Idle"
        cacheManagementMock.message =
            "Ready to remove unused temporary files."
        cacheManagementMock.canStart = true
        cacheManagementMock.canCancel = false
        cacheManagementMock.canRefresh = false
        cacheManagementMock.requiresAcknowledgment = false
        cacheManagementMock.totalFiles = 0
        cacheManagementMock.removedFiles = 0
        cacheManagementMock.removedBytes = 0
        cacheManagementMock.runtimeRemovedFiles = 0
        cacheManagementMock.runtimeRemovedBytes = 0
        cacheManagementMock.localRemovedFiles = 0
        cacheManagementMock.localRemovedBytes = 0
        cacheManagementMock.startCount = 0
        cacheManagementMock.cancelCount = 0
        cacheManagementMock.refreshCount = 0
        cacheManagementMock.acknowledgmentCount = 0

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720, "visible": true})
        verify(page !== null)
        wait(0)

        const panel = findChild(page, "temporaryFilesPanel")
        const title = findChild(page, "temporaryFilesTitle")
        const description = findChild(
            page, "temporaryFilesDescription")
        const status = findChild(page, "temporaryFilesStatus")
        const start = findChild(page, "temporaryFilesStartButton")
        const cancel = findChild(page, "temporaryFilesCancelButton")
        const refresh = findChild(page, "temporaryFilesRefreshButton")
        const acknowledge = findChild(
            page, "temporaryFilesAcknowledgeButton")
        const device = findChild(page, "devicePanel")
        const support = findChild(page, "supportBundlePanel")
        verify(panel !== null)
        verify(title !== null)
        verify(description !== null)
        verify(status !== null)
        verify(start !== null)
        verify(cancel !== null)
        verify(refresh !== null)
        verify(acknowledge !== null)
        verify(device !== null)
        verify(support !== null)
        compare(title.text, "Temporary files")
        compare(title.Accessible.role, Accessible.Heading)
        verify(description.text.includes("inactive previews"))
        verify(description.text.includes("Visible thumbnails"))
        verify(!description.text.toLowerCase().includes("path"))
        verify(panel.y > device.y)
        verify(panel.y < support.y)
        verify(start.enabled)
        verify(start.activeFocusOnTab)
        verify(start.height >= 44)
        verify(start.Accessible.name.length > 0)
        verify(start.Accessible.description.length > 0)
        compare(status.Accessible.role, Accessible.StatusBar)

        const startPosition = start.mapToItem(page.contentItem, 0, 0)
        page.contentItem.contentY = Math.max(
            0, startPosition.y - 180)
        wait(0)
        mouseClick(start, start.width / 2, start.height / 2)
        const dialog = findChild(page, "temporaryFilesConfirmation")
        const dialogCancel = findChild(
            page, "temporaryFilesDialogCancelButton")
        const dialogAccept = findChild(
            page, "temporaryFilesDialogAcceptButton")
        verify(dialog !== null)
        verify(dialogCancel !== null)
        verify(dialogAccept !== null)
        tryVerify(() => dialog.opened)
        compare(cacheManagementMock.startCount, 0)
        tryVerify(() => dialogCancel.activeFocus)
        keyClick(Qt.Key_Escape)
        tryVerify(() => !dialog.opened)
        tryVerify(() => start.activeFocus)

        mouseClick(start, start.width / 2, start.height / 2)
        tryVerify(() => dialog.opened)
        ++cacheManagementMock.eligibilityRevision
        mouseClick(
            dialogAccept,
            dialogAccept.width / 2,
            dialogAccept.height / 2)
        compare(cacheManagementMock.startCount, 0)
        tryVerify(() => !dialog.opened)
        tryVerify(() => !dialog.visible)
        tryVerify(() => status.activeFocus)

        start.forceActiveFocus()
        mouseClick(start, start.width / 2, start.height / 2)
        tryVerify(() => dialog.opened)
        ++cacheManagementMock.eligibilityRevision
        cacheManagementMock.eligibilityChanged()
        tryVerify(() => !dialog.opened)
        compare(cacheManagementMock.startCount, 0)
        tryVerify(() => start.activeFocus)

        mouseClick(start, start.width / 2, start.height / 2)
        tryVerify(() => dialog.opened)
        mouseClick(
            dialogAccept,
            dialogAccept.width / 2,
            dialogAccept.height / 2)
        compare(cacheManagementMock.startCount, 1)
        tryVerify(() => !dialog.opened)
        tryVerify(() => !dialog.visible)
        verify(cancel.visible)
        verify(cancel.enabled)
        verify(cancel.height >= 44)
        tryVerify(() => cancel.activeFocus)
        status.forceActiveFocus()
        tryVerify(() => status.activeFocus)
        cacheManagementMock.publishStateChange()
        wait(0)
        verify(status.activeFocus)
        cancel.forceActiveFocus()
        mouseClick(cancel, cancel.width / 2, cancel.height / 2)
        compare(cacheManagementMock.cancelCount, 1)

        cacheManagementMock.state = "Ready"
        cacheManagementMock.phase = "Quick"
        cacheManagementMock.canStart = false
        cacheManagementMock.canCancel = false
        cacheManagementMock.state = "Running"
        wait(0)
        verify(!cancel.visible)
        tryVerify(() => status.activeFocus)

        cacheManagementMock.state = "Succeeded"
        cacheManagementMock.phase = "Idle"
        cacheManagementMock.canStart = true
        cacheManagementMock.canCancel = false
        cacheManagementMock.message = "Unused temporary files were removed."
        cacheManagementMock.totalFiles = 3
        cacheManagementMock.removedFiles = 3
        cacheManagementMock.removedBytes = 4096
        cacheManagementMock.runtimeRemovedFiles = 2
        cacheManagementMock.runtimeRemovedBytes = 3000
        cacheManagementMock.localRemovedFiles = 1
        cacheManagementMock.localRemovedBytes = 1096
        wait(0)
        verify(status.text.includes("4096"))
        verify(status.text.includes("Removed files: 3"))
        verify(status.text.includes("Runtime: 2 files, 3000 bytes"))
        verify(status.text.includes("Preview: 1 files, 1096 bytes"))
        verify(!status.text.includes("Freed"))
        compare(status.Accessible.role, Accessible.StatusBar)

        cacheManagementMock.state = "Partial"
        cacheManagementMock.message = "Cleanup completed only partially."
        wait(0)
        compare(status.Accessible.role, Accessible.AlertMessage)

        cacheManagementMock.state = "Unresolved"
        cacheManagementMock.canStart = false
        cacheManagementMock.canRefresh = true
        cacheManagementMock.requiresAcknowledgment = true
        cacheManagementMock.message = "The cleanup result is unresolved."
        wait(0)
        verify(refresh.visible)
        verify(refresh.enabled)
        verify(acknowledge.visible)
        compare(status.Accessible.role, Accessible.AlertMessage)
        const refreshPosition = refresh.mapToItem(page.contentItem, 0, 0)
        page.contentItem.contentY = Math.max(
            0, refreshPosition.y - 180)
        wait(0)
        refresh.forceActiveFocus()
        tryVerify(() => refresh.activeFocus)
        keyClick(Qt.Key_Space)
        compare(cacheManagementMock.refreshCount, 1)
        const acknowledgePosition = acknowledge.mapToItem(
            page.contentItem, 0, 0)
        page.contentItem.contentY = Math.max(
            0, acknowledgePosition.y - 180)
        wait(0)
        acknowledge.forceActiveFocus()
        tryVerify(() => acknowledge.activeFocus)
        keyClick(Qt.Key_Space)
        compare(cacheManagementMock.acknowledgmentCount, 1)

        cacheManagementMock.state = "NotSupported"
        cacheManagementMock.canStart = false
        wait(0)
        verify(!start.enabled)

        cacheManagementMock.state = "Ready"
        cacheManagementMock.canStart = true
        const narrow = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 320, "height": 620, "visible": true})
        verify(narrow !== null)
        wait(0)
        const narrowPanel = findChild(narrow, "temporaryFilesPanel")
        const narrowStart = findChild(
            narrow, "temporaryFilesStartButton")
        verify(narrowPanel !== null)
        verify(narrowStart !== null)
        const panelPosition = narrowPanel.mapToItem(narrow, 0, 0)
        verify(panelPosition.x >= 0)
        verify(panelPosition.x + narrowPanel.width <= narrow.width + 1)
        const narrowPosition = narrowStart.mapToItem(
            narrow.contentItem, 0, 0)
        narrow.contentItem.contentY = Math.max(
            0, narrowPosition.y - 120)
        wait(0)
        mouseClick(
            narrowStart,
            narrowStart.width / 2,
            narrowStart.height / 2)
        const narrowDialog = findChild(
            narrow, "temporaryFilesConfirmation")
        verify(narrowDialog !== null)
        tryVerify(() => narrowDialog.opened)
        verify(narrowDialog.width <= 272 + 1)
        const narrowDialogPosition = narrowDialog.parent.mapToItem(
            narrow, narrowDialog.x, narrowDialog.y)
        verify(narrowDialogPosition.x >= 0)
        verify(narrowDialogPosition.x + narrowDialog.width <=
               narrow.width + 1)
        keyClick(Qt.Key_Escape)
    }

    function test_temporaryFilesRussianTranslation() {
        cacheManagementMock.state = "Ready"
        cacheManagementMock.phase = "Idle"
        cacheManagementMock.canStart = true
        cacheManagementMock.canCancel = false
        cacheManagementMock.message =
            "Ready to remove unused temporary files."

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720, "visible": true})
        verify(page !== null)
        wait(0)

        const title = findChild(page, "temporaryFilesTitle")
        const description = findChild(
            page, "temporaryFilesDescription")
        const start = findChild(page, "temporaryFilesStartButton")
        verify(title !== null)
        verify(description !== null)
        verify(start !== null)
        if (title.text === "Temporary files")
            skip("Russian translation catalog is not loaded")

        compare(title.text, "Временные файлы")
        verify(description.text.includes("неактивные предпросмотры"))
        verify(description.text.toLowerCase().includes(
                   "видимые миниатюры"))
        compare(start.text, "Удалить...")

        const startPosition = start.mapToItem(page.contentItem, 0, 0)
        page.contentItem.contentY = Math.max(0, startPosition.y - 180)
        wait(0)
        mouseClick(start, start.width / 2, start.height / 2)
        const dialog = findChild(page, "temporaryFilesConfirmation")
        const dialogCancel = findChild(
            page, "temporaryFilesDialogCancelButton")
        const dialogAccept = findChild(
            page, "temporaryFilesDialogAcceptButton")
        verify(dialog !== null)
        verify(dialogCancel !== null)
        verify(dialogAccept !== null)
        tryVerify(() => dialog.opened)
        compare(dialog.title, "Удалить ненужные временные файлы?")
        compare(dialogCancel.text, "Отмена")
        compare(dialogAccept.text, "Удалить временные файлы")
        keyClick(Qt.Key_Escape)
    }

    function test_deviceSpecificationsStates_data() {
        return [
            {"tag": "not-supported", "status": "NotSupported",
             "expected": "Not supported by the active runtime"},
            {"tag": "runtime-unavailable", "status": "RuntimeUnavailable",
             "expected": "Device specifications are unavailable because the runtime response could not be verified"},
            {"tag": "disconnected", "status": "Disconnected",
             "expected": "Connect a supported device"},
            {"tag": "unsupported", "status": "Unsupported",
             "expected": "Not available for this device"},
            {"tag": "unavailable", "status": "Unavailable",
             "expected": "Not reported for the current connection"},
            {"tag": "ready", "status": "Ready", "expected": ""}
        ]
    }

    function test_deviceSpecificationsStates(data) {
        runtimeMock.deviceSpecificationsSupported =
            data.status !== "NotSupported"
        runtimeMock.deviceSpecificationsStatus = data.status
        runtimeMock.deviceSpecificationsReady = data.status === "Ready"

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720})
        verify(page !== null)
        wait(0)

        const title = findChild(page, "deviceSpecificationsTitle")
        const state = findChild(page, "deviceSpecificationsState")
        const values = findChild(page, "deviceSpecificationsValues")
        verify(title !== null)
        verify(state !== null)
        verify(values !== null)
        compare(title.text, "Device Specifications")
        compare(title.Accessible.name, title.text)
        compare(state.visible, data.status !== "Ready")
        compare(values.visible, data.status === "Ready")
        if (data.status !== "Ready") {
            compare(state.text, data.expected)
            compare(state.textFormat, Text.PlainText)
            compare(state.Accessible.name, data.expected)
        }
    }

    function test_deviceSpecificationsReadyValuesAreSafeAndHonest() {
        runtimeMock.deviceSpecificationsSupported = true
        runtimeMock.deviceSpecificationsStatus = "Ready"
        runtimeMock.deviceSpecificationsReady = true
        runtimeMock.deviceReportedProductName = "<b>device</b>"
        runtimeMock.deviceVideoOutputWidth = 2240
        runtimeMock.deviceVideoOutputHeight = 1080
        runtimeMock.deviceScreenType = "OLED"
        runtimeMock.deviceUsbAutoKeepalive = false
        runtimeMock.mediaTargetWidth = 2240
        runtimeMock.mediaTargetHeight = 1080

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720})
        verify(page !== null)
        wait(0)

        const product = findChild(
            page, "deviceReportedProductValue")
        const video = findChild(page, "deviceVideoOutputValue")
        const screen = findChild(page, "deviceScreenTypeValue")
        const keepalive = findChild(
            page, "deviceUsbAutoKeepaliveValue")
        const warning = findChild(
            page, "deviceGeometryMismatchWarning")
        for (const value of [product, video, screen, keepalive, warning])
            verify(value !== null)

        compare(product.text, "<b>device</b>")
        compare(product.textFormat, Text.PlainText)
        compare(product.wrapMode, Text.WrapAnywhere)
        compare(product.Accessible.name, product.text)
        compare(video.text, "2240 × 1080")
        compare(screen.text, "OLED")
        compare(keepalive.text, "Disabled")
        compare(video.textFormat, Text.PlainText)
        compare(screen.textFormat, Text.PlainText)
        compare(keepalive.textFormat, Text.PlainText)
        verify(!warning.visible)

        runtimeMock.deviceUsbAutoKeepalive = true
        runtimeMock.mediaTargetWidth = 1280
        runtimeMock.mediaTargetHeight = 720
        wait(0)
        compare(keepalive.text, "Enabled")
        verify(warning.visible)
        compare(
            warning.text,
            "Device video output does not match the current media target (1280 × 720).")
        compare(warning.textFormat, Text.PlainText)
        compare(warning.Accessible.role, Accessible.AlertMessage)
        compare(warning.Accessible.name, warning.text)
    }

    function test_deviceSpecificationsRussianTranslation() {
        runtimeMock.deviceSpecificationsSupported = false
        runtimeMock.deviceSpecificationsStatus = "NotSupported"
        runtimeMock.deviceSpecificationsReady = false
        runtimeMock.mediaTargetWidth = 1280
        runtimeMock.mediaTargetHeight = 720

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720})
        verify(page !== null)
        wait(0)

        const title = findChild(page, "deviceSpecificationsTitle")
        const state = findChild(page, "deviceSpecificationsState")
        const keepalive = findChild(
            page, "deviceUsbAutoKeepaliveValue")
        const warning = findChild(
            page, "deviceGeometryMismatchWarning")
        verify(title !== null)
        verify(state !== null)
        verify(keepalive !== null)
        verify(warning !== null)
        if (title.text === "Device Specifications")
            skip("Russian translation catalog is not loaded")

        compare(title.text, "Характеристики устройства")
        compare(state.text,
                "Не поддерживается активной средой выполнения")
        compare(state.Accessible.name, state.text)

        runtimeMock.deviceSpecificationsSupported = true
        runtimeMock.deviceSpecificationsStatus = "RuntimeUnavailable"
        wait(0)
        compare(
            state.text,
            "Характеристики устройства недоступны, так как не удалось проверить ответ среды выполнения")
        runtimeMock.deviceSpecificationsStatus = "Disconnected"
        wait(0)
        compare(state.text, "Подключите поддерживаемое устройство")
        runtimeMock.deviceSpecificationsStatus = "Unsupported"
        wait(0)
        compare(state.text, "Недоступно для этого устройства")
        runtimeMock.deviceSpecificationsStatus = "Unavailable"
        wait(0)
        compare(state.text, "Не сообщено для текущего подключения")

        runtimeMock.deviceSpecificationsStatus = "Ready"
        runtimeMock.deviceSpecificationsReady = true
        runtimeMock.deviceUsbAutoKeepalive = false
        runtimeMock.deviceVideoOutputWidth = 2240
        runtimeMock.deviceVideoOutputHeight = 1080
        wait(0)
        compare(keepalive.text, "Отключено")
        verify(warning.visible)
        compare(
            warning.text,
            "Видеовыход устройства не соответствует текущему целевому разрешению медиа (1280 × 720).")
        compare(warning.Accessible.name, warning.text)
    }

    function test_deviceSummaryStaysInsideNarrowContent() {
        runtimeMock.deviceModel = "PANORAMA SE"
        runtimeMock.productId = "391a:1021"
        runtimeMock.firmwareVersion =
            "PASE-FIRMWARE-WITH-A-LONG-REPORTED-VERSION"
        runtimeMock.deviceAppVersion =
            "PASE-APPLICATION-WITH-A-LONG-REPORTED-VERSION"
        runtimeMock.deviceSpecificationsStatus = "Ready"
        runtimeMock.deviceSpecificationsReady = true
        runtimeMock.deviceReportedProductName = "P".repeat(128)
        runtimeMock.deviceVideoOutputWidth = 2240
        runtimeMock.deviceVideoOutputHeight = 1080
        runtimeMock.deviceScreenType = "OLED"
        runtimeMock.deviceUsbAutoKeepalive = false
        runtimeMock.mediaTargetWidth = 1280
        runtimeMock.mediaTargetHeight = 720

        const page = createTemporaryObject(
            settingsComponent, testCase,
            {"width": 760, "height": 720})
        verify(page !== null)
        wait(0)

        const content = findChild(page, "settingsContent")
        const objectNames = [
            "deviceModelValue",
            "deviceProductIdValue",
            "deviceFirmwareVersionValue",
            "deviceAppVersionValue",
            "deviceReportedProductValue",
            "deviceVideoOutputValue",
            "deviceScreenTypeValue",
            "deviceUsbAutoKeepaliveValue",
            "deviceGeometryMismatchWarning"
        ]
        verify(content !== null)
        for (const objectName of objectNames) {
            const value = findChild(page, objectName)
            verify(value !== null)
            const position = value.mapToItem(content, 0, 0)
            verify(position.x >= 0)
            verify(position.x + value.width <= content.width + 1)
        }
    }
}
