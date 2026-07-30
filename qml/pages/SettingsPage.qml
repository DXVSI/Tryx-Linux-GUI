import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../components"

ScrollView {
    id: root

    required property var runtime
    required property var settings
    required property var firmware

    clip: true

    readonly property var languages: [
        {"code": "en", "label": qsTr("English")},
        {"code": "ru", "label": qsTr("Russian")},
        {"code": "system", "label": qsTr("System language")}
    ]

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
                            text: qsTr("Changes are applied immediately.")
                            color: "#9ca4ac"
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
                            text: qsTr("Start the background service when you sign in")
                            color: "#f4f6f7"
                            font.bold: true
                        }
                        Label {
                            text: root.settings.busy
                                  ? qsTr("Updating autostart…")
                                  : (root.settings.autostartAvailable
                                     ? qsTr("Managed by your user systemd session")
                                     : qsTr("Autostart state is unavailable"))
                            color: "#9ca4ac"
                        }
                    }

                    Switch {
                        objectName: "autostartSwitch"
                        enabled: !root.settings.busy
                        text: root.settings.autostartEnabled
                              ? qsTr("On") : qsTr("Off")
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
                        text: qsTr("Used by legacy serial/ADB devices. PASE printer-class devices are detected automatically.")
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
                        text: qsTr("Transport")
                        color: "#9ca4ac"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.runtime.printerClassDevicePresent
                              ? qsTr("PASE printer class")
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
                    openGitHubButton.implicitHeight)

                ColumnLayout {
                    id: aboutDetails

                    anchors.left: parent.left
                    anchors.right: openGitHubButton.left
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
                }

                Button {
                    id: openGitHubButton

                    objectName: "openGitHubButton"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Open GitHub")
                    onClicked: Qt.openUrlExternally(
                        "https://github.com/DXVSI/Tryx-Linux-GUI")
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.settings.errorMessage.length > 0
            text: root.settings.errorMessage
            color: "#ef6b73"
            wrapMode: Text.WordWrap
        }

        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 20
        }
    }
}
