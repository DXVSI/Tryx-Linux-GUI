import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: root

    required property var controller
    property string selectedPath: ""

    padding: 22

    function formatBytes(bytes) {
        if (bytes <= 0)
            return qsTr("Unknown size")
        const units = [qsTr("B"), qsTr("KiB"), qsTr("MiB"), qsTr("GiB")]
        let value = bytes
        let unit = 0
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024
            ++unit
        }
        return "%1 %2".arg(value.toFixed(unit === 0 ? 0 : 1))
                         .arg(units[unit])
    }

    function phaseLabel(phase) {
        switch (phase) {
        case "Initializing":
            return qsTr("Initializing")
        case "Idle":
            return qsTr("Ready")
        case "Validating":
            return qsTr("Validating")
        case "Approved":
            return qsTr("Validated")
        case "Expired":
            return qsTr("Approval expired")
        case "Revalidating":
            return qsTr("Final identity check")
        case "Flashing":
            return qsTr("Flashing")
        case "Succeeded":
            return qsTr("Completed")
        case "Failed":
            return qsTr("Failed")
        default:
            return qsTr("Unavailable")
        }
    }

    onSelectedPathChanged: {
        if (selectedPath.length > 0 &&
                selectedPath !== controller.packagePath)
            controller.setPackagePath(selectedPath)
    }

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
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: qsTr("Firmware")
                    color: "#f4f6f7"
                    font.pixelSize: 18
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Validate a local package before sending any flash command")
                    color: "#9ca4ac"
                    wrapMode: Text.WordWrap
                }
            }

            Label {
                text: root.controller.ready
                      ? qsTr("Service ready")
                      : (root.controller.compatible
                         ? qsTr("Initializing…")
                         : qsTr("Service unavailable"))
                color: root.controller.ready
                       ? "#66d18f" : "#efb85f"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#343b42"
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Local firmware ZIP")
            color: "#f4f6f7"
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            TextField {
                id: packagePathField

                Layout.fillWidth: true
                placeholderText: qsTr("/path/to/firmware.zip")
                text: root.controller.packagePath
                selectByMouse: true
                enabled: !root.controller.busy
                onTextEdited:
                    root.controller.setPackagePath(text)
            }

            Button {
                text: root.controller.validationBusy
                      ? qsTr("Validating…")
                      : qsTr("Validate")
                enabled: root.controller.canValidate
                onClicked: root.controller.validatePackage()
            }

            Button {
                text: qsTr("Refresh")
                enabled: root.controller.serviceAvailable &&
                         !root.controller.busy
                onClicked: root.controller.refresh()
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !root.controller.serviceAvailable
            text: qsTr("The background service must be running to validate or flash firmware.")
            color: "#efb85f"
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            visible: root.controller.canonicalPath.length > 0
            columns: 2
            columnSpacing: 18
            rowSpacing: 8

            Label {
                text: qsTr("Package")
                color: "#9ca4ac"
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.canonicalPath
                color: "#d8dde1"
                elide: Text.ElideMiddle
            }

            Label {
                text: qsTr("Type")
                color: "#9ca4ac"
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.kind
                color: "#d8dde1"
            }

            Label {
                text: qsTr("Size")
                color: "#9ca4ac"
            }
            Label {
                Layout.fillWidth: true
                text: root.formatBytes(root.controller.sizeBytes)
                color: "#d8dde1"
            }

            Label {
                text: qsTr("Product")
                color: "#9ca4ac"
                visible: root.controller.productCode.length > 0
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.productCode
                color: "#d8dde1"
                visible: root.controller.productCode.length > 0
            }

            Label {
                text: qsTr("Package version")
                color: "#9ca4ac"
                visible: root.controller.firmwareVersion.length > 0 ||
                         root.controller.appVersion.length > 0
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.firmwareVersion.length > 0
                      ? root.controller.firmwareVersion
                      : root.controller.appVersion
                color: "#d8dde1"
                visible: root.controller.firmwareVersion.length > 0 ||
                         root.controller.appVersion.length > 0
            }

            Label {
                text: qsTr("SHA-256")
                color: "#9ca4ac"
            }
            Label {
                Layout.fillWidth: true
                text: root.controller.sha256
                color: "#d8dde1"
                font.family: "monospace"
                elide: Text.ElideMiddle
            }
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: root.controller.busy
            from: 0
            to: 100
            value: root.controller.progress
            indeterminate: root.controller.validationBusy ||
                           (root.controller.flashBusy &&
                            root.controller.progress <= 0)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: root.phaseLabel(root.controller.phase)
                    color: root.controller.errorMessage.length > 0
                           ? "#ef6b73"
                           : (root.controller.approvalAvailable
                              ? "#66d18f" : "#d8dde1")
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: root.controller.errorMessage.length > 0
                          ? root.controller.errorMessage
                          : root.controller.status
                    color: root.controller.errorMessage.length > 0
                           ? "#ef6b73" : "#9ca4ac"
                    wrapMode: Text.WordWrap
                }
            }

            Button {
                visible: root.controller.flashBusy &&
                         root.controller.phase === "Flashing"
                text: qsTr("Request cancellation")
                onClicked: root.controller.requestCancel()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("The device may refuse cancellation after an irreversible flashing step")
            }

            PrimaryButton {
                text: qsTr("Flash firmware…")
                enabled: root.controller.canFlash
                onClicked:
                    root.controller.requestFlashConfirmation()
            }
        }
    }

    Dialog {
        id: confirmationDialog

        parent: Overlay.overlay
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0
        width: parent ? Math.min(620, parent.width - 48) : 620
        modal: true
        focus: true
        padding: 22
        closePolicy: Popup.NoAutoClose
        title: qsTr("Confirm firmware flash")

        background: Rectangle {
            radius: 12
            color: "#1b1f23"
            border.width: 1
            border.color: "#7f8d36"
        }

        contentItem: ColumnLayout {
            spacing: 16

            Label {
                Layout.fillWidth: true
                text: qsTr("This operation can make the display unusable if USB or power is interrupted. Cancellation is refused after the updater reaches an irreversible step.")
                color: "#f0d27a"
                font.bold: true
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Package: %1")
                      .arg(root.controller.canonicalPath)
                color: "#d8dde1"
                wrapMode: Text.WrapAnywhere
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("SHA-256: %1")
                      .arg(root.controller.sha256)
                color: "#9ca4ac"
                font.family: "monospace"
                wrapMode: Text.WrapAnywhere
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Item {
                    Layout.fillWidth: true
                }

                Button {
                    text: qsTr("Cancel")
                    onClicked:
                        root.controller.cancelFlashConfirmation()
                }

                PrimaryButton {
                    text: qsTr("Flash firmware")
                    enabled: root.controller.canFlash
                    onClicked: root.controller.confirmFlash()
                }
            }
        }
    }

    Connections {
        target: root.controller

        function onConfirmationRequiredChanged() {
            if (root.controller.confirmationRequired)
                confirmationDialog.open()
            else
                confirmationDialog.close()
        }
    }
}
