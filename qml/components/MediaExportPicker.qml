pragma ComponentBehavior: Bound

import Qt.labs.folderlistmodel
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    objectName: "mediaExportPicker"

    required property var workflow
    required property url homeFolder

    property url currentFolder: homeFolder
    property string mediaId: ""
    property string mediaName: ""
    property alias fileName: fileNameField.text

    function openFor(targetMediaId, targetMediaName) {
        mediaId = targetMediaId
        mediaName = targetMediaName
        fileName = workflow.suggestedExportFileName(targetMediaName)
        if (currentFolder.toString().length === 0)
            currentFolder = homeFolder
        open()
    }

    function navigate(folderUrl) {
        currentFolder = folderUrl
    }

    function exportCopy() {
        if (mediaId.length === 0 || fileName.trim().length === 0)
            return
        const targetId = mediaId
        const targetName = mediaName
        const folder = currentFolder
        const destinationName = fileName.trim()
        close()
        Qt.callLater(() => root.workflow.beginExport(
                         targetId, targetName,
                         folder, destinationName))
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: parent
           ? Math.min(760, Math.max(520, parent.width - 48))
           : 760
    height: parent
            ? Math.min(680, Math.max(480, parent.height - 48))
            : 680
    modal: true
    focus: true
    padding: 20
    closePolicy: Popup.CloseOnEscape

    onClosed: {
        mediaId = ""
        mediaName = ""
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
        showFiles: false
        showDirs: true
        showDirsFirst: true
        showDotAndDotDot: false
        showHidden: hiddenFolders.checked
        showOnlyReadable: true
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
                    text: qsTr("Export device media copy")
                    color: "#f4f6f7"
                    font.pixelSize: 20
                    font.bold: true
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Choose a local folder and H.264 file name")
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
                onClicked: root.navigate(root.homeFolder)
            }

            Button {
                text: qsTr("Up")
                enabled: folderModel.parentFolder.toString().length > 0 &&
                         folderModel.parentFolder.toString() !==
                         root.currentFolder.toString()
                onClicked: root.navigate(folderModel.parentFolder)
            }

            Item { Layout.fillWidth: true }

            CheckBox {
                id: hiddenFolders
                text: qsTr("Show hidden folders")
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
                objectName: "mediaExportFolderList"
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: folderModel
                spacing: 2

                ScrollBar.vertical: ScrollBar {}

                delegate: ItemDelegate {
                    id: folderDelegate

                    required property string fileName
                    required property url fileUrl
                    required property bool fileIsDir

                    width: ListView.view.width
                    height: 52
                    visible: fileIsDir

                    onClicked:
                        root.navigate(folderDelegate.fileUrl)

                    contentItem: RowLayout {
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 54
                            Layout.preferredHeight: 28
                            radius: 5
                            color: "#30372d"
                            border.width: 1
                            border.color: "#7b883c"

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("DIR")
                                color: "#def750"
                                font.pixelSize: 9
                                font.bold: true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: folderDelegate.fileName
                            color: "#eef1f3"
                            elide: Text.ElideMiddle
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
                text: qsTr("This folder has no subfolders")
                color: "#9da5ac"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                text: qsTr("File name")
            }

            TextField {
                id: fileNameField

                objectName: "mediaExportFileName"
                Layout.fillWidth: true
                placeholderText: qsTr("device-media-copy.h264")
                selectByMouse: true
                onAccepted: root.exportCopy()
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("The exported file is the device-ready raw H.264 copy.")
            color: "#8f989f"
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

            PrimaryButton {
                objectName: "mediaExportSaveButton"
                text: qsTr("Export")
                enabled: root.mediaId.length > 0 &&
                         root.fileName.trim().length > 0
                onClicked: root.exportCopy()
            }
        }
    }
}
