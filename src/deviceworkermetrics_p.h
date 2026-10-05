#pragma once

#include "gpuinventory.h"
#include <QStringList>

struct SystemMetrics;

namespace tryx::worker_metrics {
const GpuMetrics *gpuForPin(const SystemMetrics &metrics,
                            tryx::GpuSelectionPin *pin);
void collectPaseMetricValues(const SystemMetrics &metrics,
                             const GpuMetrics *selectedGpu,
                             const QString &temperatureUnit,
                             QStringList *labels, QStringList *values,
                             QStringList *units);
}
