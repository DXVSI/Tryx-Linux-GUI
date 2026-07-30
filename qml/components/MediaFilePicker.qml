pragma ComponentBehavior: Bound

import Qt.labs.folderlistmodel
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    objectName: "mediaFilePicker"

    required property var editor

    property url currentFolder: editor.homeFolder
    property url selectedFile: ""
    property string selectedName: ""
    property url pendingSource: ""

    function clearSelection() {
        selectedFile = ""
        selectedName = ""
    }

    function openPicker() {
        clearSelection()
        pendingSource = ""
        if (currentFolder.toString().length === 0)
            currentFolder = editor.homeFolder
        open()
    }

    function navigate(folderUrl) {
        clearSelection()
        currentFolder = folderUrl
    }

    function selectEntry(fileUrl, isDirectory, fileName) {
        if (isDirectory) {
            navigate(fileUrl)
            return
        }
        selectedFile = fileUrl
        selectedName = fileName
    }

    function acceptSelection() {
        if (selectedFile.toString().length === 0)
            return
        pendingSource = selectedFile
        close()
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: parent
           ? Math.min(920, Math.max(520, parent.width - 48))
           : 920
    height: parent
            ? Math.min(720, Math.max(500, parent.height - 48))
            : 720
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape

    onClosed: {
        const source = pendingSource
        pendingSource = ""
        clearSelection()
        if (source.toString().length > 0)
            Qt.callLater(() => root.editor.begin(source))
    }

    background: Rectangle {
        radius: 12
        color: "#1b1f23"
        border.width: 1
        border.color: "#4a535a"
    }

    FolderListModel {
        id: folderModel

        folder: root.currentFolder
        nameFilters: [
            "*.mp4", "*.webm", "*.mkv", "*.avi", "*.mov",
            "*.gif", "*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp"
        ]
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
            objectName: "mediaFilePickerHeader"
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Select media file")
                    color: "#f4f6f7"
                    font.pixelSize: 20
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Choose one image, GIF or video to edit before upload")
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
                text: root.currentFolder.toString()
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
                onClicked: root.navigate(root.editor.homeFolder)
            }

            Button {
                text: qsTr("Up")
                enabled: folderModel.parentFolder.toString().length > 0 &&
                         folderModel.parentFolder.toString() !==
                         root.currentFolder.toString()
                onClicked: root.navigate(folderModel.parentFolder)
            }

            Item {
                Layout.fillWidth: true
            }

            CheckBox {
                id: hiddenFiles
                text: qsTr("Show hidden files")
                onCheckedChanged: root.clearSelection()
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
                id: fileList

                objectName: "mediaFilePickerList"
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
                        root.selectedFile.toString() ===
                        fileDelegate.fileUrl.toString()

                    onClicked:
                        root.selectEntry(
                            fileDelegate.fileUrl,
                            fileDelegate.fileIsDir,
                            fileDelegate.fileName)
                    onDoubleClicked: {
                        root.selectEntry(
                            fileDelegate.fileUrl,
                            fileDelegate.fileIsDir,
                            fileDelegate.fileName)
                        if (!fileDelegate.fileIsDir)
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
                                      ? qsTr("DIR") : qsTr("FILE")
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
                            text: qsTr("%1 MB").arg(
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
                text: qsTr("No supported media files in this folder")
                color: "#9da5ac"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: root.selectedFile.toString().length > 0
                      ? qsTr("Selected: %1").arg(
                            root.selectedName.length > 0
                            ? root.selectedName
                            : decodeURIComponent(
                                  root.selectedFile.toString()
                                      .split("/").pop()))
                      : qsTr("Select a media file to continue")
                color: root.selectedFile.toString().length > 0
                       ? "#c7cdd2" : "#7f8990"
                elide: Text.ElideMiddle
            }

            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            PrimaryButton {
                objectName: "mediaFilePickerOpenButton"
                text: qsTr("Open")
                enabled: root.selectedFile.toString().length > 0
                onClicked: root.acceptSelection()
            }
        }
    }
}
