#include "gpuinventory.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace tryx {
namespace {

int vendorPriority(GpuVendor vendor) {
    switch (vendor) {
    case GpuVendor::Nvidia:
        return 3;
    case GpuVendor::Amd:
        return 2;
    case GpuVendor::Intel:
        return 1;
    case GpuVendor::Other:
        return 0;
    }
    return 0;
}

bool baseRowsValid(const QVector<GpuMetrics> &rows) {
    static const QRegularExpression deviceIdExpression(
        QStringLiteral("^[0-9A-F]{4}$"));
    static const QRegularExpression revisionIdExpression(
        QStringLiteral("^[0-9A-F]{2}$"));
    QSet<QString> bdfs;
    for (const GpuMetrics &gpu : rows) {
        if (normalizeGpuPciBdf(gpu.pciBdf) != gpu.pciBdf ||
            !deviceIdExpression.match(gpu.pciDeviceId).hasMatch() ||
            !revisionIdExpression.match(gpu.pciRevisionId).hasMatch() ||
            bdfs.contains(gpu.pciBdf)) {
            return false;
        }
        bdfs.insert(gpu.pciBdf);
    }
    return true;
}

void applyProviderSample(GpuMetrics *gpu,
                         const nvidia::GpuSample &sample) {
    gpu->providerUuid = sample.uuid;
    gpu->temperature = sample.temperatureC.value;
    gpu->temperatureAvailable = sample.temperatureC.available;
    gpu->usagePercent = sample.utilizationPercent.value;
    gpu->usageAvailable = sample.utilizationPercent.available;
    gpu->frequencyMHz = sample.graphicsClockMHz.value;
    gpu->frequencyAvailable = sample.graphicsClockMHz.available;
    gpu->powerWatts = sample.powerWatts.value;
    gpu->powerAvailable = sample.powerWatts.available;
    gpu->vramAvailable = sample.vramUsedMiB.available &&
                         sample.vramTotalMiB.available;
    gpu->vramUsedMB = gpu->vramAvailable
        ? sample.vramUsedMiB.value
        : 0;
    gpu->vramTotalMB = gpu->vramAvailable
        ? sample.vramTotalMiB.value
        : 0;
}

}  // namespace

QString normalizeGpuPciBdf(const QString &value) {
    static const QRegularExpression expression(QStringLiteral(
        "^([0-9A-F]{4}|[0-9A-F]{8}):([0-9A-F]{2}):"
        "([0-9A-F]{2})\\.([0-7])$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = expression.match(value.trimmed());
    if (!match.hasMatch()) {
        return {};
    }
    bool domainOk = false;
    const quint64 domain = match.captured(1).toULongLong(&domainOk, 16);
    if (!domainOk || domain > 0xffffU) {
        return {};
    }
    return QStringLiteral("%1:%2:%3.%4")
        .arg(domain, 4, 16, QLatin1Char('0'))
        .arg(match.captured(2).toLower())
        .arg(match.captured(3).toLower())
        .arg(match.captured(4));
}

GpuSelectionPin primaryGpuSelectionPin(
    const QVector<GpuMetrics> &gpus) {
    if (gpus.isEmpty() || gpus.constFirst().entityKey.isEmpty()) {
        return {};
    }
    return GpuSelectionPin{gpus.constFirst().entityKey,
                           gpus.constFirst().providerUuid};
}

qsizetype resolveGpuSelectionPin(
    const QVector<GpuMetrics> &gpus, GpuSelectionPin *pin) {
    if (!pin || pin->isEmpty()) {
        return -1;
    }
    for (qsizetype index = 0; index < gpus.size(); ++index) {
        const GpuMetrics &gpu = gpus.at(index);
        if (gpu.entityKey != pin->entityKey) {
            continue;
        }
        if (pin->providerUuid.isEmpty() || gpu.providerUuid.isEmpty() ||
            pin->providerUuid == gpu.providerUuid) {
            if (pin->providerUuid.isEmpty() &&
                !gpu.providerUuid.isEmpty()) {
                pin->providerUuid = gpu.providerUuid;
            }
            return index;
        }
        break;
    }
    if (pin->providerUuid.isEmpty()) {
        return -1;
    }
    qsizetype resolvedIndex = -1;
    for (qsizetype index = 0; index < gpus.size(); ++index) {
        if (gpus.at(index).providerUuid != pin->providerUuid) {
            continue;
        }
        if (resolvedIndex >= 0) {
            return -1;
        }
        resolvedIndex = index;
    }
    if (resolvedIndex >= 0) {
        pin->entityKey = gpus.at(resolvedIndex).entityKey;
    }
    return resolvedIndex;
}

bool GpuProviderIdentityTracker::accept(
    const QVector<nvidia::GpuSample> &providerRows) {
    QHash<QString, QString> candidateByBdf;
    QSet<QString> candidateUuids;
    candidateByBdf.reserve(providerRows.size());
    for (const nvidia::GpuSample &sample : providerRows) {
        if (sample.pciBdf.isEmpty() || sample.uuid.isEmpty() ||
            candidateByBdf.contains(sample.pciBdf) ||
            candidateUuids.contains(sample.uuid)) {
            return false;
        }
        const auto confirmed = confirmedUuidByBdf_.constFind(
            sample.pciBdf);
        if (confirmed != confirmedUuidByBdf_.cend() &&
            *confirmed != sample.uuid) {
            return false;
        }
        candidateByBdf.insert(sample.pciBdf, sample.uuid);
        candidateUuids.insert(sample.uuid);
    }
    for (auto candidate = candidateByBdf.cbegin();
         candidate != candidateByBdf.cend(); ++candidate) {
        confirmedUuidByBdf_.insert(candidate.key(), candidate.value());
    }
    return true;
}

void GpuProviderIdentityTracker::reset() {
    confirmedUuidByBdf_.clear();
}

GpuInventoryResult buildGpuInventory(
    const QVector<GpuMetrics> &baseRows,
    const QVector<nvidia::GpuSample> &providerRows,
    quint64 topologyGeneration) {
    GpuInventoryResult result;
    if (!baseRowsValid(baseRows)) {
        return result;
    }

    result.gpus = baseRows;
    QHash<QString, qsizetype> byBdf;
    byBdf.reserve(result.gpus.size());
    for (qsizetype index = 0; index < result.gpus.size(); ++index) {
        GpuMetrics &gpu = result.gpus[index];
        gpu.entityKey = QStringLiteral("gpu:%1:%2")
                            .arg(topologyGeneration)
                            .arg(gpu.pciBdf);
        gpu.providerUuid.clear();
        if (gpu.vendor == GpuVendor::Nvidia) {
            gpu.temperature = 0.0;
            gpu.usagePercent = 0.0;
            gpu.frequencyMHz = 0.0;
            gpu.voltageMV = 0.0;
            gpu.powerWatts = 0.0;
            gpu.vramUsedMB = 0;
            gpu.vramTotalMB = 0;
            gpu.temperatureAvailable = false;
            gpu.usageAvailable = false;
            gpu.frequencyAvailable = false;
            gpu.powerAvailable = false;
            gpu.vramAvailable = false;
        }
        byBdf.insert(gpu.pciBdf, index);
    }

    QSet<QString> providerUuids;
    QSet<QString> providerBdfs;
    for (const nvidia::GpuSample &sample : providerRows) {
        if (providerUuids.contains(sample.uuid) ||
            providerBdfs.contains(sample.pciBdf) ||
            !byBdf.contains(sample.pciBdf)) {
            result.gpus.clear();
            return result;
        }
        providerUuids.insert(sample.uuid);
        providerBdfs.insert(sample.pciBdf);
        GpuMetrics &gpu = result.gpus[byBdf.value(sample.pciBdf)];
        if (gpu.vendor != GpuVendor::Nvidia) {
            result.gpus.clear();
            return result;
        }
        applyProviderSample(&gpu, sample);
    }
    if (!providerRows.isEmpty()) {
        const qsizetype nvidiaCount = std::count_if(
            result.gpus.cbegin(), result.gpus.cend(),
            [](const GpuMetrics &gpu) {
                return gpu.vendor == GpuVendor::Nvidia;
            });
        if (providerBdfs.size() != nvidiaCount) {
            result.gpus.clear();
            return result;
        }
    }

    std::stable_sort(
        result.gpus.begin(), result.gpus.end(),
        [](const GpuMetrics &left, const GpuMetrics &right) {
            const int leftPriority = vendorPriority(left.vendor);
            const int rightPriority = vendorPriority(right.vendor);
            if (leftPriority != rightPriority) {
                return leftPriority > rightPriority;
            }
            if (left.bootVga != right.bootVga) {
                return left.bootVga;
            }
            if (left.pciBdf != right.pciBdf) {
                return left.pciBdf < right.pciBdf;
            }
            return left.providerUuid < right.providerUuid;
        });
    result.ok = true;
    return result;
}

QString gpuTopologyFingerprint(const QVector<GpuMetrics> &baseRows,
                               bool *ok) {
    if (ok) {
        *ok = false;
    }
    if (!baseRowsValid(baseRows)) {
        return {};
    }
    QStringList identities;
    identities.reserve(baseRows.size());
    for (const GpuMetrics &gpu : baseRows) {
        identities.append(
            QStringLiteral("%1|%2|%3|%4|%5")
                .arg(static_cast<int>(gpu.vendor))
                .arg(gpu.pciDeviceId)
                .arg(gpu.pciRevisionId)
                .arg(gpu.bootVga ? 1 : 0)
                .arg(gpu.pciBdf));
    }
    std::sort(identities.begin(), identities.end());
    if (ok) {
        *ok = true;
    }
    return identities.join(QLatin1Char(';'));
}

}  // namespace tryx
