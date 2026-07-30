import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "../components"

ScrollView {
    id: root

    required property var runtime
    required property var systemMetrics
    property url deviceIconSource:
        "qrc:/icons/tryx-panorama.png"

    signal openDisplayRequested()

    clip: true

    function percent(available, value) {
        return available ? Math.round(value) + "%" : "—"
    }

    function temperature(available, value) {
        return available ? Math.round(value) + " °C" : "—"
    }

    function frequency(available, value) {
        if (!available)
            return qsTr("Frequency unavailable")
        if (value >= 1000)
            return qsTr("%1 GHz").arg((value / 1000).toFixed(2))
        return qsTr("%1 MHz").arg(Math.round(value))
    }

    function memory(available, usedMB, totalMB) {
        if (!available || totalMB <= 0)
            return qsTr("Memory data unavailable")
        return qsTr("%1 / %2 GiB")
            .arg((usedMB / 1024).toFixed(1))
            .arg((totalMB / 1024).toFixed(1))
    }

    function storage(available, usedGB, totalGB) {
        if (!available || totalGB <= 0)
            return qsTr("Storage data unavailable")
        return qsTr("%1 / %2 GiB").arg(usedGB).arg(totalGB)
    }

    function rate(available, value) {
        if (!available)
            return "—"
        if (value >= 1024)
            return qsTr("%1 MiB/s").arg((value / 1024).toFixed(1))
        return qsTr("%1 KiB/s").arg(value.toFixed(1))
    }

    ColumnLayout {
        objectName: "dashboardContent"
        x: 28
        y: 24
        width: Math.max(0, root.availableWidth - 56)
        spacing: 20

        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: 230
            padding: 24

            background: Rectangle {
                radius: 14
                color: "#23282d"
                border.width: 1
                border.color: "#3d454c"

                Rectangle {
                    width: 5
                    height: parent.height - 36
                    anchors.left: parent.left
                    anchors.leftMargin: 1
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 3
                    color: "#def750"
                }
            }

            contentItem: RowLayout {
                spacing: 24

                Rectangle {
                    Layout.preferredWidth: 138
                    Layout.preferredHeight: 138
                    radius: 18
                    color: "#171a1e"
                    border.width: 1
                    border.color: "#343b42"

                    Image {
                        anchors.centerIn: parent
                        width: 92
                        height: 92
                        source: root.deviceIconSource
                        fillMode: Image.PreserveAspectFit
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: qsTr("PANORAMA SE")
                        color: "#f4f6f7"
                        font.pixelSize: 25
                        font.bold: true
                    }

                    RowLayout {
                        spacing: 8

                        Rectangle {
                            Layout.preferredWidth: 9
                            Layout.preferredHeight: 9
                            radius: 5
                            color: root.runtime.displaySessionActive
                                   ? "#66d18f" : "#efb85f"
                        }

                        Label {
                            text: root.runtime.connectionStatus
                            color: root.runtime.displaySessionActive
                                   ? "#bceccc" : "#efcf94"
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Layout: %1 · Playback: %2")
                              .arg(root.runtime.currentScreenMode ||
                                   qsTr("Unknown"))
                              .arg(root.runtime.currentPlayMode ||
                                   qsTr("Unknown"))
                        color: "#9ca4ac"
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.runtime.displayedMedia.length > 0
                              ? root.runtime.displayedMedia.join(", ")
                              : qsTr("No media is currently selected")
                        color: "#9ca4ac"
                        elide: Text.ElideMiddle
                    }
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 10

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: root.runtime.displayStateValid
                              ? qsTr("%1% brightness")
                                .arg(root.runtime.brightness)
                              : qsTr("Display state unavailable")
                        color: "#d7dce0"
                    }

                    PrimaryButton {
                        text: qsTr("Manage display")
                        onClicked: root.openDisplayRequested()
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("This PC")
                color: "#f4f6f7"
                font.pixelSize: 20
                font.bold: true
            }

            Item { Layout.fillWidth: true }

            Label {
                text: root.systemMetrics.sampled
                      ? qsTr("Updated automatically")
                      : qsTr("Reading sensors…")
                color: "#7f8991"
                font.pixelSize: 12
            }
        }

        GridLayout {
            id: metricsGrid

            objectName: "systemMetricsGrid"
            Layout.fillWidth: true
            columns: width >= 1040 ? 4 : 2
            columnSpacing: 14
            rowSpacing: 14

            MetricCard {
                objectName: "cpuMetricCard"
                Layout.fillWidth: true
                title: qsTr("CPU")
                subtitle: root.systemMetrics.cpuName ||
                          qsTr("Processor")
                valueText: root.percent(
                    root.systemMetrics.cpuUsageAvailable,
                    root.systemMetrics.cpuUsage)
                percentage:
                    root.systemMetrics.cpuUsageAvailable
                    ? root.systemMetrics.cpuUsage : -1
                details: root.temperature(
                    root.systemMetrics.cpuTemperatureAvailable,
                    root.systemMetrics.cpuTemperature) +
                    " · " + root.frequency(
                        root.systemMetrics.cpuFrequencyAvailable,
                        root.systemMetrics.cpuFrequencyMHz)
            }

            MetricCard {
                objectName: "gpuMetricCard"
                Layout.fillWidth: true
                title: qsTr("GPU")
                subtitle: root.systemMetrics.gpuName ||
                          qsTr("Graphics processor")
                valueText: root.percent(
                    root.systemMetrics.gpuUsageAvailable,
                    root.systemMetrics.gpuUsage)
                percentage:
                    root.systemMetrics.gpuUsageAvailable
                    ? root.systemMetrics.gpuUsage : -1
                details: root.temperature(
                    root.systemMetrics.gpuTemperatureAvailable,
                    root.systemMetrics.gpuTemperature) +
                    " · " + root.frequency(
                        root.systemMetrics.gpuFrequencyAvailable,
                        root.systemMetrics.gpuFrequencyMHz)
            }

            MetricCard {
                objectName: "memoryMetricCard"
                Layout.fillWidth: true
                title: qsTr("Memory")
                subtitle: qsTr("System RAM")
                valueText: root.percent(
                    root.systemMetrics.ramUsageAvailable,
                    root.systemMetrics.ramUsage)
                percentage:
                    root.systemMetrics.ramUsageAvailable
                    ? root.systemMetrics.ramUsage : -1
                details: root.memory(
                    root.systemMetrics.ramUsageAvailable,
                    root.systemMetrics.ramUsedMB,
                    root.systemMetrics.ramTotalMB)
            }

            MetricCard {
                objectName: "storageMetricCard"
                Layout.fillWidth: true
                title: qsTr("Storage")
                subtitle: qsTr("System disk")
                valueText: root.percent(
                    root.systemMetrics.diskUsageAvailable,
                    root.systemMetrics.diskUsage)
                percentage:
                    root.systemMetrics.diskUsageAvailable
                    ? root.systemMetrics.diskUsage : -1
                details: root.storage(
                    root.systemMetrics.diskUsageAvailable,
                    root.systemMetrics.diskUsedGB,
                    root.systemMetrics.diskTotalGB)
            }
        }

        Frame {
            Layout.fillWidth: true
            Layout.preferredHeight: 116
            padding: 18

            background: Rectangle {
                radius: 12
                color: "#23282d"
                border.width: 1
                border.color: "#343b42"
            }

            contentItem: RowLayout {
                spacing: 20

                ColumnLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("Network")
                        color: "#f4f6f7"
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Label {
                        text: root.systemMetrics.networkAvailable
                              ? qsTr("Current transfer rate")
                              : qsTr("Waiting for the next sample")
                        color: "#9ca4ac"
                        font.pixelSize: 12
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.fillHeight: true
                    color: "#343b42"
                }

                ColumnLayout {
                    Layout.preferredWidth: 190
                    Label {
                        text: qsTr("Download")
                        color: "#9ca4ac"
                        font.pixelSize: 11
                    }
                    Label {
                        text: root.rate(
                            root.systemMetrics.networkAvailable,
                            root.systemMetrics.rxSpeedKBs)
                        color: "#def750"
                        font.pixelSize: 23
                        font.bold: true
                    }
                }

                ColumnLayout {
                    Layout.preferredWidth: 190
                    Label {
                        text: qsTr("Upload")
                        color: "#9ca4ac"
                        font.pixelSize: 11
                    }
                    Label {
                        text: root.rate(
                            root.systemMetrics.networkAvailable,
                            root.systemMetrics.txSpeedKBs)
                        color: "#d7dce0"
                        font.pixelSize: 23
                        font.bold: true
                    }
                }
            }
        }

        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 20
        }
    }
}
