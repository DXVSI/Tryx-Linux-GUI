#include "hudpage.h"
#include "devicemanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QColorDialog>
#include <QDateTime>

HudPage::HudPage(DeviceManager *deviceMgr, QWidget *parent)
    : QWidget(parent), deviceMgr_(deviceMgr) {

    monitor_ = new SystemMonitor(this);
    metricsTimer_ = new QTimer(this);

    setupUi();

    connect(metricsTimer_, &QTimer::timeout, this, &HudPage::onSendMetrics);
}

void HudPage::setupUi() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    // Metric selection (max 3)
    auto *metricsGroup = new QGroupBox(tr("Metrics on display (max 3)"));
    auto *metricsLayout = new QGridLayout(metricsGroup);

    struct MetricDef {
        const char *displayName;
        QString protocolLabel;
        QString unit;
    };

    QList<MetricDef> defs = {
        {QT_TR_NOOP("CPU Temperature"),         "CPU Temperature",         "°C"},
        {QT_TR_NOOP("CPU Frequency"),           "CPU Frequency",           "MHz"},
        {QT_TR_NOOP("CPU Usage"),               "CPU Usage",               "%"},
        {QT_TR_NOOP("CPU Voltage"),             "CPU Voltage",             "V"},
        {QT_TR_NOOP("GPU Temperature"),         "GPU Temperature",         "°C"},
        {QT_TR_NOOP("GPU Frequency"),           "GPU Frequency",           "MHz"},
        {QT_TR_NOOP("GPU Voltage"),             "GPU Voltage",             "V"},
        {QT_TR_NOOP("Motherboard Temperature"), "Motherboard Temperature", "°C"},
        {QT_TR_NOOP("Memory Frequency"),        "Memory Frequency",        "MHz"},
        {QT_TR_NOOP("Memory Utilization"),      "Memory Utilization",      "%"},
        {QT_TR_NOOP("Date & Time"),             "Date & Time",             ""},
    };

    int row = 0, col = 0;
    for (const auto &def : defs) {
        auto *cb = new QCheckBox(tr(def.displayName));
        metricsLayout->addWidget(cb, row, col);

        MetricOption opt;
        opt.checkbox = cb;
        opt.label = def.protocolLabel;
        opt.unit = def.unit;
        metricOptions_.append(opt);

        connect(cb, &QCheckBox::toggled, this, &HudPage::onMetricToggled);

        col++;
        if (col >= 3) { col = 0; row++; }
    }

    selectionCountLabel_ = new QLabel(tr("Selected: 0 / 3"));
    metricsLayout->addWidget(selectionCountLabel_, row + 1, 0, 1, 3);

    mainLayout->addWidget(metricsGroup);

    // Display settings
    auto *settingsGroup = new QGroupBox(tr("Display settings"));
    auto *settingsLayout = new QGridLayout(settingsGroup);

    settingsLayout->addWidget(new QLabel(tr("Position:")), 0, 0);
    positionCombo_ = new QComboBox;
    positionCombo_->addItem(tr("Top"), "Top");
    positionCombo_->addItem(tr("Center"), "Center");
    positionCombo_->addItem(tr("Bottom"), "Bottom");
    settingsLayout->addWidget(positionCombo_, 0, 1);

    settingsLayout->addWidget(new QLabel(tr("Alignment:")), 1, 0);
    alignCombo_ = new QComboBox;
    alignCombo_->addItem(tr("Left"), "Left");
    alignCombo_->addItem(tr("Center"), "Center");
    alignCombo_->addItem(tr("Right"), "Right");
    settingsLayout->addWidget(alignCombo_, 1, 1);

    textColorBtn_ = new QPushButton(tr("Text color"));
    textColorBtn_->setStyleSheet("background-color: #FFFFFF;");
    settingsLayout->addWidget(textColorBtn_, 2, 0, 1, 2);

    connect(textColorBtn_, &QPushButton::clicked, this, &HudPage::onChooseTextColor);

    // Badges
    cbCpuBadge_ = new QCheckBox(tr("CPU Badge"));
    cbGpuBadge_ = new QCheckBox(tr("GPU Badge"));
    settingsLayout->addWidget(cbCpuBadge_, 3, 0);
    settingsLayout->addWidget(cbGpuBadge_, 3, 1);

    mainLayout->addWidget(settingsGroup);

    // Apply config button
    applyConfigBtn_ = new QPushButton(tr("Apply configuration"));
    applyConfigBtn_->setMinimumHeight(36);
    mainLayout->addWidget(applyConfigBtn_);
    connect(applyConfigBtn_, &QPushButton::clicked, this, &HudPage::onApplyConfig);

    // Interval + start/stop metrics sending
    auto *controlGroup = new QGroupBox(tr("Send metrics"));
    auto *controlLayout = new QHBoxLayout(controlGroup);

    controlLayout->addWidget(new QLabel(tr("Interval (sec):")));
    intervalSpin_ = new QSpinBox;
    intervalSpin_->setRange(1, 60);
    intervalSpin_->setValue(5);
    controlLayout->addWidget(intervalSpin_);

    startStopBtn_ = new QPushButton(tr("Start"));
    startStopBtn_->setMinimumHeight(36);
    controlLayout->addWidget(startStopBtn_);

    mainLayout->addWidget(controlGroup);

    connect(startStopBtn_, &QPushButton::clicked, this, &HudPage::onStartStopClicked);

    // Status
    statusLabel_ = new QLabel(tr("Metrics not being sent"));
    statusLabel_->setStyleSheet("color: #888; padding: 8px;");
    mainLayout->addWidget(statusLabel_);

    mainLayout->addStretch();
}

void HudPage::onMetricToggled() {
    int count = 0;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) count++;
    }

    selectionCountLabel_->setText(tr("Selected: %1 / 3").arg(count));

    // Disable unchecked if already 3 selected
    for (auto &opt : metricOptions_) {
        if (!opt.checkbox->isChecked()) {
            opt.checkbox->setEnabled(count < 3);
        }
    }
}

void HudPage::onChooseTextColor() {
    QColor color = QColorDialog::getColor(textColor_, this, tr("Text color"));
    if (color.isValid()) {
        textColor_ = color;
        textColorBtn_->setStyleSheet(
            QString("background-color: %1;").arg(color.name()));
    }
}

void HudPage::onApplyConfig() {
    applyScreenConfig();
    emit statusMessage(tr("Metrics configuration applied"));
}

void HudPage::applyScreenConfig() {
    // Collect selected labels
    QStringList labels;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            labels << opt.label;
        }
    }

    // Collect badges
    QStringList badges;
    if (cbCpuBadge_->isChecked()) badges << "CPU Badge";
    if (cbGpuBadge_->isChecked()) badges << "GPU Badge";

    // Get current media from device (we don't change it, just re-apply config)
    // The screen config is sent with current display settings
    deviceMgr_->setScreenConfig(
        {},                                     // media - empty means keep current
        "2:1",                                  // ratio
        "Full Screen",                          // screenMode
        "Single",                               // playMode
        labels,                                 // sysinfoDisplay
        positionCombo_->currentData().toString(), // position
        textColor_.name(),                      // color
        alignCombo_->currentData().toString(),  // align
        badges,                                 // badges
        0                                       // filter opacity
    );
}

void HudPage::onStartStopClicked() {
    if (hudRunning_) {
        stopHud();
    } else {
        startHud();
    }
}

void HudPage::startHud() {
    if (hudRunning_) return;

    // Check that at least one metric is selected
    QStringList labels;
    for (const auto &opt : metricOptions_) {
        if (opt.checkbox->isChecked()) {
            labels << opt.label;
        }
    }
    if (labels.isEmpty()) {
        emit statusMessage(tr("Select at least one metric"));
        return;
    }

    // Apply screen config first
    applyScreenConfig();

    hudRunning_ = true;
    startStopBtn_->setText(tr("Stop"));
    metricsTimer_->start(intervalSpin_->value() * 1000);
    emit hudRunningChanged(true);
    statusLabel_->setText(tr("Sending metrics..."));
    statusLabel_->setStyleSheet("color: #4CAF50; padding: 8px;");

    // Send first batch immediately
    onSendMetrics();
}

void HudPage::stopHud() {
    if (!hudRunning_) return;

    hudRunning_ = false;
    metricsTimer_->stop();
    startStopBtn_->setText(tr("Start"));
    emit hudRunningChanged(false);
    statusLabel_->setText(tr("Metrics not being sent"));
    statusLabel_->setStyleSheet("color: #888; padding: 8px;");
}

void HudPage::onSendMetrics() {
    monitor_->update();
    auto metrics = monitor_->currentMetrics();

    QStringList labels, values, units;

    for (const auto &opt : metricOptions_) {
        if (!opt.checkbox->isChecked()) continue;

        QString value;
        if (opt.label == "CPU Temperature") {
            value = QString::number(metrics.cpu.temperature, 'f', 0);
        } else if (opt.label == "CPU Frequency") {
            value = QString::number(metrics.cpu.frequencyMHz, 'f', 0);
        } else if (opt.label == "CPU Usage") {
            value = QString::number(metrics.cpu.usagePercent, 'f', 1);
        } else if (opt.label == "CPU Voltage") {
            value = "0"; // TODO: read CPU voltage from sysfs
        } else if (opt.label == "GPU Temperature") {
            value = !metrics.gpus.isEmpty()
                        ? QString::number(metrics.gpus[0].temperature, 'f', 0)
                        : "0";
        } else if (opt.label == "GPU Frequency") {
            value = "0"; // TODO: read GPU clock from sysfs
        } else if (opt.label == "GPU Voltage") {
            value = "0"; // TODO: read GPU voltage from sysfs
        } else if (opt.label == "Motherboard Temperature") {
            value = "0"; // TODO: read MB temp from sysfs
        } else if (opt.label == "Memory Frequency") {
            value = "0"; // TODO: read RAM frequency
        } else if (opt.label == "Memory Utilization") {
            value = QString::number(metrics.ram.usagePercent, 'f', 1);
        } else if (opt.label == "Date & Time") {
            value = QDateTime::currentDateTime().toString("hh:mm:ss");
        }

        labels << opt.label;
        values << value;
        units << opt.unit;
    }

    deviceMgr_->sendSysinfo(labels, values, units);

    statusLabel_->setText(tr("Sent: %1 metrics").arg(labels.size()));
}
