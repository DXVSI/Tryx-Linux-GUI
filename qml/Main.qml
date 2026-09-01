import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "components"
import "pages"

ApplicationWindow {
    id: window

    required property var runtime
    required property var mediaEditor
    required property var deviceMedia
    required property var firmware
    required property var systemMetrics
    required property var settings
    required property var windowChrome
    required property var supportBundle
    required property var cacheManagement
    required property bool quickSmokeTest
    required property bool autostartRequested

    width: 1420
    height: 900
    minimumWidth: 1060
    minimumHeight: 700
    visible: !quickSmokeTest && !autostartRequested
    title: qsTr("TRYX Panorama Manager")
    flags: Qt.Window | Qt.FramelessWindowHint

    Material.theme: Material.Dark
    Material.accent: "#def750"
    color: "#15181b"

    property int currentPage: 0
    property bool approvedCloseBypass: false
    property int guardedIntent: DirtyDraftGuard.NoIntent
    property var guardedTarget
    property string draftGuardMessage: ""
    readonly property bool displayLeaveGuardRequired:
        panoramaPage.hasUnsavedChanges || panoramaPage.applyPending ||
        panoramaPage.applyUnresolved

    Binding {
        target: window.systemMetrics
        property: "dashboardActive"
        value: window.visible &&
               window.visibility !== Window.Minimized &&
               window.currentPage === 0
    }

    function requestDirtyDraftIntent(intent, target) {
        if (!dirtyDraftGuard.request(intent, target))
            return false
        guardedIntent = intent
        guardedTarget = target
        draftGuardMessage = ""
        return true
    }

    function requestPage(targetPage) {
        if (guardedIntent !== DirtyDraftGuard.NoIntent)
            return
        if (targetPage < 0 || targetPage > 2 ||
            targetPage === currentPage) {
            return
        }
        if (displayLeaveGuardRequired) {
            requestDirtyDraftIntent(DirtyDraftGuard.RouteIntent,
                                    targetPage)
            return
        }
        currentPage = targetPage
    }

    function continueGuardedIntent(intent, target) {
        if (displayLeaveGuardRequired) {
            requestDirtyDraftIntent(intent, target)
            return
        }
        switch (intent) {
        case DirtyDraftGuard.RouteIntent:
            requestPage(target)
            break
        case DirtyDraftGuard.HideToTrayIntent:
            if (!windowChrome.hideWindowToTray()) {
                approvedCloseBypass = true
                Qt.callLater(function() {
                    windowChrome.closeWindow()
                })
            }
            break
        case DirtyDraftGuard.WindowCloseIntent:
            approvedCloseBypass = true
            windowChrome.closeWindow()
            break
        case DirtyDraftGuard.ExplicitQuitIntent:
            windowChrome.approveExplicitQuit()
            break
        }
    }

    function discardAndContinue(intent, target) {
        panoramaPage.discardChanges()
        if (panoramaPage.hasUnsavedChanges) {
            guardedIntent = DirtyDraftGuard.NoIntent
            guardedTarget = undefined
            draftGuardMessage =
                qsTr("The display draft could not be discarded.")
            Qt.callLater(function() {
                requestDirtyDraftIntent(intent, target)
            })
            return
        }

        guardedIntent = DirtyDraftGuard.NoIntent
        guardedTarget = undefined
        draftGuardMessage = ""
        Qt.callLater(function() {
            continueGuardedIntent(intent, target)
        })
    }

    function finishGuardedApply(outcome, message) {
        if (guardedIntent === DirtyDraftGuard.NoIntent)
            return
        if (outcome === "Succeeded" &&
            !panoramaPage.hasUnsavedChanges &&
            !panoramaPage.applyUnresolved) {
            const intent = guardedIntent
            const target = guardedTarget
            guardedIntent = DirtyDraftGuard.NoIntent
            guardedTarget = undefined
            draftGuardMessage = ""
            dirtyDraftGuard.complete()
            Qt.callLater(function() {
                continueGuardedIntent(intent, target)
            })
            return
        }

        if (outcome === "Succeeded") {
            draftGuardMessage = qsTr(
                "Display changes remain after Apply. Review them before continuing.")
        } else if (message && message.length > 0) {
            draftGuardMessage = message
        } else if (!panoramaPage.applyUnresolved) {
            draftGuardMessage = qsTr(
                "Display changes were not applied. Review the draft and try again.")
        }
    }

    function requestExplicitQuit() {
        if (guardedIntent !== DirtyDraftGuard.NoIntent)
            return
        if (displayLeaveGuardRequired) {
            requestDirtyDraftIntent(
                DirtyDraftGuard.ExplicitQuitIntent, -1)
            return
        }
        windowChrome.approveExplicitQuit()
    }

    onClosing: close => {
        if (approvedCloseBypass) {
            approvedCloseBypass = false
            if (!displayLeaveGuardRequired) {
                close.accepted = true
                return
            }
        }
        if (guardedIntent !== DirtyDraftGuard.NoIntent) {
            close.accepted = false
            return
        }
        if (!displayLeaveGuardRequired) {
            if (windowChrome.handleCloseRequest())
                close.accepted = false
            return
        }

        close.accepted = false
        const intent = windowChrome.closeWouldHideToTray()
                     ? DirtyDraftGuard.HideToTrayIntent
                     : DirtyDraftGuard.WindowCloseIntent
        requestDirtyDraftIntent(intent, -1)
    }

    readonly property string pageTitle:
        currentPage === 0 ? qsTr("Dashboard")
        : currentPage === 1 ? qsTr("Display")
                            : qsTr("Settings")
    readonly property string pageDescription:
        currentPage === 0
        ? qsTr("Device status and live metrics from this PC")
        : currentPage === 1
          ? qsTr("Manage media, layout and screen controls")
          : qsTr("Application, startup and device preferences")

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: titleBar

            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: "#171a1e"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                spacing: 10

                Image {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    source: "qrc:/icons/tryx-panorama.png"
                    fillMode: Image.PreserveAspectFit
                }

                Label {
                    text: qsTr("PANORAMA")
                    color: "#f4f6f7"
                    font.pixelSize: 15
                    font.bold: true
                    font.letterSpacing: 1
                }

                Item {
                    id: moveArea

                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    DragHandler {
                        target: null
                        acceptedButtons: Qt.LeftButton
                        onActiveChanged: {
                            if (active)
                                window.windowChrome.startMove()
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.LeftButton
                        onDoubleTapped:
                            window.windowChrome.toggleMaximized()
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: window.runtime.displaySessionActive
                           ? "#66d18f" : "#efb85f"
                }

                Label {
                    Layout.maximumWidth: 340
                    text: window.runtime.connectionStatus
                    color: "#aeb5bb"
                    elide: Text.ElideRight
                    font.pixelSize: 12
                }

                ToolButton {
                    id: minimizeButton

                    text: "−"
                    Accessible.name: qsTr("Minimize")
                    onClicked: window.windowChrome.minimize()
                    background: Rectangle {
                        color: minimizeButton.hovered
                               ? "#2a3036" : "transparent"
                    }
                }

                ToolButton {
                    id: maximizeButton

                    text: window.windowChrome.maximized ? "❐" : "□"
                    Accessible.name: window.windowChrome.maximized
                                     ? qsTr("Restore")
                                     : qsTr("Maximize")
                    onClicked:
                        window.windowChrome.toggleMaximized()
                    background: Rectangle {
                        color: maximizeButton.hovered
                               ? "#2a3036" : "transparent"
                    }
                }

                ToolButton {
                    id: closeButton

                    text: "×"
                    Accessible.name: qsTr("Close")
                    onClicked: window.windowChrome.closeWindow()
                    background: Rectangle {
                        color: closeButton.hovered
                               ? "#b43b43" : "transparent"
                    }
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: "#2d3338"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 214
                color: "#1b1f23"

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    Label {
                        Layout.leftMargin: 10
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        text: qsTr("CONTROL CENTER")
                        color: "#6f7880"
                        font.pixelSize: 10
                        font.bold: true
                        font.letterSpacing: 1
                    }

                    NavButton {
                        Layout.fillWidth: true
                        text: qsTr("Dashboard")
                        selected: window.currentPage === 0
                        onClicked: window.requestPage(0)
                    }

                    NavButton {
                        Layout.fillWidth: true
                        text: qsTr("Display")
                        selected: window.currentPage === 1
                        onClicked: window.requestPage(1)
                    }

                    Item { Layout.fillHeight: true }

                    NavButton {
                        Layout.fillWidth: true
                        text: qsTr("Settings")
                        selected: window.currentPage === 2
                        onClicked: window.requestPage(2)
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 70
                        radius: 10
                        color: "#16191c"
                        border.width: 1
                        border.color: "#30363c"

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 4

                            Label {
                                text: qsTr("DISPLAY SESSION")
                                color: "#737d85"
                                font.pixelSize: 9
                                font.bold: true
                            }

                            Label {
                                Layout.fillWidth: true
                                text: window.runtime.displaySessionActive
                                      ? qsTr("Ready")
                                      : qsTr("Waiting for device")
                                color: window.runtime.displaySessionActive
                                       ? "#66d18f" : "#efb85f"
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 76
                    color: "#1d2125"

                    ColumnLayout {
                        anchors.left: parent.left
                        anchors.leftMargin: 28
                        anchors.right: refreshButton.visible
                                       ? refreshButton.left : parent.right
                        anchors.rightMargin: refreshButton.visible ? 14 : 28
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Label {
                            text: window.pageTitle
                            color: "#f4f6f7"
                            font.pixelSize: 24
                            font.bold: true
                        }

                        Label {
                            text: window.pageDescription
                            color: "#8d969e"
                            font.pixelSize: 12
                        }
                    }

                    Button {
                        id: refreshButton

                        objectName: "pageRefreshButton"
                        anchors.right: parent.right
                        anchors.rightMargin: 28
                        anchors.verticalCenter: parent.verticalCenter
                        visible: window.currentPage !== 2
                        text: qsTr("Refresh")
                        enabled: !window.runtime.operationBusy
                        onClicked: {
                            window.runtime.refreshAll()
                            window.systemMetrics.refresh()
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: "#2d3338"
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: window.currentPage

                    HomePage {
                        runtime: window.runtime
                        systemMetrics: window.systemMetrics
                        onOpenDisplayRequested:
                            window.requestPage(1)
                    }

                    PanoramaPage {
                        id: panoramaPage
                        objectName: "panoramaPage"

                        runtime: window.runtime
                        editor: window.mediaEditor
                        deviceMedia: window.deviceMedia
                        onApplyFinished: (outcome, message) =>
                            window.finishGuardedApply(outcome, message)
                    }

                    SettingsPage {
                        runtime: window.runtime
                        settings: window.settings
                        firmware: window.firmware
                        windowChrome: window.windowChrome
                        supportBundle: window.supportBundle
                        cacheManagement: window.cacheManagement
                    }
                }
            }
        }
    }

    DirtyDraftGuard {
        id: dirtyDraftGuard

        applyEnabled: panoramaPage.canApplyChanges
        applying: panoramaPage.applyPending
        unresolved: panoramaPage.applyUnresolved
        unresolvedMessage:
            window.draftGuardMessage.length > 0
            ? window.draftGuardMessage
            : panoramaPage.applyBlockReason
        onApplyRequested: {
            window.draftGuardMessage = ""
            panoramaPage.applyChanges()
        }
        onDiscardRequested: (intent, target) =>
            window.discardAndContinue(intent, target)
        onStayRequested: {
            window.guardedIntent = DirtyDraftGuard.NoIntent
            window.guardedTarget = undefined
            window.draftGuardMessage = ""
        }
    }

    MediaEditor {
        id: editor
        controller: window.mediaEditor
    }

    Popup {
        id: toast

        parent: Overlay.overlay
        x: parent.width - width - 24
        y: parent.height - height - 24
        width: Math.min(520, parent.width - 48)
        padding: 14
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        property bool error: false
        property string message: ""

        background: Rectangle {
            radius: 8
            color: toast.error ? "#52252c" : "#213e33"
            border.color: toast.error ? "#ef6473" : "#43d58b"
        }

        contentItem: Label {
            text: toast.message
            color: "#ffffff"
            wrapMode: Text.WordWrap
        }
    }

    Connections {
        target: window.windowChrome
        function onExplicitQuitRequested() {
            window.requestExplicitQuit()
        }
    }

    Connections {
        target: window.runtime
        function onUserMessage(message, isError) {
            toast.message = message
            toast.error = isError
            toast.open()
        }
    }

    Connections {
        target: window.deviceMedia
        function onUserMessage(message, error) {
            toast.message = message
            toast.error = error
            toast.open()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: 1
        border.color: "#3a4147"
        visible: !window.windowChrome.maximized
        z: 999
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.LeftEdge
        cursorShape: Qt.SizeHorCursor
        width: 6
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 9
        anchors.bottomMargin: 9
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.RightEdge
        cursorShape: Qt.SizeHorCursor
        width: 6
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: 9
        anchors.bottomMargin: 9
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.TopEdge
        cursorShape: Qt.SizeVerCursor
        height: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 9
        anchors.rightMargin: 9
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.BottomEdge
        cursorShape: Qt.SizeVerCursor
        height: 6
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 9
        anchors.rightMargin: 9
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.LeftEdge | Qt.TopEdge
        cursorShape: Qt.SizeFDiagCursor
        width: 9
        height: 9
        anchors.left: parent.left
        anchors.top: parent.top
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.RightEdge | Qt.TopEdge
        cursorShape: Qt.SizeBDiagCursor
        width: 9
        height: 9
        anchors.right: parent.right
        anchors.top: parent.top
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.LeftEdge | Qt.BottomEdge
        cursorShape: Qt.SizeBDiagCursor
        width: 9
        height: 9
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        enabled: !window.windowChrome.maximized
    }

    WindowResizeHandle {
        chrome: window.windowChrome
        edges: Qt.RightEdge | Qt.BottomEdge
        cursorShape: Qt.SizeFDiagCursor
        width: 9
        height: 9
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        enabled: !window.windowChrome.maximized
    }
}
