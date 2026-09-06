import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../components"

ScrollView {
    id: root

    required property var runtime
    required property var settings
    required property var firmware
    required property var windowChrome
    required property var supportBundle
    required property var cacheManagement

    property bool updateAvailable: false
    property string availableVersion: ""
    property url releaseUrl: ""
    signal openReleaseRequested()

    property string temporaryFilesFocusState:
        cacheManagement.state
    property string temporaryFilesFocusPhase:
        cacheManagement.phase

    clip: true

    readonly property var languages: [
        {"code": "en", "label": qsTr("English")},
        {"code": "ru", "label": qsTr("Russian")},
        {"code": "system", "label": qsTr("System language")}
    ]
    readonly property var temperatureUnits: [
        {"value": "Celsius", "label": qsTr("Celsius (°C)")},
        {"value": "Fahrenheit", "label": qsTr("Fahrenheit (°F)")}
    ]
    readonly property var timeFormats: [
        {"value": "24H", "label": qsTr("24-hour")},
        {"value": "12H", "label": qsTr("12-hour (AM/PM)")}
    ]
    readonly property url projectHomeUrl:
        "https://github.com/DXVSI/Tryx-Linux-GUI"
    readonly property url projectLicenseUrl:
        "https://github.com/DXVSI/Tryx-Linux-GUI/blob/production/LICENSE"

    function reportedValue(value) {
        const normalized = value === undefined || value === null
                           ? "" : String(value).trim()
        return normalized.length > 0 ? normalized : qsTr("Not reported")
    }

    function deviceSpecificationsStateText(status) {
        switch (status) {
        case "NotSupported":
            return qsTr("Not supported by the active runtime")
        case "Disconnected":
            return qsTr("Connect a supported device")
        case "Unsupported":
            return qsTr("Not available for this device")
        case "Unavailable":
            return qsTr("Not reported for the current connection")
        case "Ready":
            return ""
        case "RuntimeUnavailable":
        default:
            return qsTr("Device specifications are unavailable because the runtime response could not be verified")
        }
    }

    ColumnLayout {
        objectName: "settingsContent"
        x: 28
        y: 24
        width: Math.max(0, root.availableWidth - 56)
        spacing: 16

        Frame {
            Layout.fillWidth: true
            padding: 22

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: ColumnLayout {
                spacing: 16

                Label {
                    text: qsTr("General")
                    color: "#f4f6f7"
                    font.pixelSize: 18
                    font.bold: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Application language")
                            color: "#f4f6f7"
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: qsTr("Changes are applied immediately.")
                            color: "#9ca4ac"
                            wrapMode: Text.WordWrap
                        }
                    }

                    ComboBox {
                        objectName: "languageCombo"
                        Layout.preferredWidth: 220
                        model: root.languages
                        textRole: "label"
                        currentIndex: Math.max(
                            0,
                            root.languages.findIndex(
                                item => item.code ===
                                        root.settings.language))
                        onActivated: index => {
                            root.settings.setLanguage(
                                root.languages[index].code)
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true

                        Label {
                            id: temperatureUnitLabel

                            Layout.fillWidth: true
                            text: qsTr("Temperature unit")
                            color: "#f4f6f7"
                            font.bold: true
                        }

                        Label {
                            id: temperatureUnitDescription

                            Layout.fillWidth: true
                            text: qsTr("Used for temperatures in the dashboard and on the display overlay.")
                            color: "#9ca4ac"
                            wrapMode: Text.WordWrap
                        }
                    }

                    ComboBox {
                        id: temperatureUnitCombo

                        objectName: "temperatureUnitCombo"
                        Layout.preferredWidth: 220
                        model: root.temperatureUnits
                        textRole: "label"
                        enabled: root.runtime.presentationPreferencesReady &&
                                 !root.runtime.presentationPreferencesBusy
                        Accessible.name: temperatureUnitLabel.text
                        Accessible.description:
                            temperatureUnitDescription.text
                        onActivated: index => {
                            root.runtime.setPresentationPreferences(
                                root.temperatureUnits[index].value,
                                root.runtime.timeFormat)
                        }

                        Binding on currentIndex {
                            value: Math.max(
                                0,
                                root.temperatureUnits.findIndex(
                                    item => item.value ===
                                            root.runtime.temperatureUnit))
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true

                        Label {
                            id: timeFormatLabel

                            Layout.fillWidth: true
                            text: qsTr("Time format")
                            color: "#f4f6f7"
                            font.bold: true
                        }

                        Label {
                            id: timeFormatDescription

                            Layout.fillWidth: true
                            text: qsTr("Uses the system local time zone on the display overlay.")
                            color: "#9ca4ac"
                            wrapMode: Text.WordWrap
                        }
                    }

                    ComboBox {
                        id: timeFormatCombo

                        objectName: "timeFormatCombo"
                        Layout.preferredWidth: 220
                        model: root.timeFormats
                        textRole: "label"
                        enabled: root.runtime.presentationPreferencesReady &&
                                 !root.runtime.presentationPreferencesBusy
                        Accessible.name: timeFormatLabel.text
                        Accessible.description:
                            timeFormatDescription.text
                        onActivated: index => {
                            root.runtime.setPresentationPreferences(
                                root.runtime.temperatureUnit,
                                root.timeFormats[index].value)
                        }

                        Binding on currentIndex {
                            value: Math.max(
                                0,
                                root.timeFormats.findIndex(
                                    item => item.value ===
                                            root.runtime.timeFormat))
                        }
                    }
                }

                Label {
                    objectName: "presentationPreferencesExplanation"
                    Layout.fillWidth: true
                    text: root.runtime.presentationPreferencesBusy
                          ? qsTr("Saving presentation preferences…")
                          : (root.runtime.presentationPreferencesReady
                             ? qsTr("The background runtime keeps the dashboard and display overlay in sync.")
                             : qsTr("These preferences are unavailable from the active background runtime. Celsius and 24-hour format remain in use."))
                    color: root.runtime.presentationPreferencesReady
                           ? "#9ca4ac" : "#efb85f"
                    wrapMode: Text.WordWrap
                    Accessible.name: text
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop

                        Label {
                            text: qsTr("Close button behavior")
                            color: "#f4f6f7"
                            font.bold: true
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("The background runtime keeps running in either mode.")
                            color: "#9ca4ac"
                            wrapMode: Text.WordWrap
                        }
                    }

                    ColumnLayout {
                        Layout.preferredWidth: 280
                        Layout.alignment: Qt.AlignTop
                        spacing: 2

                        RadioButton {
                            id: hideToTrayRadio

                            objectName: "hideToTrayRadio"
                            Layout.fillWidth: true
                            text: qsTr("Hide to tray")
                            enabled: root.windowChrome.trayAvailable
                            Accessible.description: enabled
                                ? qsTr("Keep the GUI available from the system tray.")
                                : qsTr("The desktop system tray is unavailable.")
                            onClicked:
                                root.settings.setHideToTrayOnClose(true)

                            Binding on checked {
                                value: root.settings.hideToTrayOnClose
                            }
                        }

                        Label {
                            objectName: "trayUnavailableExplanation"
                            Layout.fillWidth: true
                            visible: !root.windowChrome.trayAvailable
                            text: qsTr("The desktop system tray is unavailable. Closing the window will quit the GUI.")
                            color: "#efb85f"
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                        }

                        RadioButton {
                            id: quitGuiRadio

                            objectName: "quitGuiRadio"
                            Layout.fillWidth: true
                            text: qsTr("Quit GUI")
                            Accessible.description: qsTr("Close only the desktop interface.")
                            onClicked:
                                root.settings.setHideToTrayOnClose(false)

                            Binding on checked {
                                value: !root.settings.hideToTrayOnClose
                            }
                        }
                    }
                }
            }
        }

        FirmwarePanel {
            objectName: "firmwarePanel"
            Layout.fillWidth: true
            controller: root.firmware
        }

        Frame {
            Layout.fillWidth: true
            padding: 22

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: ColumnLayout {
                spacing: 16

                Label {
                    text: qsTr("Startup")
                    color: "#f4f6f7"
                    font.pixelSize: 18
                    font.bold: true
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    ColumnLayout {
                        Layout.fillWidth: true
                        Label {
                            id: autostartTitleLabel

                            objectName: "autostartTitle"
                            text: qsTr("Start TRYX Panorama Manager when you sign in")
                            color: "#f4f6f7"
                            font.bold: true
                        }
                        Label {
                            id: autostartDescriptionLabel

                            objectName: "autostartDescription"
                            text: root.settings.busy
                                  ? qsTr("Updating GUI autostart…")
                                  : (root.settings.autostartAvailable
                                     ? qsTr("Starts hidden only when Hide to tray is selected and a system tray is available. The background service is unchanged.")
                                     : qsTr("GUI autostart state is unavailable. The background service is unchanged."))
                            color: "#9ca4ac"
                        }
                    }

                    Switch {
                        objectName: "autostartSwitch"
                        enabled: !root.settings.busy
                                 && root.settings.autostartAvailable
                        text: root.settings.autostartEnabled
                              ? qsTr("On") : qsTr("Off")
                        Accessible.name: autostartTitleLabel.text
                        Accessible.description:
                            autostartDescriptionLabel.text
                        onToggled: root.settings.setAutostartEnabled(
                            checked)

                        Binding on checked {
                            value: root.settings.autostartEnabled
                        }
                    }
                }
            }
        }

        Frame {
            objectName: "devicePanel"
            Layout.fillWidth: true
            padding: 22

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: ColumnLayout {
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Device")
                        color: "#f4f6f7"
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Button {
                        text: qsTr("Refresh status")
                        enabled: !root.runtime.operationBusy
                        onClicked: {
                            root.runtime.refreshAll()
                            root.settings.refreshAutostart()
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 3
                    columnSpacing: 16
                    rowSpacing: 10

                    Label {
                        text: qsTr("Legacy serial port")
                        color: "#9ca4ac"
                    }

                    ComboBox {
                        id: serialPortCombo

                        objectName: "serialPortCombo"
                        Layout.fillWidth: true
                        model: [qsTr("Auto")].concat(
                                   root.settings.serialPorts)
                        currentIndex: {
                            if (root.settings.devicePort.length === 0)
                                return 0
                            const portIndex =
                                root.settings.serialPorts.indexOf(
                                    root.settings.devicePort)
                            return portIndex < 0 ? 0 : portIndex + 1
                        }
                        onActivated: index => {
                            root.settings.setDevicePort(
                                index === 0
                                ? ""
                                : root.settings.serialPorts[index - 1])
                        }
                    }

                    Button {
                        text: qsTr("Rescan ports")
                        onClicked: root.settings.refreshSerialPorts()
                    }

                    Label {
                        text: qsTr("Keepalive interval")
                        color: "#9ca4ac"
                    }

                    SpinBox {
                        id: keepaliveSpin

                        objectName: "keepaliveSpin"
                        from: 5
                        to: 60
                        value: root.settings.keepaliveInterval
                        editable: true
                        textFromValue: value => qsTr("%1 s").arg(value)
                        valueFromText: text => {
                            const parsed = parseInt(text)
                            return isNaN(parsed)
                                   ? root.settings.keepaliveInterval
                                   : parsed
                        }
                        onValueModified:
                            root.settings.setKeepaliveInterval(value)
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Used by legacy serial/ADB devices. TRYX printer-class devices are detected automatically.")
                        color: "#7f8991"
                        wrapMode: Text.WordWrap
                    }

                    Button {
                        objectName: "reconnectDeviceButton"
                        text: qsTr("Reconnect")
                        enabled: root.runtime.serviceAvailable &&
                                 !root.runtime.operationBusy
                        onClicked: {
                            root.runtime.disconnectDevice()
                            root.runtime.connectDevice(
                                root.settings.devicePort)
                            root.runtime.startKeepalive(
                                root.settings.keepaliveInterval)
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 24
                    rowSpacing: 12

                    Label {
                        text: qsTr("Background service")
                        color: "#9ca4ac"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.runtime.serviceAvailable
                              ? qsTr("Running")
                              : qsTr("Not running")
                        color: root.runtime.serviceAvailable
                               ? "#66d18f" : "#efb85f"
                    }

                    Label {
                        text: qsTr("USB device")
                        color: "#9ca4ac"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.runtime.printerClassDevicePresent
                              ? qsTr("Detected")
                              : qsTr("Not detected")
                        color: root.runtime.printerClassDevicePresent
                               ? "#66d18f" : "#efb85f"
                    }

                    Label {
                        text: qsTr("Model")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceModelValue"
                        Layout.fillWidth: true
                        text: root.reportedValue(root.runtime.deviceModel)
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: "#f4f6f7"
                    }

                    Label {
                        text: qsTr("Product ID")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceProductIdValue"
                        Layout.fillWidth: true
                        text: root.reportedValue(root.runtime.productId)
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: "#f4f6f7"
                    }

                    Label {
                        text: qsTr("Firmware version")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceFirmwareVersionValue"
                        Layout.fillWidth: true
                        text: root.reportedValue(
                                  root.runtime.firmwareVersion)
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: "#f4f6f7"
                    }

                    Label {
                        text: qsTr("Device app version")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceAppVersionValue"
                        Layout.fillWidth: true
                        text: root.reportedValue(
                                  root.runtime.deviceAppVersion)
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: "#f4f6f7"
                    }

                    Label {
                        text: qsTr("Transport")
                        color: "#9ca4ac"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.runtime.printerClassDevicePresent
                              ? qsTr("TRYX printer class")
                              : (root.runtime.legacyConnected
                                 ? qsTr("Legacy serial / ADB")
                                 : qsTr("Waiting for device"))
                        color: root.runtime.printerClassDevicePresent ||
                               root.runtime.legacyConnected
                               ? "#66d18f" : "#efb85f"
                    }

                    Label {
                        text: qsTr("Display session")
                        color: "#9ca4ac"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.runtime.displaySessionActive
                              ? qsTr("Ready")
                              : qsTr("Waiting")
                        color: root.runtime.displaySessionActive
                               ? "#66d18f" : "#efb85f"
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: "#343b42"
                }

                Label {
                    objectName: "deviceSpecificationsTitle"
                    Layout.fillWidth: true
                    text: qsTr("Device Specifications")
                    textFormat: Text.PlainText
                    color: "#f4f6f7"
                    font.pixelSize: 16
                    font.bold: true
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }

                Label {
                    objectName: "deviceSpecificationsState"
                    Layout.fillWidth: true
                    visible: !root.runtime.deviceSpecificationsReady
                    text: root.deviceSpecificationsStateText(
                              root.runtime.deviceSpecificationsStatus)
                    textFormat: Text.PlainText
                    color: "#efb85f"
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                GridLayout {
                    objectName: "deviceSpecificationsValues"
                    Layout.fillWidth: true
                    visible: root.runtime.deviceSpecificationsReady
                    columns: 2
                    columnSpacing: 24
                    rowSpacing: 12

                    Label {
                        text: qsTr("Device-reported product")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceReportedProductValue"
                        Layout.fillWidth: true
                        text: root.runtime.deviceReportedProductName
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: "#f4f6f7"
                        Accessible.name: text
                    }

                    Label {
                        text: qsTr("Video output")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceVideoOutputValue"
                        Layout.fillWidth: true
                        text: qsTr("%1 × %2")
                              .arg(root.runtime.deviceVideoOutputWidth)
                              .arg(root.runtime.deviceVideoOutputHeight)
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: "#f4f6f7"
                        Accessible.name: text
                    }

                    Label {
                        text: qsTr("Screen type")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceScreenTypeValue"
                        Layout.fillWidth: true
                        text: root.runtime.deviceScreenType
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: "#f4f6f7"
                        Accessible.name: text
                    }

                    Label {
                        text: qsTr("USB automatic keepalive")
                        color: "#9ca4ac"
                    }
                    Label {
                        objectName: "deviceUsbAutoKeepaliveValue"
                        Layout.fillWidth: true
                        text: root.runtime.deviceUsbAutoKeepalive
                              ? qsTr("Enabled") : qsTr("Disabled")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: "#f4f6f7"
                        Accessible.name: text
                    }
                }

                Label {
                    objectName: "deviceGeometryMismatchWarning"
                    Layout.fillWidth: true
                    visible: root.runtime.deviceSpecificationsReady &&
                             (root.runtime.deviceVideoOutputWidth !==
                                  root.runtime.mediaTargetWidth ||
                              root.runtime.deviceVideoOutputHeight !==
                                  root.runtime.mediaTargetHeight)
                    text: qsTr("Device video output does not match the current media target (%1 × %2).")
                          .arg(root.runtime.mediaTargetWidth)
                          .arg(root.runtime.mediaTargetHeight)
                    textFormat: Text.PlainText
                    color: "#efb85f"
                    wrapMode: Text.WordWrap
                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.runtime.diagnostic.length > 0
                    text: root.runtime.diagnostic
                    color: "#9ca4ac"
                    wrapMode: Text.WordWrap
                }
            }
        }

        Frame {
            objectName: "temporaryFilesPanel"
            Layout.fillWidth: true
            padding: 22

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: ColumnLayout {
                spacing: 14

                Label {
                    id: temporaryFilesTitle

                    objectName: "temporaryFilesTitle"
                    Layout.fillWidth: true
                    text: qsTr("Temporary files")
                    textFormat: Text.PlainText
                    color: "#f4f6f7"
                    font.pixelSize: 18
                    font.bold: true
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }

                Label {
                    id: temporaryFilesDescription

                    objectName: "temporaryFilesDescription"
                    Layout.fillWidth: true
                    text: qsTr("Remove only inactive previews, orphaned thumbnails, and expired idle copies. Visible thumbnails, saved layouts, recovery data, operation files, and files in use are preserved.")
                    textFormat: Text.PlainText
                    color: "#9ca4ac"
                    wrapMode: Text.WordWrap
                    Accessible.name: text
                }

                Label {
                    id: temporaryFilesStatus

                    objectName: "temporaryFilesStatus"
                    Layout.fillWidth: true
                    activeFocusOnTab: true
                    text: {
                        const base = root.cacheManagement.message
                        if (root.cacheManagement.removedFiles <= 0)
                            return base
                        return base + "\n" +
                            qsTr("Removed files: %1. Runtime: %2 files, %3 bytes. Preview: %4 files, %5 bytes. Size of removed files: %6 bytes.")
                                .arg(root.cacheManagement.removedFiles)
                                .arg(root.cacheManagement.runtimeRemovedFiles)
                                .arg(root.cacheManagement.runtimeRemovedBytes)
                                .arg(root.cacheManagement.localRemovedFiles)
                                .arg(root.cacheManagement.localRemovedBytes)
                                .arg(root.cacheManagement.removedBytes)
                    }
                    textFormat: Text.PlainText
                    color: root.cacheManagement.state === "Partial" ||
                           root.cacheManagement.state === "Unresolved" ||
                           root.cacheManagement.state === "Blocked"
                           ? "#efb85f"
                           : (root.cacheManagement.state === "Succeeded"
                              ? "#66d18f" : "#9ca4ac")
                    wrapMode: Text.WordWrap
                    Accessible.role:
                        root.cacheManagement.state === "Partial" ||
                        root.cacheManagement.state === "Unresolved" ||
                        root.cacheManagement.state === "Blocked"
                        ? Accessible.AlertMessage
                        : Accessible.StatusBar
                    Accessible.name: text
                }

                GridLayout {
                    objectName: "temporaryFilesActions"
                    Layout.fillWidth: true
                    columns: root.availableWidth <= 430 ? 1 : 2
                    columnSpacing: 12
                    rowSpacing: 8

                    Button {
                        id: temporaryFilesStartButton

                        objectName: "temporaryFilesStartButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        visible: root.cacheManagement.state !== "Running" &&
                                 root.cacheManagement.state !== "Unresolved"
                        enabled: root.cacheManagement.canStart
                        text: qsTr("Remove...")
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description:
                            temporaryFilesDescription.text
                        onClicked: {
                            temporaryFilesConfirmation.pendingEligibilityRevision =
                                root.cacheManagement.eligibilityRevision
                            temporaryFilesConfirmation.open()
                        }
                    }

                    Button {
                        id: temporaryFilesCancelButton

                        objectName: "temporaryFilesCancelButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        visible: root.cacheManagement.state === "Running" &&
                                 root.cacheManagement.phase === "Runtime"
                        enabled: root.cacheManagement.canCancel
                        text: qsTr("Cancel cleanup")
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description: qsTr(
                            "Cancel only this temporary file cleanup operation.")
                        onClicked: root.cacheManagement.cancelCleanup()
                    }

                    Button {
                        id: temporaryFilesRefreshButton

                        objectName: "temporaryFilesRefreshButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        visible: root.cacheManagement.state === "Unresolved"
                        enabled: root.cacheManagement.canRefresh
                        text: qsTr("Refresh state")
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description: qsTr(
                            "Ask the original runtime owner for this exact operation without repeating cleanup.")
                        onClicked: {
                            root.cacheManagement.refresh()
                            temporaryFilesStatus.forceActiveFocus()
                        }
                    }

                    Button {
                        id: temporaryFilesAcknowledgeButton

                        objectName: "temporaryFilesAcknowledgeButton"
                        Layout.fillWidth: true
                        Layout.minimumHeight: 44
                        visible:
                            root.cacheManagement.requiresAcknowledgment
                        enabled: visible
                        text: qsTr("Acknowledge unknown result")
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description: qsTr(
                            "Stop tracking the unknown result. A new cleanup will use a new operation identity.")
                        onClicked: {
                            root.cacheManagement.acknowledgeUnresolved()
                            Qt.callLater.call(Qt, function() {
                                if (temporaryFilesStartButton.enabled)
                                    temporaryFilesStartButton.forceActiveFocus()
                                else
                                    temporaryFilesStatus.forceActiveFocus()
                            })
                        }
                    }
                }
            }
        }

        Frame {
            objectName: "supportBundlePanel"
            Layout.fillWidth: true
            padding: 22

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: ColumnLayout {
                spacing: 14

                Label {
                    text: qsTr("Support report")
                    color: "#f4f6f7"
                    font.pixelSize: 18
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Create a redacted JSON report for troubleshooting. It is saved locally and is never uploaded automatically.")
                    textFormat: Text.PlainText
                    color: "#9ca4ac"
                    wrapMode: Text.WordWrap
                    Accessible.name: text
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    Label {
                        id: supportBundleDescription

                        objectName: "supportBundleDescription"
                        Layout.fillWidth: true
                        text: root.supportBundle.busy
                              ? qsTr("Collecting and saving support information…")
                              : (root.supportBundle.runtimeDetailsAvailable
                                 ? qsTr("The report will include host, application, and allowlisted runtime details.")
                                 : qsTr("Runtime details are unavailable. A host-only report will be created."))
                        textFormat: Text.PlainText
                        color: root.supportBundle.runtimeDetailsAvailable
                               ? "#9ca4ac" : "#efb85f"
                        wrapMode: Text.WordWrap
                        Accessible.name: text
                    }

                    Button {
                        id: supportBundleExportButton

                        objectName: "supportBundleExportButton"
                        text: qsTr("Export report…")
                        enabled: !root.supportBundle.busy
                        activeFocusOnTab: true
                        Accessible.name: text
                        Accessible.description:
                            supportBundleDescription.text
                        onClicked:
                            supportBundleFolderPicker.openForExport()
                    }
                }

                Label {
                    objectName: "supportBundleResult"
                    Layout.fillWidth: true
                    visible: root.supportBundle.state !== "idle" &&
                             root.supportBundle.message.length > 0
                    text: root.supportBundle.message
                    textFormat: Text.PlainText
                    color: root.supportBundle.state === "error"
                           ? "#ef6b73"
                           : (root.supportBundle.state === "saved"
                              ? "#66d18f" : "#efb85f")
                    wrapMode: Text.WrapAnywhere
                    Accessible.role:
                        root.supportBundle.state === "error" ||
                        root.supportBundle.state === "saved"
                        ? Accessible.AlertMessage
                        : Accessible.StaticText
                    Accessible.name: text
                }
            }
        }

        Frame {
            Layout.fillWidth: true
            padding: 22

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: Item {
                implicitHeight: Math.max(
                    aboutDetails.implicitHeight,
                    aboutActions.implicitHeight)

                ColumnLayout {
                    id: aboutDetails

                    anchors.left: parent.left
                    anchors.right: aboutActions.left
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0

                    Label {
                        text: qsTr("About")
                        color: "#f4f6f7"
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Label {
                        text: qsTr("TRYX Panorama Manager %1")
                              .arg(Qt.application.version)
                        color: "#9ca4ac"
                    }
                    Label {
                        text: qsTr("Open-source Linux control application")
                        color: "#9ca4ac"
                    }

                    ColumnLayout {
                        objectName: "releaseUpdateRow"
                        Layout.fillWidth: true
                        Layout.topMargin: visible ? 12 : 0
                        spacing: 8
                        visible: root.updateAvailable
                        onVisibleChanged: {
                            if (!visible && openReleaseButton.activeFocus)
                                openGitHubButton.forceActiveFocus()
                        }

                        Label {
                            id: availableReleaseLabel
                            objectName: "availableReleaseLabel"
                            Layout.fillWidth: true
                            text: qsTr("Version %1 is available").arg(root.availableVersion)
                            textFormat: Text.PlainText
                            color: "#def750"
                            wrapMode: Text.WordWrap
                            Accessible.name: text
                        }

                        Button {
                            id: openReleaseButton
                            objectName: "openReleaseButton"
                            text: qsTr("Open release")
                            enabled: root.updateAvailable && String(root.releaseUrl).length > 0
                            Accessible.name: text
                            Accessible.description: availableReleaseLabel.text
                            onClicked: root.openReleaseRequested()
                        }
                    }
                }

                ColumnLayout {
                    id: aboutActions

                    objectName: "aboutActions"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8

                    Button {
                        id: openGitHubButton

                        objectName: "openGitHubButton"
                        Layout.fillWidth: true
                        readonly property url destinationUrl:
                            root.projectHomeUrl
                        text: qsTr("Open GitHub")
                        onClicked:
                            Qt.openUrlExternally(destinationUrl)
                    }

                    Button {
                        id: openLicenseButton

                        objectName: "openLicenseButton"
                        Layout.fillWidth: true
                        readonly property url destinationUrl:
                            root.projectLicenseUrl
                        text: qsTr("View License")
                        onClicked:
                            Qt.openUrlExternally(destinationUrl)
                    }
                }
            }
        }

        Label {
            objectName: "settingsErrorMessage"
            Layout.fillWidth: true
            visible: root.settings.errorMessage.length > 0
            text: root.settings.errorMessage
            color: "#ef6b73"
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 20
        }
    }

    Dialog {
        id: temporaryFilesConfirmation

        objectName: "temporaryFilesConfirmation"
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: Math.max(
            0, Math.min(
                440, Math.min(parent.width, root.width) - 48))
        x: root.mapToItem(
               parent, Math.max(0, (root.width - width) / 2), 0).x
        y: root.mapToItem(
               parent, 0,
               Math.max(0, (root.height - height) / 2)).y
        title: qsTr("Remove unused temporary files?")

        property bool dispatchAccepted: false
        property real pendingEligibilityRevision: 0

        onOpened: {
            dispatchAccepted = false
            temporaryFilesDialogCancelButton.forceActiveFocus()
        }
        onClosed: {
            if (!dispatchAccepted) {
                Qt.callLater.call(Qt, function() {
                    temporaryFilesStartButton.forceActiveFocus()
                })
            }
            dispatchAccepted = false
            pendingEligibilityRevision = 0
        }

        contentItem: ColumnLayout {
            spacing: 12
            Accessible.role: Accessible.Dialog
            Accessible.name: temporaryFilesConfirmation.title

            Label {
                Layout.fillWidth: true
                text: qsTr("Only inactive previews, orphaned thumbnails, and expired idle copies will be removed. Files in use and recovery data are preserved.")
                textFormat: Text.PlainText
                color: "#f4f6f7"
                wrapMode: Text.WordWrap
                Accessible.name: text
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("This action reports the size of removed files, not guaranteed free disk space.")
                textFormat: Text.PlainText
                color: "#9ca4ac"
                wrapMode: Text.WordWrap
                Accessible.name: text
            }
        }

        footer: GridLayout {
            columns: temporaryFilesConfirmation.width <= 360 ? 1 : 2
            columnSpacing: 12
            rowSpacing: 8

            Button {
                id: temporaryFilesDialogCancelButton

                objectName: "temporaryFilesDialogCancelButton"
                Layout.fillWidth: true
                Layout.minimumHeight: 44
                text: qsTr("Cancel")
                activeFocusOnTab: true
                Accessible.name: text
                Accessible.description: qsTr(
                    "Keep all temporary files and return to Settings.")
                onClicked: temporaryFilesConfirmation.close()
            }

            Button {
                id: temporaryFilesDialogAcceptButton

                objectName: "temporaryFilesDialogAcceptButton"
                Layout.fillWidth: true
                Layout.minimumHeight: 44
                text: qsTr("Remove temporary files")
                activeFocusOnTab: true
                Accessible.name: text
                Accessible.description: qsTr(
                    "Run the allowlisted temporary file cleanup now.")
                onClicked: {
                    const expectedEligibilityRevision =
                        temporaryFilesConfirmation
                            .pendingEligibilityRevision
                    temporaryFilesConfirmation.dispatchAccepted = true
                    temporaryFilesConfirmation.close()
                    const started =
                        root.cacheManagement.startCleanup(
                            expectedEligibilityRevision)
                    Qt.callLater(function() {
                        if (started &&
                            temporaryFilesCancelButton.visible &&
                            temporaryFilesCancelButton.enabled) {
                            temporaryFilesCancelButton.forceActiveFocus()
                        } else {
                            temporaryFilesStatus.forceActiveFocus()
                        }
                    })
                }
            }
        }
    }

    Connections {
        target: root.cacheManagement
        ignoreUnknownSignals: true

        function onEligibilityChanged() {
            if (temporaryFilesConfirmation.opened &&
                !temporaryFilesConfirmation.dispatchAccepted) {
                temporaryFilesConfirmation.close()
            }
        }

        function onStateChanged() {
            if (temporaryFilesConfirmation.opened &&
                !temporaryFilesConfirmation.dispatchAccepted) {
                temporaryFilesConfirmation.close()
            }
            const state = root.cacheManagement.state
            const phase = root.cacheManagement.phase
            const enteredRuntime =
                state === "Running" && phase === "Runtime" &&
                (root.temporaryFilesFocusState !== state ||
                 root.temporaryFilesFocusPhase !== phase)
            const enteredQuick =
                state === "Running" && phase === "Quick" &&
                (root.temporaryFilesFocusState !== state ||
                 root.temporaryFilesFocusPhase !== phase)
            const enteredUnresolved =
                state === "Unresolved" &&
                root.temporaryFilesFocusState !== state
            const enteredTerminal =
                state !== "Running" && state !== "Unresolved" &&
                root.temporaryFilesFocusState !== state
            root.temporaryFilesFocusState = state
            root.temporaryFilesFocusPhase = phase
            if (!enteredRuntime && !enteredQuick &&
                !enteredUnresolved && !enteredTerminal) {
                return
            }
            Qt.callLater(function() {
                if (enteredRuntime &&
                    temporaryFilesCancelButton.visible &&
                    temporaryFilesCancelButton.enabled) {
                    temporaryFilesCancelButton.forceActiveFocus()
                } else if (enteredUnresolved &&
                           temporaryFilesRefreshButton.visible &&
                           temporaryFilesRefreshButton.enabled) {
                    temporaryFilesRefreshButton.forceActiveFocus()
                } else if (!enteredQuick &&
                           root.cacheManagement.canStart &&
                           temporaryFilesStartButton.visible) {
                    temporaryFilesStartButton.forceActiveFocus()
                } else {
                    temporaryFilesStatus.forceActiveFocus()
                }
            })
        }
    }

    SupportBundleExportPicker {
        id: supportBundleFolderPicker

        controller: root.supportBundle
        homeFolder: root.supportBundle.homeFolder
        focusReturnItem: supportBundleExportButton
    }
}
