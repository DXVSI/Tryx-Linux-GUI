pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest

import "../../../qml/components" as Components

TestCase {
    id: testCase

    name: "MetricSelector"
    when: hostWindow.visible

    readonly property var catalog: [
        "CPU Temperature", "CPU Frequency", "CPU Usage", "CPU Power",
        "GPU Temperature", "GPU Frequency", "GPU Usage", "GPU Power",
        "Memory Frequency", "Memory Usage", "Date&Time"
    ]
    property var activeHost

    ApplicationWindow {
        id: hostWindow

        width: 760
        height: 1100
        visible: true
    }

    Component {
        id: selectorHostComponent

        Item {
            id: selectorHost

            width: hostWindow.width
            height: hostWindow.height
            property var selected: []
            property var available: testCase.catalog.slice(0)
            property bool catalogReady: true
            property int editCount: 0
            property alias selector: selector

            Components.MetricSelector {
                id: selector

                width: parent.width
                areaTitle: qsTr("Full")
                fieldPrefix: "full"
                catalog: testCase.catalog
                catalogReady: selectorHost.catalogReady
                availableMetrics: selectorHost.available
                selectedMetrics: selectorHost.selected
                editable: true
                onSelectionEdited: metrics => {
                    selectorHost.selected = metrics
                    selectorHost.editCount += 1
                }
            }
        }
    }

    function createSelector(selected, available) {
        const host = createTemporaryObject(
            selectorHostComponent, hostWindow.contentItem,
            {"selected": selected || [],
             "available": available || catalog.slice(0)})
        verify(host !== null)
        activeHost = host
        wait(0)
        return host
    }

    function child(selector, objectName) {
        const item = findChild(selector, objectName)
        verify(item !== null, "Missing " + objectName)
        return item
    }

    function init() {
        hostWindow.width = 760
        hostWindow.height = 1100
        hostWindow.requestActivate()
        tryVerify(() => hostWindow.active)
    }

    function cleanup() {
        if (activeHost && activeHost.selector) {
            activeHost.selector.cancelReplacement()
            tryVerify(() => !activeHost.selector.replacementVisible)
        }
        activeHost = null
        wait(0)
    }

    function test_groupsCounterAndLocalizedSearch() {
        const host = createSelector([], catalog)
        const selector = host.selector
        const counter = child(selector, "fullMetricCounter")
        verify(counter.text.endsWith("0/3"))
        compare(counter.Accessible.name, counter.text)
        verify(child(selector, "fullMetricGroupCPU").visible)
        verify(child(selector, "fullMetricGroupGPU").visible)
        verify(child(selector, "fullMetricGroupMemory").visible)
        verify(child(selector, "fullMetricGroupSystem").visible)

        const search = child(selector, "fullMetricSearch")
        search.text = selector.metricLabel("Memory Usage")
        wait(0)
        compare(selector.visibleMetricTokens, ["Memory Usage"])
        verify(counter.text.endsWith("0/3"))
    }

    function test_selectedUnavailableRemainsVisibleAndRemovable() {
        const host = createSelector(
            ["GPU Power"], ["CPU Temperature"])
        const selector = host.selector
        const metric = child(selector, "fullMetricGpuPower")
        verify(metric.checked)
        verify(metric.enabled)
        verify(metric.text !== selector.metricLabel("GPU Power"))
        compare(metric.Accessible.name, metric.text)
        verify(metric.Accessible.description.length > 0)
        verify(child(selector, "fullMetricCounter").text.endsWith("1/3"))
        const scenePosition = metric.mapToItem(
            hostWindow.contentItem, 0, 0)
        verify(scenePosition.y >= 0 &&
               scenePosition.y + metric.height <= hostWindow.height,
               "Selected unavailable metric is outside the test window: " +
               scenePosition.y + "+" + metric.height)

        metric.forceActiveFocus()
        tryVerify(() => metric.activeFocus)
        keyClick(Qt.Key_Space)
        tryCompare(host, "selected", [])
        compare(host.editCount, 1)
    }

    function test_unselectedUnavailableIsDisabledAndAccessible() {
        const host = createSelector([], ["CPU Temperature"])
        const metric = child(
            host.selector, "fullMetricGpuPower")
        verify(!metric.checked)
        verify(!metric.enabled)
        verify(metric.height >= 44)
        verify(metric.Accessible.name.length > 0)
        verify(metric.Accessible.description.length > 0)
    }

    function test_catalogUnavailableStillShowsAndRemovesSelection() {
        const host = createSelector(["GPU Power"], [])
        host.catalogReady = false
        wait(0)
        const metric = child(
            host.selector, "fullMetricGpuPower")
        verify(metric.visible)
        verify(metric.checked)
        verify(metric.enabled)
        metric.forceActiveFocus()
        tryVerify(() => metric.activeFocus)
        keyClick(Qt.Key_Space)
        tryCompare(host, "selected", [])
        compare(host.editCount, 1)
    }

    function test_fourthChoiceWaitsForExplicitPosition() {
        const selected = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        const fourth = child(selector, "fullMetricGpuTemperature")
        fourth.forceActiveFocus()
        selector.requestToggle("GPU Temperature", fourth)

        compare(host.selected, selected)
        compare(host.editCount, 0)
        tryVerify(() => selector.replacementOpen)
        compare(child(selector, "fullMetricCounter").text, "Full 3/3")

        const position = selector.replacementItemAt(1)
        verify(position !== null)
        mouseClick(position)
        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, [
            "CPU Temperature", "GPU Temperature", "CPU Usage"
        ])
        compare(host.editCount, 1)
    }

    function test_replacementUsesExactPosition_data() {
        return [
            {"tag": "first", "position": 0,
             "expected": ["GPU Temperature", "CPU Frequency",
                          "CPU Usage"]},
            {"tag": "last", "position": 2,
             "expected": ["CPU Temperature", "CPU Frequency",
                          "GPU Temperature"]}
        ]
    }

    function test_replacementUsesExactPosition(data) {
        const selected = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        selector.requestToggle("GPU Temperature", null)
        tryVerify(() => selector.replacementOpen)
        compare(host.selected, selected)
        compare(host.editCount, 0)

        const position = selector.replacementItemAt(data.position)
        verify(position !== null)
        mouseClick(position)
        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, data.expected)
        compare(host.editCount, 1)
    }

    function test_escapeCancelsAndRestoresOriginFocus() {
        const selected = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        const fourth = child(selector, "fullMetricGpuTemperature")
        // Focus can be assigned before the first frame. Mouse input also
        // requires a rendered target when this runs in the full QML suite.
        verify(waitForRendering(fourth, 1000))
        fourth.forceActiveFocus()
        tryVerify(() => fourth.activeFocus)
        mouseClick(fourth)
        tryVerify(() => selector.replacementOpen)

        keyClick(Qt.Key_Escape)
        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, selected)
        compare(host.editCount, 0)
        verify(!fourth.checked,
               "Escaped pending metric must not remain visually selected")
        tryVerify(() => fourth.activeFocus)
    }

    function test_keyboardReplacementTraversesAndConfirmsExactPosition() {
        const selected = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        const fourth = child(selector, "fullMetricGpuTemperature")
        fourth.forceActiveFocus()
        keyClick(Qt.Key_Space)
        tryVerify(() => selector.replacementOpen)

        const first = selector.replacementItemAt(0)
        const second = selector.replacementItemAt(1)
        verify(first !== null)
        verify(second !== null)
        tryVerify(() => first.activeFocus)
        keyClick(Qt.Key_Tab)
        tryVerify(() => second.activeFocus)
        keyClick(Qt.Key_Space)

        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, [
            "CPU Temperature", "GPU Temperature", "CPU Usage"
        ])
        compare(host.editCount, 1)
        tryVerify(() => fourth.activeFocus)
    }

    function test_cancelButtonAndProgrammaticCloseDoNotEdit() {
        const selected = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        const fourth = child(selector, "fullMetricGpuTemperature")
        selector.requestToggle("GPU Temperature", null)
        tryVerify(() => selector.replacementOpen)
        const cancel = selector.replacementCancelItem()
        verify(cancel !== null)
        mouseClick(cancel)
        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, selected)
        compare(host.editCount, 0)

        fourth.forceActiveFocus()
        tryVerify(() => fourth.activeFocus)
        keyClick(Qt.Key_Space)
        tryVerify(() => selector.replacementOpen)
        selector.cancelReplacement()
        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, selected)
        compare(host.editCount, 0)
        verify(!fourth.checked,
               "Cancelled pending metric must not remain visually selected")
    }

    function test_keyboardSpaceSelectsAvailableMetric() {
        const host = createSelector([], catalog)
        const metric = child(
            host.selector, "fullMetricCpuTemperature")
        metric.forceActiveFocus()
        tryVerify(() => metric.activeFocus)
        keyClick(Qt.Key_Space)
        tryCompare(host, "selected", ["CPU Temperature"])
        compare(host.editCount, 1)
        verify(metric.Accessible.description.length > 0)
    }

    function test_confirmRevalidatesLiveAvailability() {
        const selected = [
            "CPU Temperature", "CPU Frequency", "CPU Usage"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        selector.requestToggle("GPU Temperature", null)
        tryVerify(() => selector.replacementOpen)

        host.available = catalog.filter(
            token => token !== "GPU Temperature")
        wait(0)
        const position = selector.replacementItemAt(0)
        verify(position !== null)
        mouseClick(position)
        tryVerify(() => !selector.replacementOpen)
        compare(host.selected, selected)
        compare(host.editCount, 0)
    }

    function test_narrowReplacementDialogBoundsLongLocalizedLabels() {
        hostWindow.width = 300
        hostWindow.height = 480
        const selected = [
            "CPU Temperature", "Memory Frequency", "Date&Time"
        ]
        const host = createSelector(selected, catalog)
        const selector = host.selector
        selector.requestToggle("GPU Temperature", null)
        tryVerify(() => selector.replacementOpen)

        const dialog = findChild(
            hostWindow.contentItem, "fullMetricReplacementDialog")
        verify(dialog !== null)
        verify(dialog.width <= hostWindow.width - 32)
        verify(dialog.height <= hostWindow.height - 32)
        verify(dialog.contentItem.Accessible.name.length > 0)
        verify(dialog.contentItem.Accessible.description.length > 0)
        for (let index = 0; index < 3; ++index) {
            const position = selector.replacementItemAt(index)
            verify(position !== null)
            verify(position.width <= dialog.availableWidth)
            verify(position.height >= 44)
            verify(position.Accessible.name.length > 0)
            verify(position.Accessible.description.length > 0)
        }
    }

    function test_narrowWidthDoesNotRequireHorizontalLayout() {
        hostWindow.width = 240
        const host = createSelector([], ["CPU Temperature"])
        host.selector.width = 220
        wait(0)
        verify(host.selector.layoutContentWidth <=
               host.selector.availableWidth)
        verify(child(host.selector, "fullMetricSearch").width <=
               host.selector.availableWidth)
        for (const token of host.selector.visibleMetricTokens) {
            const definition = host.selector.metricDefinition(token)
            const metric = child(
                host.selector, "fullMetric" + definition.suffix)
            verify(metric.width <= host.selector.availableWidth,
                   metric.objectName + " overflows narrow selector")
        }
    }
}
