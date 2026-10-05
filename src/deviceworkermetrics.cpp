#include "deviceworkermetrics_p.h"
#include "runtimecontract.h"
#include "systemmonitor.h"

namespace tryx::worker_metrics {

const GpuMetrics *gpuForPin(const SystemMetrics &metrics,
                            tryx::GpuSelectionPin *pin) {
    const qsizetype index = tryx::resolveGpuSelectionPin(
        metrics.gpus, pin);
    return index >= 0 ? &metrics.gpus.at(index) : nullptr;
}

void collectPaseMetricValues(const SystemMetrics &metrics,
                             const GpuMetrics *selectedGpu,
                             const QString &temperatureUnit,
                             QStringList *labels,
                             QStringList *values, QStringList *units) {
    labels->clear();
    values->clear();
    units->clear();
    const auto appendMetric = [labels, values, units](
                                  const QString &label, double value,
                                  const QString &unit, bool available,
                                  int precision = 0) {
        if (!available) {
            return;
        }
        labels->append(label);
        values->append(QString::number(value, 'f', precision));
        units->append(unit);
    };
    const QString temperatureSymbol =
        tryxTemperatureUnitSymbol(temperatureUnit);
    const auto appendTemperature =
        [labels, values, units, &temperatureUnit,
         &temperatureSymbol](const QString &label, double celsius,
                             bool available) {
            if (!available) {
                return;
            }
            const QString value = tryxFormatTemperatureValue(
                celsius, temperatureUnit);
            if (value.isEmpty() || temperatureSymbol.isEmpty()) {
                return;
            }
            labels->append(label);
            values->append(value);
            units->append(temperatureSymbol);
        };
    appendTemperature(QStringLiteral("CPU Temperature"),
                      metrics.cpu.temperature,
                      metrics.cpu.temperatureAvailable);
    appendMetric(QStringLiteral("CPU Frequency"), metrics.cpu.frequencyMHz,
                 QStringLiteral("MHZ"), metrics.cpu.frequencyAvailable);
    appendMetric(QStringLiteral("CPU Usage"), metrics.cpu.usagePercent,
                 QStringLiteral("%"), metrics.cpu.usageAvailable);
    appendMetric(QStringLiteral("CPU Power"), metrics.cpu.powerWatts,
                 QStringLiteral("W"), metrics.cpu.powerAvailable, 1);
    if (selectedGpu) {
        const GpuMetrics &gpu = *selectedGpu;
        appendTemperature(QStringLiteral("GPU Temperature"),
                          gpu.temperature,
                          gpu.temperatureAvailable);
        appendMetric(QStringLiteral("GPU Frequency"), gpu.frequencyMHz,
                     QStringLiteral("MHZ"), gpu.frequencyAvailable);
        appendMetric(QStringLiteral("GPU Usage"), gpu.usagePercent,
                     QStringLiteral("%"), gpu.usageAvailable);
        appendMetric(QStringLiteral("GPU Power"), gpu.powerWatts,
                     QStringLiteral("W"), gpu.powerAvailable, 1);
    }
    appendMetric(QStringLiteral("Memory Frequency"),
                 metrics.ram.frequencyMHz, QStringLiteral("MHZ"),
                 metrics.ram.frequencyAvailable);
    appendMetric(QStringLiteral("Memory Usage"), metrics.ram.usagePercent,
                 QStringLiteral("%"), metrics.ram.usageAvailable);
}

}
