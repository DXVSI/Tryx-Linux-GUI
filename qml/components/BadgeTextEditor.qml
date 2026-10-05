pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property var runtime
    required property string badgeTitle
    required property string fieldPrefix
    required property var choice
    property bool editable: true
    readonly property bool custom: choice.mode === "Custom"
    readonly property string validationError: custom && "badgeTextError" in runtime
        ? runtime.badgeTextError(choice.mode, choice.text) : ""
    signal choiceEdited(string mode, string text)

    Layout.fillWidth: true
    Layout.minimumWidth: 0
    spacing: 6
    enabled: editable

    Label {
        text: root.badgeTitle
        textFormat: Text.PlainText
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
    }
    ComboBox {
        objectName: root.fieldPrefix + "BadgeMode"
        Layout.fillWidth: true
        activeFocusOnTab: true
        model: [qsTr("Automatic"), qsTr("Custom text")]
        currentIndex: root.custom ? 1 : 0
        Accessible.name: qsTr("%1 text source").arg(root.badgeTitle)
        onActivated: index => root.choiceEdited(index === 1 ? "Custom" : "Auto", "")
    }
    TextField {
        objectName: root.fieldPrefix + "BadgeTextField"
        Layout.fillWidth: true
        Layout.minimumWidth: 0
        visible: root.custom
        activeFocusOnTab: true
        selectByMouse: true
        text: root.choice.text
        placeholderText: qsTr("1-32 characters")
        Accessible.name: qsTr("%1 custom text").arg(root.badgeTitle)
        Accessible.description: root.validationError.length > 0
            ? root.validationError : qsTr("Plain text, applied with the display layout")
        onTextEdited: root.choiceEdited("Custom", text)
    }
    Label {
        objectName: root.fieldPrefix + "BadgeTextError"
        Layout.fillWidth: true
        visible: root.validationError.length > 0
        text: root.validationError
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: "#ffb4ab"
    }
}
