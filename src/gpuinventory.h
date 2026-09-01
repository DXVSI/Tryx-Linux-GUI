#pragma once

#include "nvidiasmiparser.h"

#include <QMetaType>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>

namespace tryx {

enum class GpuVendor {
    Other,
    Intel,
    Amd,
    Nvidia,
};

struct GpuMetrics {
    QString entityKey;
    QString pciBdf;
    QString pciDeviceId;
    QString pciRevisionId;
    QString providerUuid;
    QString name;
    GpuVendor vendor = GpuVendor::Other;
    bool bootVga = false;
    double temperature = 0.0;
    double usagePercent = 0.0;
    double frequencyMHz = 0.0;
    double voltageMV = 0.0;
    double powerWatts = 0.0;
    int64_t vramUsedMB = 0;
    int64_t vramTotalMB = 0;
    bool temperatureAvailable = false;
    bool usageAvailable = false;
    bool frequencyAvailable = false;
    bool powerAvailable = false;
    bool vramAvailable = false;
};

struct GpuInventoryResult {
    bool ok = false;
    QVector<GpuMetrics> gpus;
};

struct GpuSelectionPin {
    QString entityKey;
    QString providerUuid;

    bool isEmpty() const { return entityKey.isEmpty(); }
};

GpuSelectionPin primaryGpuSelectionPin(
    const QVector<GpuMetrics> &gpus);
qsizetype resolveGpuSelectionPin(
    const QVector<GpuMetrics> &gpus, GpuSelectionPin *pin);

class GpuProviderIdentityTracker final {
public:
    bool accept(
        const QVector<nvidia::GpuSample> &providerRows);
    void reset();

private:
    QHash<QString, QString> confirmedUuidByBdf_;
};

QString normalizeGpuPciBdf(const QString &value);

GpuInventoryResult buildGpuInventory(
    const QVector<GpuMetrics> &baseRows,
    const QVector<nvidia::GpuSample> &providerRows,
    quint64 topologyGeneration);

QString gpuTopologyFingerprint(const QVector<GpuMetrics> &baseRows,
                               bool *ok = nullptr);

}  // namespace tryx

using GpuMetrics = tryx::GpuMetrics;
using GpuVendor = tryx::GpuVendor;

Q_DECLARE_METATYPE(tryx::GpuMetrics)
Q_DECLARE_METATYPE(QVector<tryx::GpuMetrics>)
