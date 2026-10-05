pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase

    name: "DirtyDraftGuard"
    when: hostWindow.visible

    ApplicationWindow {
        id: hostWindow

        width: 900
        height: 700
        visible: true
    }

    Component {
        id: guardHostComponent

        Item {
            id: guardHost

            width: hostWindow.width
            height: hostWindow.height

            property bool guardApplyEnabled: true
            property bool guardApplying: false
            property bool guardUnresolved: false
            property string guardUnresolvedMessage: ""
            property int applyCount: 0
            property int discardCount: 0
            property int stayCount: 0
            property int lastIntent: -1
            property var lastTarget

            property alias guard: dirtyDraftGuard
            property alias focusOrigin: focusOrigin

            Button {
                id: focusOrigin

                objectName: "dirtyDraftFocusOrigin"
                text: "Focus origin"
                anchors.left: parent.left
                anchors.top: parent.top
            }

            Components.DirtyDraftGuard {
                id: dirtyDraftGuard

                objectName: "dirtyDraftGuard"
                applyEnabled: guardHost.guardApplyEnabled
                applying: guardHost.guardApplying
                unresolved: guardHost.guardUnresolved
                unresolvedMessage:
                    guardHost.guardUnresolvedMessage

                onApplyRequested: (intent, target) => {
                    guardHost.applyCount += 1
                    guardHost.lastIntent = intent
                    guardHost.lastTarget = target
                    guardHost.guardApplying = true
                }
                onDiscardRequested: (intent, target) => {
                    guardHost.discardCount += 1
                    guardHost.lastIntent = intent
                    guardHost.lastTarget = target
                }
                onStayRequested: {
                    guardHost.stayCount += 1
                }
            }
        }
    }

    function createGuardHost() {
        const host = createTemporaryObject(
            guardHostComponent, hostWindow.contentItem)
        verify(host !== null)
        wait(0)
        return host
    }

    function openGuard(host, intent, target) {
        host.guard.request(intent, target)
        tryVerify(() => host.guard.opened)
        return host.guard
    }

    function guardButton(guard, objectName) {
        const button = findChild(guard, objectName)
        verify(button !== null)
        return button
    }

    function init() {
        hostWindow.width = 900
        hostWindow.height = 700
        hostWindow.requestActivate()
        tryVerify(() => hostWindow.active)
        wait(0)
    }

    function test_firstIntentWinsUntilChoice() {
        const host = createGuardHost()
        const guard = openGuard(
            host, Components.DirtyDraftGuard.RouteIntent, 2)

        // A later close/quit request must not replace the route that opened
        // the prompt or cause a second prompt.
        guard.request(
            Components.DirtyDraftGuard.ExplicitQuitIntent, -1)
        wait(0)
        verify(guard.opened)

        const discard = guardButton(
            guard, "dirtyDraftDiscardButton")
        verify(discard.enabled)
        mouseClick(discard)
        tryVerify(() => !guard.opened)

        compare(host.discardCount, 1)
        compare(host.applyCount, 0)
        compare(host.stayCount, 0)
        compare(host.lastIntent,
                Components.DirtyDraftGuard.RouteIntent)
        compare(host.lastTarget, 2)
    }

    function test_stayCancelsIntentAndRestoresSafeFocus() {
        const host = createGuardHost()
        host.focusOrigin.forceActiveFocus()
        tryVerify(() => host.focusOrigin.activeFocus)

        const guard = openGuard(
            host, Components.DirtyDraftGuard.WindowCloseIntent,
            "window-close")
        const stay = guardButton(
            guard, "dirtyDraftStayButton")
        tryVerify(() => stay.activeFocus)

        mouseClick(stay)
        tryVerify(() => !guard.opened)
        tryVerify(() => host.focusOrigin.activeFocus)

        compare(host.stayCount, 1)
        compare(host.applyCount, 0)
        compare(host.discardCount, 0)
    }

    function test_escapeIsExactlyStay() {
        const host = createGuardHost()
        host.focusOrigin.forceActiveFocus()
        const guard = openGuard(
            host, Components.DirtyDraftGuard.ExplicitQuitIntent,
            "quit")
        const stay = guardButton(
            guard, "dirtyDraftStayButton")
        tryVerify(() => stay.activeFocus)

        keyClick(Qt.Key_Escape)
        tryVerify(() => !guard.opened)
        tryVerify(() => host.focusOrigin.activeFocus)
        compare(host.stayCount, 1)
        compare(host.applyCount, 0)
        compare(host.discardCount, 0)
    }

    function test_applyEmitsOnceAndLocksDestructiveChoices() {
        const host = createGuardHost()
        const guard = openGuard(
            host, Components.DirtyDraftGuard.HideToTrayIntent,
            "tray")
        const apply = guardButton(
            guard, "dirtyDraftApplyButton")
        const discard = guardButton(
            guard, "dirtyDraftDiscardButton")
        const stay = guardButton(
            guard, "dirtyDraftStayButton")

        verify(apply.enabled)
        verify(discard.enabled)
        mouseClick(apply)
        wait(0)

        compare(host.applyCount, 1)
        compare(host.lastIntent,
                Components.DirtyDraftGuard.HideToTrayIntent)
        compare(host.lastTarget, "tray")
        verify(guard.opened)
        verify(!apply.enabled)
        verify(!discard.enabled)
        verify(stay.enabled)

        mouseClick(apply)
        mouseClick(discard)
        wait(0)
        compare(host.applyCount, 1)
        compare(host.discardCount, 0)

        mouseClick(stay)
        tryVerify(() => !guard.opened)
        compare(host.stayCount, 1)
    }

    function test_completeClosesWithoutChoiceAndReleasesIntent() {
        const host = createGuardHost()
        host.focusOrigin.forceActiveFocus()
        tryVerify(() => host.focusOrigin.activeFocus)
        const guard = openGuard(
            host, Components.DirtyDraftGuard.RouteIntent, 2)

        guard.complete()
        tryVerify(() => !guard.opened)
        tryVerify(() => host.focusOrigin.activeFocus)
        compare(host.applyCount, 0)
        compare(host.discardCount, 0)
        compare(host.stayCount, 0)

        openGuard(
            host, Components.DirtyDraftGuard.ExplicitQuitIntent,
            "quit")
        const discard = guardButton(
            guard, "dirtyDraftDiscardButton")
        mouseClick(discard)
        tryVerify(() => !guard.opened)
        compare(host.discardCount, 1)
        compare(host.lastIntent,
                Components.DirtyDraftGuard.ExplicitQuitIntent)
        compare(host.lastTarget, "quit")
    }

    function test_unresolvedOutcomeFailsClosed() {
        const host = createGuardHost()
        host.guardApplyEnabled = false
        host.guardUnresolved = true
        host.guardUnresolvedMessage =
            "The device result could not be confirmed"
        const guard = openGuard(
            host, Components.DirtyDraftGuard.RouteIntent, 0)
        const apply = guardButton(
            guard, "dirtyDraftApplyButton")
        const discard = guardButton(
            guard, "dirtyDraftDiscardButton")
        const stay = guardButton(
            guard, "dirtyDraftStayButton")
        const status = findChild(
            guard, "dirtyDraftStatus")

        verify(status !== null)
        verify(status.visible)
        compare(status.Accessible.role, Accessible.AlertMessage)
        verify(status.text.indexOf(
                   host.guardUnresolvedMessage) >= 0)
        verify(!apply.enabled)
        verify(discard.enabled)
        verify(stay.enabled)

        mouseClick(discard)
        tryVerify(() => !guard.opened)
        compare(host.discardCount, 1)
        compare(host.lastIntent,
                Components.DirtyDraftGuard.RouteIntent)
        compare(host.lastTarget, 0)
        compare(host.stayCount, 0)
        compare(host.applyCount, 0)
    }

    function test_narrowDialogReflowsAndRemainsAccessible() {
        hostWindow.width = 320
        hostWindow.height = 640
        wait(0)
        const host = createGuardHost()
        const guard = openGuard(
            host, Components.DirtyDraftGuard.RouteIntent, 2)
        const actions = findChild(
            guard, "dirtyDraftActionGrid")
        const stay = guardButton(
            guard, "dirtyDraftStayButton")
        const discard = guardButton(
            guard, "dirtyDraftDiscardButton")
        const apply = guardButton(
            guard, "dirtyDraftApplyButton")

        verify(guard.width <= hostWindow.width - 32)
        verify(guard.height <= hostWindow.height - 32)
        verify(actions !== null)
        compare(actions.columns, 1)
        verify(actions.width <= guard.contentItem.width,
               "actions width " + actions.width
               + ", content width " + guard.contentItem.width
               + ", parent width " + actions.parent.width)

        const buttons = [stay, discard, apply]
        for (const button of buttons) {
            verify(button.visible)
            verify(button.width <= actions.width)
            verify(button.height >= 44)
            verify(button.activeFocusOnTab)
            verify(button.Accessible.name.length > 0)
            verify(button.Accessible.description.length > 0)
        }

        compare(guard.Accessible.role, Accessible.Dialog)
        verify(guard.Accessible.name.length > 0)
        verify(guard.Accessible.description.length > 0)
    }
}
