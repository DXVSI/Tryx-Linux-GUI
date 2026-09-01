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

        function formatTemperature(available, rawCelsius) {
            return available
                   ? Math.round(rawCelsius * 9 / 5 + 32) + " °F"
                   : "—"
        }
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
        property real gpuPowerWatts: 0
        property bool gpuPowerAvailable: true
        property int gpuVramUsedMB: 0
        property int gpuVramTotalMB: 8192
        property bool gpuVramAvailable: true
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
        const gpuCard =
            findChild(compact, "gpuMetricCard")
        const memoryCard =
            findChild(compact, "memoryMetricCard")
        verify(cpuCard !== null)
        verify(gpuCard !== null)
        verify(memoryCard !== null)
        compare(cpuCard.y, gpuCard.y)
        compare(cpuCard.height, gpuCard.height)
        metricsMock.cpuUsageAvailable = false
        metricsMock.ramUsageAvailable = false
        wait(0)
        compare(cpuCard.valueText, "—")
        compare(memoryCard.valueText, "—")
        compare(memoryCard.details,
                "Memory data unavailable")
    }

    function test_temperatureFormattingComesFromRuntime() {
        metricsMock.cpuTemperature = 50
        metricsMock.cpuTemperatureAvailable = true

        const page = createTemporaryObject(
            homeComponent, testCase,
            {"width": 900, "height": 800})
        verify(page !== null)
        wait(0)

        const cpuCard = findChild(page, "cpuMetricCard")
        verify(cpuCard !== null)
        verify(cpuCard.details.startsWith("122 °F · "))

        metricsMock.cpuTemperatureAvailable = false
        wait(0)
        verify(cpuCard.details.startsWith("— · "))
    }

    function test_gpuTelemetryShowsPowerAndVramAvailability() {
        metricsMock.gpuPowerWatts = 0
        metricsMock.gpuPowerAvailable = true
        metricsMock.gpuVramUsedMB = 0
        metricsMock.gpuVramTotalMB = 8192
        metricsMock.gpuVramAvailable = true

        const page = createTemporaryObject(
            homeComponent, testCase,
            {"width": 900, "height": 800})
        verify(page !== null)
        wait(0)

        const gpuCard = findChild(page, "gpuMetricCard")
        verify(gpuCard !== null)
        compare(gpuCard.secondaryDetails,
                "0.0 W · 0.0 / 8.0 GiB VRAM")

        metricsMock.gpuPowerAvailable = false
        metricsMock.gpuVramAvailable = false
        wait(0)
        compare(gpuCard.secondaryDetails,
                "Power unavailable · VRAM unavailable")
    }
}
