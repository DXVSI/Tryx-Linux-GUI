pragma ComponentBehavior: Bound

import Qt.labs.folderlistmodel
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    objectName: "firmwareFilePicker"

    required property var controller

    property url currentFolder: controller.homeFolder
    property url selectedFile: ""
    property string selectedName: ""

    function openPicker() {
        selectedFile = ""
        selectedName = ""
        if (String(currentFolder).length === 0)
            currentFolder = controller.homeFolder
        open()
    }

    function navigate(folderUrl) {
        selectedFile = ""
        selectedName = ""
        currentFolder = folderUrl
    }

    function acceptSelection() {
        if (String(selectedFile).length === 0)
            return
        controller.setPackagePath(String(selectedFile))
        close()
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: parent
           ? Math.min(820, Math.max(520, parent.width - 48))
           : 820
    height: parent
            ? Math.min(680, Math.max(480, parent.height - 48))
            : 680
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        radius: 12
        color: "#1b1f23"
        border.width: 1
        border.color: "#4a535a"
    }

    FolderListModel {
        id: folderModel

        folder: root.currentFolder
        nameFilters: ["*.zip"]
        showFiles: true
        showDirs: true
        showDirsFirst: true
        showDotAndDotDot: false
        showHidden: hiddenFiles.checked
        showOnlyReadable: true
        caseSensitive: false
        sortField: FolderListModel.Name
        sortCaseSensitive: false
    }

    contentItem: ColumnLayout {
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Select firmware package")
                    color: "#f4f6f7"
                    font.pixelSize: 20
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Choose one local ZIP package to validate")
                    color: "#9da5ac"
                    elide: Text.ElideRight
                }
            }

            ToolButton {
                text: "×"
                Accessible.name: qsTr("Close")
                onClicked: root.close()
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: 48

            background: Rectangle {
                radius: 7
                color: "#15181b"
                border.width: 1
                border.color: "#363d43"
            }

            Label {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                text: String(root.currentFolder)
                color: "#c7cdd2"
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                text: qsTr("Home")
                onClicked: root.navigate(root.controller.homeFolder)
            }

            Button {
                text: qsTr("Up")
                enabled: String(folderModel.parentFolder).length > 0 &&
                         String(folderModel.parentFolder) !==
                         String(root.currentFolder)
                onClicked: root.navigate(folderModel.parentFolder)
            }

            Item { Layout.fillWidth: true }

            CheckBox {
                id: hiddenFiles
                text: qsTr("Show hidden files")
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.fillHeight: true

            background: Rectangle {
                radius: 8
                color: "#15181b"
                border.width: 1
                border.color: "#363d43"
            }

            ListView {
                objectName: "firmwareFilePickerList"
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: folderModel
                currentIndex: -1
                spacing: 2

                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: fileDelegate

                    required property string fileName
                    required property url fileUrl
                    required property double fileSize
                    required property bool fileIsDir

                    width: ListView.view.width
                    height: 54
                    highlighted:
                        !fileDelegate.fileIsDir &&
                        String(root.selectedFile) ===
                        String(fileDelegate.fileUrl)

                    onClicked: {
                        if (fileDelegate.fileIsDir) {
                            root.navigate(fileDelegate.fileUrl)
                            return
                        }
                        root.selectedFile = fileDelegate.fileUrl
                        root.selectedName = fileDelegate.fileName
                    }
                    onDoubleClicked: {
                        if (fileDelegate.fileIsDir) {
                            root.navigate(fileDelegate.fileUrl)
                            return
                        }
                        root.selectedFile = fileDelegate.fileUrl
                        root.selectedName = fileDelegate.fileName
                        root.acceptSelection()
                    }

                    contentItem: RowLayout {
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 54
                            Layout.preferredHeight: 28
                            radius: 5
                            color: fileDelegate.fileIsDir
                                   ? "#30372d" : "#252b30"
                            border.width: 1
                            border.color: fileDelegate.fileIsDir
                                          ? "#7b883c" : "#41494f"

                            Label {
                                anchors.centerIn: parent
                                text: fileDelegate.fileIsDir
                                      ? qsTr("DIR") : qsTr("ZIP")
                                color: fileDelegate.fileIsDir
                                       ? "#def750" : "#aeb5bb"
                                font.pixelSize: 9
                                font.bold: true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: fileDelegate.fileName
                            color: "#eef1f3"
                            elide: Text.ElideMiddle
                        }

                        Label {
                            visible: !fileDelegate.fileIsDir
                            Layout.preferredWidth: 96
                            text: qsTr("%1 MiB").arg(
                                      (fileDelegate.fileSize /
                                       1048576).toFixed(1))
                            color: "#8f989f"
                            horizontalAlignment: Text.AlignRight
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: folderModel.status === FolderListModel.Null
                text: qsTr("This folder cannot be opened")
                color: "#efb85f"
            }

            Label {
                anchors.centerIn: parent
                visible: folderModel.status === FolderListModel.Loading
                text: qsTr("Loading folder…")
                color: "#9da5ac"
            }

            Label {
                anchors.centerIn: parent
                visible: folderModel.status === FolderListModel.Ready &&
                         folderModel.count === 0
                text: qsTr("No firmware ZIP files in this folder")
                color: "#9da5ac"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: String(root.selectedFile).length > 0
                      ? qsTr("Selected: %1").arg(root.selectedName)
                      : qsTr("Select a ZIP package to continue")
                color: "#9da5ac"
                elide: Text.ElideMiddle
            }

            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            PrimaryButton {
                objectName: "firmwarePickerSelectButton"
                text: qsTr("Select")
                enabled: String(root.selectedFile).length > 0
                onClicked: root.acceptSelection()
            }
        }
    }
}
