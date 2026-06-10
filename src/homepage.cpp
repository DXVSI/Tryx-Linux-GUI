#include "homepage.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QScrollArea>
#include <QPainterPath>
#include <QtMath>

// ============================================================
// GaugeWidget - semi-circular gauge with needle
// ============================================================

GaugeWidget::GaugeWidget(QWidget *parent)
    : QWidget(parent) {
    setMinimumSize(140, 100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void GaugeWidget::setValue(double percent) {
    value_ = qBound(0.0, percent, 100.0);
    update();
}

void GaugeWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    const int margin = 10;
    const int maxDiam = qMin(w - 40, 160);
    const int diameter = qMin(maxDiam, (h - 20) * 2);
    const int radius = diameter / 2;
    const QPointF center(w / 2.0, margin + radius + 4);

    const QRectF arcRect(center.x() - radius, center.y() - radius,
                         diameter, diameter);

    // Arc angles: Qt uses 1/16th degrees, 0 = 3 o'clock, counter-clockwise positive
    // We want arc from 200 deg to -20 deg (span of 220 degrees, a wide semi-circle)
    const double startAngle = 200.0;   // left side
    const double spanAngle = -220.0;   // sweep clockwise

    // Background arc
    QPen bgPen(QColor(60, 60, 80), 8, Qt::SolidLine, Qt::RoundCap);
    p.setPen(bgPen);
    p.drawArc(arcRect, static_cast<int>(startAngle * 16),
              static_cast<int>(spanAngle * 16));

    // Value arc
    double valueSpan = spanAngle * (value_ / 100.0);
    QPen valPen(QColor(0xDE, 0xF7, 0x50), 8, Qt::SolidLine, Qt::RoundCap);
    p.setPen(valPen);
    if (qAbs(valueSpan) > 0.5) {
        p.drawArc(arcRect, static_cast<int>(startAngle * 16),
                  static_cast<int>(valueSpan * 16));
    }

    // Needle
    double needleAngle = startAngle + spanAngle * (value_ / 100.0);
    double needleRad = qDegreesToRadians(needleAngle);
    double needleLen = radius - 14;
    QPointF needleTip(center.x() + needleLen * qCos(needleRad),
                      center.y() - needleLen * qSin(needleRad));
    QPen needlePen(QColor(255, 255, 255, 200), 2, Qt::SolidLine, Qt::RoundCap);
    p.setPen(needlePen);
    p.drawLine(center, needleTip);

    // Center dot
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 220));
    p.drawEllipse(center, 3.0, 3.0);
}

// ============================================================
// GraphWidget - rolling line chart
// ============================================================

GraphWidget::GraphWidget(const QColor &lineColor, QWidget *parent)
    : QWidget(parent), lineColor_(lineColor) {
    setMinimumSize(200, 60);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    data_.fill(0.0, MAX_POINTS);
}

void GraphWidget::addValue(double val) {
    data_.append(val);
    if (data_.size() > MAX_POINTS) {
        data_.removeFirst();
    }
    update();
}

void GraphWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int w = width();
    const int h = height();
    const int pad = 4;
    const double drawW = w - 2 * pad;
    const double drawH = h - 2 * pad;

    // Find max for scaling
    double maxVal = 1.0;
    for (double v : data_) {
        if (v > maxVal) maxVal = v;
    }

    // Draw grid lines (subtle)
    p.setPen(QPen(QColor(60, 60, 80, 80), 1));
    for (int i = 1; i < 4; i++) {
        double y = pad + drawH * i / 4.0;
        p.drawLine(QPointF(pad, y), QPointF(w - pad, y));
    }

    // Build path
    if (data_.size() < 2) return;

    QPainterPath path;
    QPainterPath fillPath;
    double step = drawW / (MAX_POINTS - 1);

    for (int i = 0; i < data_.size(); i++) {
        double x = pad + i * step;
        double y = pad + drawH - (data_[i] / maxVal) * drawH;
        if (i == 0) {
            path.moveTo(x, y);
            fillPath.moveTo(x, h - pad);
            fillPath.lineTo(x, y);
        } else {
            path.lineTo(x, y);
            fillPath.lineTo(x, y);
        }
    }

    // Fill under the curve
    fillPath.lineTo(pad + (data_.size() - 1) * step, h - pad);
    fillPath.closeSubpath();
    QColor fillColor = lineColor_;
    fillColor.setAlpha(30);
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawPath(fillPath);

    // Draw line
    QPen linePen(lineColor_, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(linePen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// ============================================================
// Homepage
// ============================================================

static const QColor BG_COLOR(0x1a, 0x1a, 0x2e);
static const QColor CARD_BG(0x2a, 0x2a, 0x3e);
static const QColor CARD_BORDER(0x3a, 0x3a, 0x4e);
static const QColor ACCENT(0xDE, 0xF7, 0x50);
static const QColor TEXT_WHITE(255, 255, 255);
static const QColor TEXT_GRAY(0xaa, 0xaa, 0xaa);
static const QColor GRAPH_GREEN(0x55, 0xef, 0xc4);
static const QColor GRAPH_BLUE(0x74, 0xb9, 0xff);

static const QString CARD_STYLE =
    "QFrame#DashCard {"
    "  background: #2a2a3e;"
    "  border: 1px solid #3a3a4e;"
    "  border-radius: 12px;"
    "  padding: 16px;"
    "}";

Homepage::Homepage(QWidget *parent)
    : QWidget(parent) {
    monitor_ = new SystemMonitor(this);
    updateTimer_ = new QTimer(this);

    setupUi();

    connect(updateTimer_, &QTimer::timeout, monitor_, &SystemMonitor::update);
    connect(monitor_, &SystemMonitor::metricsUpdated, this, &Homepage::onMetricsUpdated);

    updateTimer_->start(2000);
    monitor_->update();
}

QFrame *Homepage::createCard() {
    auto *card = new QFrame;
    card->setObjectName("DashCard");
    card->setStyleSheet(CARD_STYLE);
    return card;
}

void Homepage::setupUi() {
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setStyleSheet(
        "QScrollArea { border: none; background: #1a1a2e; }"
        "QScrollBar:vertical { background: #1a1a2e; width: 6px; }"
        "QScrollBar::handle:vertical { background: #3a3a4e; border-radius: 3px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }");

    auto *scrollWidget = new QWidget;
    scrollWidget->setStyleSheet("background: #1a1a2e;");
    auto *mainLayout = new QVBoxLayout(scrollWidget);
    mainLayout->setSpacing(16);
    mainLayout->setContentsMargins(24, 24, 24, 24);

    // Title
    auto *titleLabel = new QLabel(tr("PANORAMA"));
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(22);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setStyleSheet("color: #fff; background: transparent;");
    mainLayout->addWidget(titleLabel);

    auto *subtitleLabel = new QLabel(tr("System Monitoring Dashboard"));
    subtitleLabel->setStyleSheet("color: #666; font-size: 12px; background: transparent; margin-bottom: 4px;");
    mainLayout->addWidget(subtitleLabel);

    // Cards grid: 2 rows x 3 columns conceptually
    // Network spans rows 0-1, col 0
    // CPU at (0,1), GPU at (0,2)
    // Memory at (1,1), Disk at (1,2)
    auto *grid = new QGridLayout;
    grid->setSpacing(12);
    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(2, 2);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);

    // ========== Network Status Card (spans 2 rows, left) ==========
    {
        auto *card = createCard();
        auto *layout = new QVBoxLayout(card);
        layout->setSpacing(8);

        auto *title = new QLabel(tr("Network Status"));
        QFont tf = title->font();
        tf.setPointSize(12);
        tf.setBold(true);
        title->setFont(tf);
        title->setStyleSheet("color: #fff; border: none; background: transparent;");
        layout->addWidget(title);

        layout->addSpacing(4);

        // Download section
        downloadGraph_ = new GraphWidget(GRAPH_GREEN, this);
        downloadGraph_->setMinimumHeight(80);
        layout->addWidget(downloadGraph_);

        netDownloadLabel_ = new QLabel(tr("Download: 0 KB/s"));
        netDownloadLabel_->setStyleSheet("color: #55efc4; border: none; background: transparent; font-size: 12px;");
        QFont dlFont = netDownloadLabel_->font();
        dlFont.setBold(true);
        netDownloadLabel_->setFont(dlFont);
        layout->addWidget(netDownloadLabel_);

        layout->addSpacing(8);

        // Upload section
        uploadGraph_ = new GraphWidget(GRAPH_BLUE, this);
        uploadGraph_->setMinimumHeight(80);
        layout->addWidget(uploadGraph_);

        netUploadLabel_ = new QLabel(tr("Upload: 0 KB/s"));
        netUploadLabel_->setStyleSheet("color: #74b9ff; border: none; background: transparent; font-size: 12px;");
        QFont ulFont = netUploadLabel_->font();
        ulFont.setBold(true);
        netUploadLabel_->setFont(ulFont);
        layout->addWidget(netUploadLabel_);

        layout->addStretch();

        grid->addWidget(card, 0, 0, 2, 1);
    }

    // ========== CPU Load Card (top middle) ==========
    {
        auto *card = createCard();
        auto *layout = new QVBoxLayout(card);
        layout->setSpacing(4);
        layout->setAlignment(Qt::AlignCenter);

        cpuUsageLabel_ = new QLabel("0%");
        QFont bigFont = cpuUsageLabel_->font();
        bigFont.setPointSize(24);
        bigFont.setBold(true);
        cpuUsageLabel_->setFont(bigFont);
        cpuUsageLabel_->setStyleSheet("color: #fff; border: none; background: transparent;");
        cpuUsageLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(cpuUsageLabel_);

        auto *subtitle = new QLabel(tr("CPU Load"));
        subtitle->setStyleSheet("color: #aaa; border: none; background: transparent; font-size: 10px;");
        subtitle->setAlignment(Qt::AlignCenter);
        layout->addWidget(subtitle);

        layout->addSpacing(2);

        cpuGauge_ = new GaugeWidget(this);
        cpuGauge_->setMinimumHeight(100);
        layout->addWidget(cpuGauge_);

        cpuTempLabel_ = new QLabel(QString::fromUtf8("\xF0\x9F\x8C\xA1 0\xC2\xB0""C"));
        cpuTempLabel_->setStyleSheet("color: #fff; border: none; background: transparent; font-size: 12px;");
        cpuTempLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(cpuTempLabel_);

        grid->addWidget(card, 0, 1);
    }

    // ========== GPU Load Card (top right) ==========
    {
        auto *card = createCard();
        auto *layout = new QVBoxLayout(card);
        layout->setSpacing(4);
        layout->setAlignment(Qt::AlignCenter);

        gpuUsageLabel_ = new QLabel("0%");
        QFont bigFont = gpuUsageLabel_->font();
        bigFont.setPointSize(24);
        bigFont.setBold(true);
        gpuUsageLabel_->setFont(bigFont);
        gpuUsageLabel_->setStyleSheet("color: #fff; border: none; background: transparent;");
        gpuUsageLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(gpuUsageLabel_);

        auto *subtitle = new QLabel(tr("GPU Load"));
        subtitle->setStyleSheet("color: #aaa; border: none; background: transparent; font-size: 10px;");
        subtitle->setAlignment(Qt::AlignCenter);
        layout->addWidget(subtitle);

        layout->addSpacing(2);

        gpuGauge_ = new GaugeWidget(this);
        gpuGauge_->setMinimumHeight(100);
        layout->addWidget(gpuGauge_);

        gpuTempLabel_ = new QLabel(QString::fromUtf8("\xF0\x9F\x8C\xA1 0\xC2\xB0""C"));
        gpuTempLabel_->setStyleSheet("color: #fff; border: none; background: transparent; font-size: 12px;");
        gpuTempLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(gpuTempLabel_);

        grid->addWidget(card, 0, 2);
    }

    // ========== Memory Load Card (bottom middle) ==========
    {
        auto *card = createCard();
        auto *layout = new QVBoxLayout(card);
        layout->setSpacing(6);
        layout->setAlignment(Qt::AlignCenter);

        memUsageLabel_ = new QLabel("0%");
        QFont bigFont = memUsageLabel_->font();
        bigFont.setPointSize(36);
        bigFont.setBold(true);
        memUsageLabel_->setFont(bigFont);
        memUsageLabel_->setStyleSheet("color: #fff; border: none; background: transparent;");
        memUsageLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(memUsageLabel_);

        auto *subtitle = new QLabel(tr("Memory Load"));
        subtitle->setStyleSheet("color: #aaa; border: none; background: transparent; font-size: 11px;");
        subtitle->setAlignment(Qt::AlignCenter);
        layout->addWidget(subtitle);

        layout->addSpacing(8);

        // Progress bar container
        memBar_ = new QFrame;
        memBar_->setFixedHeight(12);
        memBar_->setStyleSheet(
            "QFrame { background: #1a1a2e; border-radius: 6px; border: none; }");

        memBarFill_ = new QFrame(memBar_);
        memBarFill_->setFixedHeight(12);
        memBarFill_->setStyleSheet(
            "QFrame { background: #DEF750; border-radius: 6px; border: none; }");
        memBarFill_->setGeometry(0, 0, 0, 12);

        layout->addWidget(memBar_);

        layout->addSpacing(4);

        memDetailLabel_ = new QLabel(QString::fromUtf8("\xF0\x9F\x92\xBE 0.0G / 0G"));
        memDetailLabel_->setStyleSheet("color: #aaa; border: none; background: transparent; font-size: 12px;");
        memDetailLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(memDetailLabel_);

        layout->addStretch();

        grid->addWidget(card, 1, 1);
    }

    // ========== Hard Disk Load Card (bottom right) ==========
    {
        auto *card = createCard();
        auto *layout = new QVBoxLayout(card);
        layout->setSpacing(6);
        layout->setAlignment(Qt::AlignCenter);

        diskUsageLabel_ = new QLabel("0%");
        QFont bigFont = diskUsageLabel_->font();
        bigFont.setPointSize(36);
        bigFont.setBold(true);
        diskUsageLabel_->setFont(bigFont);
        diskUsageLabel_->setStyleSheet("color: #fff; border: none; background: transparent;");
        diskUsageLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(diskUsageLabel_);

        auto *subtitle = new QLabel(tr("Hard disk Load"));
        subtitle->setStyleSheet("color: #aaa; border: none; background: transparent; font-size: 11px;");
        subtitle->setAlignment(Qt::AlignCenter);
        layout->addWidget(subtitle);

        layout->addSpacing(8);

        // Progress bar container
        diskBar_ = new QFrame;
        diskBar_->setFixedHeight(12);
        diskBar_->setStyleSheet(
            "QFrame { background: #1a1a2e; border-radius: 6px; border: none; }");

        diskBarFill_ = new QFrame(diskBar_);
        diskBarFill_->setFixedHeight(12);
        diskBarFill_->setStyleSheet(
            "QFrame { background: #DEF750; border-radius: 6px; border: none; }");
        diskBarFill_->setGeometry(0, 0, 0, 12);

        layout->addWidget(diskBar_);

        layout->addSpacing(4);

        diskDetailLabel_ = new QLabel(QString::fromUtf8("\xF0\x9F\x92\xBF 0G / 0G"));
        diskDetailLabel_->setStyleSheet("color: #aaa; border: none; background: transparent; font-size: 12px;");
        diskDetailLabel_->setAlignment(Qt::AlignCenter);
        layout->addWidget(diskDetailLabel_);

        layout->addStretch();

        grid->addWidget(card, 1, 2);
    }

    mainLayout->addLayout(grid);
    mainLayout->addStretch();

    scrollArea->setWidget(scrollWidget);
    outerLayout->addWidget(scrollArea);
}

void Homepage::onMetricsUpdated(const SystemMetrics &m) {
    auto formatSpeed = [](double kbs) -> QString {
        if (kbs >= 1024.0) {
            return QString("%1 MB/s").arg(kbs / 1024.0, 0, 'f', 1);
        }
        return QString("%1 KB/s").arg(kbs, 0, 'f', 0);
    };

    // CPU
    int cpuUsage = static_cast<int>(m.cpu.usagePercent);
    cpuUsageLabel_->setText(QString("%1%").arg(cpuUsage));
    cpuGauge_->setValue(m.cpu.usagePercent);
    cpuTempLabel_->setText(QString::fromUtf8("\xF0\x9F\x8C\xA1 %1\xC2\xB0""C")
                               .arg(m.cpu.temperature, 0, 'f', 0));

    // GPU
    if (!m.gpus.isEmpty()) {
        int gpuUsage = static_cast<int>(m.gpus[0].usagePercent);
        gpuUsageLabel_->setText(QString("%1%").arg(gpuUsage));
        gpuGauge_->setValue(m.gpus[0].usagePercent);
        gpuTempLabel_->setText(QString::fromUtf8("\xF0\x9F\x8C\xA1 %1\xC2\xB0""C")
                                   .arg(m.gpus[0].temperature, 0, 'f', 0));
    }

    // Memory
    int memUsage = static_cast<int>(m.ram.usagePercent);
    memUsageLabel_->setText(QString("%1%").arg(memUsage));
    double usedGB = m.ram.usedMB / 1024.0;
    double totalGB = m.ram.totalMB / 1024.0;
    memDetailLabel_->setText(QString::fromUtf8("\xF0\x9F\x92\xBE %1G / %2G")
                                 .arg(usedGB, 0, 'f', 1)
                                 .arg(totalGB, 0, 'f', 0));
    // Update progress bar fill width
    int barWidth = memBar_->width();
    int fillWidth = static_cast<int>(barWidth * m.ram.usagePercent / 100.0);
    memBarFill_->setGeometry(0, 0, fillWidth, 12);

    // Disk
    int diskUsage = static_cast<int>(m.disk.usagePercent);
    diskUsageLabel_->setText(QString("%1%").arg(diskUsage));
    diskDetailLabel_->setText(QString::fromUtf8("\xF0\x9F\x92\xBF %1G / %2G")
                                  .arg(m.disk.usedGB)
                                  .arg(m.disk.totalGB));
    int diskBarWidth = diskBar_->width();
    int diskFillWidth = static_cast<int>(diskBarWidth * m.disk.usagePercent / 100.0);
    diskBarFill_->setGeometry(0, 0, diskFillWidth, 12);

    // Network
    netDownloadLabel_->setText(tr("Download: %1").arg(formatSpeed(m.net.rxSpeedKBs)));
    netUploadLabel_->setText(tr("Upload: %1").arg(formatSpeed(m.net.txSpeedKBs)));
    downloadGraph_->addValue(m.net.rxSpeedKBs);
    uploadGraph_->addValue(m.net.txSpeedKBs);
}
