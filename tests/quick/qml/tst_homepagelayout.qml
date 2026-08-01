pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import "../../../qml/pages" as Pages

TestCase {
    id: testCase
    name: "HomePageLayout"
    when: windowShown
    width: 1250
    height: 850

    QtObject {
        id: runtimeMock

        property bool displaySessionActive: true
        property string connectionStatus: "Ready"
        property string currentScreenMode: "Full Screen"
        property string currentPlayMode: "Single"
        property var displayedMedia: ["sample.mp4"]
        property bool displayStateValid: true
        property int brightness: 75
        property string diagnostic: ""
    }

    QtObject {
        id: metricsMock

        property bool sampled: true
        property string cpuName: "Test CPU"
        property real cpuUsage: 25
        property bool cpuUsageAvailable: true
        property real cpuTemperature: 50
        property bool cpuTemperatureAvailable: true
        property real cpuFrequencyMHz: 4200
        property bool cpuFrequencyAvailable: true
        property string gpuName: "Test GPU"
        property real gpuUsage: 35
        property bool gpuUsageAvailable: true
        property real gpuTemperature: 60
        property bool gpuTemperatureAvailable: true
        property real gpuFrequencyMHz: 2300
        property bool gpuFrequencyAvailable: true
        property real ramUsage: 45
        property bool ramUsageAvailable: true
        property int ramUsedMB: 16384
        property int ramTotalMB: 32768
        property real diskUsage: 55
        property bool diskUsageAvailable: true
        property int diskUsedGB: 550
        property int diskTotalGB: 1000
        property bool networkAvailable: true
        property real rxSpeedKBs: 2048
        property real txSpeedKBs: 512
    }

    Component {
        id: homeComponent

        Pages.HomePage {
            runtime: runtimeMock
            systemMetrics: metricsMock
            deviceIconSource: ""
        }
    }

    function test_metricsGridUsesResponsiveColumns() {
        const wide = createTemporaryObject(
            homeComponent, testCase,
            {"width": 1200, "height": 800})
        verify(wide !== null)
        wait(0)
        const wideGrid =
            findChild(wide, "systemMetricsGrid")
        verify(wideGrid !== null)
        compare(wideGrid.columns, 4)

        const compact = createTemporaryObject(
            homeComponent, testCase,
            {"width": 900, "height": 800})
        verify(compact !== null)
        wait(0)
        const compactGrid =
            findChild(compact, "systemMetricsGrid")
        verify(compactGrid !== null)
        compare(compactGrid.columns, 2)

        const cpuCard =
            findChild(compact, "cpuMetricCard")
        const memoryCard =
            findChild(compact, "memoryMetricCard")
        verify(cpuCard !== null)
        verify(memoryCard !== null)
        metricsMock.cpuUsageAvailable = false
        metricsMock.ramUsageAvailable = false
        wait(0)
        compare(cpuCard.valueText, "—")
        compare(memoryCard.valueText, "—")
        compare(memoryCard.details,
                "Memory data unavailable")
    }
}
