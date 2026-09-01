pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

GroupBox {
    id: root

    required property string sectionTitle
    required property string fieldPrefix
    required property string placement
    required property string textColor
    required property string alignment

    property bool activeForMode: true
    property bool showPlacement: true
    readonly property int placementOptionIndex:
        placementOptions.findIndex(item => item.value === placement)
    readonly property int alignmentOptionIndex:
        alignmentOptions.findIndex(item => item.value === alignment)
    readonly property bool valid:
        placementOptionIndex >= 0 &&
        alignmentOptionIndex >= 0 &&
        colorField.acceptableInput
    readonly property bool validationErrorVisible: !valid
    readonly property var placementOptions: [
        {"value": "Top", "label": qsTr("Top")},
        {"value": "Bottom", "label": qsTr("Bottom")}
    ]
    readonly property var alignmentOptions: [
        {"value": "Left", "label": qsTr("Left")},
        {"value": "Center", "label": qsTr("Center")},
        {"value": "Right", "label": qsTr("Right")}
    ]

    signal placementEdited(string value)
    signal textColorEdited(string value)
    signal alignmentEdited(string value)

    title: sectionTitle
    implicitWidth: 280
    Layout.minimumWidth: 0
    visible: activeForMode

    GridLayout {
        anchors.fill: parent
        columns: 2
        columnSpacing: 10
        rowSpacing: 8

        Label {
            visible: root.showPlacement
            text: qsTr("Position")
        }
        ComboBox {
            objectName: root.fieldPrefix + "StylePlacementCombo"
            Layout.fillWidth: true
            visible: root.showPlacement
            activeFocusOnTab: true
            model: root.placementOptions
            textRole: "label"
            currentIndex: root.placementOptionIndex
            Accessible.name: qsTr("%1 position")
                                 .arg(root.sectionTitle)
            onActivated: index => root.placementEdited(
                root.placementOptions[index].value)
        }

        Label {
            text: qsTr("Alignment")
        }
        ComboBox {
            objectName: root.fieldPrefix + "StyleAlignmentCombo"
            Layout.fillWidth: true
            activeFocusOnTab: true
            model: root.alignmentOptions
            textRole: "label"
            currentIndex: root.alignmentOptionIndex
            Accessible.name: qsTr("%1 alignment")
                                 .arg(root.sectionTitle)
            onActivated: index => root.alignmentEdited(
                root.alignmentOptions[index].value)
        }

        Label {
            text: qsTr("Text color")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            TextField {
                id: colorField

                objectName: root.fieldPrefix + "StyleColorField"
                Layout.fillWidth: true
                activeFocusOnTab: true
                text: root.textColor
                placeholderText: "#dcdcdc"
                selectByMouse: true
                validator: RegularExpressionValidator {
                    regularExpression: /^#[0-9A-Fa-f]{6}$/
                }
                Accessible.name: qsTr("%1 text color")
                                     .arg(root.sectionTitle)
                Accessible.description:
                    qsTr("Enter an exact six-digit hexadecimal RGB color")
                onTextEdited: root.textColorEdited(text)
            }

            Button {
                objectName: root.fieldPrefix + "StyleColorButton"
                Layout.preferredWidth: 44
                Layout.preferredHeight: 36
                activeFocusOnTab: true
                Accessible.name: qsTr("Choose %1 text color")
                                     .arg(root.sectionTitle)
                onClicked: {
                    colorDialog.selectedColor =
                        colorField.acceptableInput
                        ? colorField.text
                        : "#dcdcdc"
                    colorDialog.open()
                }

                contentItem: Rectangle {
                    radius: 4
                    color: colorField.acceptableInput
                           ? colorField.text
                           : "#dcdcdc"
                    border.width: 1
                    border.color: "#aeb4bd"
                }
            }
        }

        Item {
            Layout.preferredWidth: 1
            Layout.preferredHeight: colorError.visible
                                    ? colorError.implicitHeight
                                    : 0
        }
        Label {
            id: colorError

            objectName: root.fieldPrefix + "StyleColorError"
            Layout.fillWidth: true
            visible: !root.valid
            text: !colorField.acceptableInput
                  ? qsTr("Use exact #RRGGBB format.")
                  : root.placementOptionIndex < 0
                    ? qsTr("Unsupported position value.")
                    : qsTr("Unsupported alignment value.")
            color: "#ff8f8f"
            wrapMode: Text.WordWrap
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
    }

    ColorDialog {
        id: colorDialog

        title: qsTr("Choose %1 text color").arg(root.sectionTitle)
        onAccepted: {
            const value = selectedColor.toString()
            const rgb = value.length === 9
                        ? "#" + value.slice(3)
                        : value
            root.textColorEdited(rgb)
        }
    }
}
