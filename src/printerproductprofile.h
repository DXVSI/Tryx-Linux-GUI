#pragma once

#include <QString>
#include <optional>

enum class PrinterIdleMode { OverlayLayout, TransferOnly };

struct PrinterProductProfile {
    quint16 productId = 0;
    int mediaWidth = 0;
    int mediaHeight = 0;
    PrinterIdleMode idleMode = PrinterIdleMode::TransferOnly;
    bool mediaUploadSupported = false;
    bool mediaCatalogSupported = false;
    bool displayConfigurationSupported = false;
    bool splitAreaMediaSupported = false;
    bool overlayMetricsSupported = false;
    bool firmwareFlashSupported = false;
};

std::optional<PrinterProductProfile> printerProductProfileForId(quint16 productId);
QString printerProductIdString(quint16 productId);
