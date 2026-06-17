#include "splitconfig.h"

#include <cstdio>
#include <cstdlib>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>

static const char *METRIC_LABELS[] = {
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Temperature"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Frequency"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Usage"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Voltage"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Temperature"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Frequency"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Usage"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Voltage"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Hard Disk Temperature"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Motherboard Temperature"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Memory Frequency"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Memory Utilization"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Date&Time")
};

SplitConfigWidget::SplitConfigWidget(QWidget *parent)
    : QWidget(parent) {
    setupUi();
}

void SplitConfigWidget::setupUi() {
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(12);

    // Preview frames side by side
    auto *previewLayout = new QHBoxLayout;
    previewLayout->setSpacing(12);

    // Left preview
    auto *leftBox = new QVBoxLayout;
    auto *leftLabel = new QLabel(tr("Left"));
    leftLabel->setAlignment(Qt::AlignCenter);
    leftLabel->setStyleSheet("color: #ccc; font-weight: bold; font-size: 11px;");
    leftBox->addWidget(leftLabel);

    leftPreview_ = new QLabel;
    leftPreview_->setMinimumHeight(150);
    leftPreview_->setMinimumWidth(200);
    leftPreview_->setAlignment(Qt::AlignCenter);
    leftPreview_->setScaledContents(false);
    leftPreview_->setStyleSheet(
        "QLabel { background: #1e1e2e; border: 2px dashed #555; border-radius: 8px; color: #555; font-size: 12px; }");
    leftPreview_->setText(tr("Drop media here"));
    leftBox->addWidget(leftPreview_);

    leftFileLabel_ = new QLabel;
    leftFileLabel_->setAlignment(Qt::AlignCenter);
    leftFileLabel_->setStyleSheet("color: #888; font-size: 10px;");
    leftBox->addWidget(leftFileLabel_);

    previewLayout->addLayout(leftBox, 1);

    // Right preview
    auto *rightBox = new QVBoxLayout;
    auto *rightLabel = new QLabel(tr("Right"));
    rightLabel->setAlignment(Qt::AlignCenter);
    rightLabel->setStyleSheet("color: #ccc; font-weight: bold; font-size: 11px;");
    rightBox->addWidget(rightLabel);

    rightPreview_ = new QLabel;
    rightPreview_->setMinimumHeight(150);
    rightPreview_->setMinimumWidth(200);
    rightPreview_->setAlignment(Qt::AlignCenter);
    rightPreview_->setScaledContents(false);
    rightPreview_->setStyleSheet(
        "QLabel { background: #1e1e2e; border: 2px dashed #555; border-radius: 8px; color: #555; font-size: 12px; }");
    rightPreview_->setText(tr("Drop media here"));
    rightBox->addWidget(rightPreview_);

    rightFileLabel_ = new QLabel;
    rightFileLabel_->setAlignment(Qt::AlignCenter);
    rightFileLabel_->setStyleSheet("color: #888; font-size: 10px;");
    rightBox->addWidget(rightFileLabel_);

    previewLayout->addLayout(rightBox, 1);
    mainLayout->addLayout(previewLayout);

    // Settings row
    auto *settingsLayout = new QHBoxLayout;
    settingsLayout->setSpacing(12);

    settingsLayout->addWidget(new QLabel(tr("Play Mode:")));
    playModeCombo_ = new QComboBox;
    playModeCombo_->addItem(tr("Single"), "Single");
    playModeCombo_->addItem(tr("Shuffle"), "Shuffle");
    playModeCombo_->addItem(tr("Loop"), "Loop");
    settingsLayout->addWidget(playModeCombo_);

    waterfallCheck_ = new QCheckBox(tr("Waterfall Mode"));
    waterfallCheck_->setToolTip(tr("Enable if display is physically rotated 90 degrees"));
    waterfallCheck_->setStyleSheet("QCheckBox { color: #ccc; }");
    settingsLayout->addWidget(waterfallCheck_);

    // Left metrics button
    leftMetricsBtn_ = new QToolButton;
    leftMetricsBtn_->setText(QString::fromUtf8("%1: 0 / 3 \u25BC").arg(tr("Left")));
    leftMetricsBtn_->setPopupMode(QToolButton::InstantPopup);
    leftMetricsBtn_->setStyleSheet(
        "QToolButton { background: #2a2a3e; color: #fff; border: 1px solid #4a4a5e; "
        "border-radius: 4px; padding: 6px 12px; min-width: 100px; font-size: 12px; } "
        "QToolButton::menu-indicator { image: none; } "
        "QToolButton:hover { background: #3a3a4e; }");

    leftMetricsMenu_ = new QMenu(this);
    for (const auto *label : METRIC_LABELS) {
        auto *wa = new QWidgetAction(leftMetricsMenu_);
        auto *cb = new QCheckBox(tr(label));
        cb->setProperty("protocolLabel", label);
        cb->setStyleSheet("QCheckBox { color: #fff; padding: 4px 8px; } QCheckBox:hover { background: #3a3a4e; }");
        wa->setDefaultWidget(cb);
        leftMetricsMenu_->addAction(wa);
        leftMetricCheckboxes_.append(cb);
        connect(cb, &QCheckBox::toggled, this, [this](bool) {
            int count = 0;
            for (auto *c : leftMetricCheckboxes_) {
                if (c->isChecked()) count++;
            }
            if (count > 3) {
                auto *sender = qobject_cast<QCheckBox *>(QObject::sender());
                if (sender) sender->setChecked(false);
                return;
            }
            rebuildMetricsButtonCb(leftMetricsBtn_, leftMetricCheckboxes_, tr("Left"));
        });
    }
    leftMetricsBtn_->setMenu(leftMetricsMenu_);
    settingsLayout->addWidget(leftMetricsBtn_);

    // Right metrics button
    rightMetricsBtn_ = new QToolButton;
    rightMetricsBtn_->setText(QString::fromUtf8("%1: 0 / 3 \u25BC").arg(tr("Right")));
    rightMetricsBtn_->setPopupMode(QToolButton::InstantPopup);
    rightMetricsBtn_->setStyleSheet(
        "QToolButton { background: #2a2a3e; color: #fff; border: 1px solid #4a4a5e; "
        "border-radius: 4px; padding: 6px 12px; min-width: 100px; font-size: 12px; } "
        "QToolButton::menu-indicator { image: none; } "
        "QToolButton:hover { background: #3a3a4e; }");

    rightMetricsMenu_ = new QMenu(this);
    for (const auto *label : METRIC_LABELS) {
        auto *wa = new QWidgetAction(rightMetricsMenu_);
        auto *cb = new QCheckBox(tr(label));
        cb->setProperty("protocolLabel", label);
        cb->setStyleSheet("QCheckBox { color: #fff; padding: 4px 8px; } QCheckBox:hover { background: #3a3a4e; }");
        wa->setDefaultWidget(cb);
        rightMetricsMenu_->addAction(wa);
        rightMetricCheckboxes_.append(cb);
        connect(cb, &QCheckBox::toggled, this, [this](bool) {
            int count = 0;
            for (auto *c : rightMetricCheckboxes_) {
                if (c->isChecked()) count++;
            }
            if (count > 3) {
                auto *sender = qobject_cast<QCheckBox *>(QObject::sender());
                if (sender) sender->setChecked(false);
                return;
            }
            rebuildMetricsButtonCb(rightMetricsBtn_, rightMetricCheckboxes_, tr("Right"));
        });
    }
    rightMetricsBtn_->setMenu(rightMetricsMenu_);
    settingsLayout->addWidget(rightMetricsBtn_);

    settingsLayout->addStretch();
    mainLayout->addLayout(settingsLayout);
}

void SplitConfigWidget::rebuildMetricsButtonCb(QToolButton *btn, const QList<QCheckBox *> &checkboxes, const QString &side) {
    int count = 0;
    for (auto *c : checkboxes) {
        if (c->isChecked()) count++;
    }
    btn->setText(QString::fromUtf8("%1: %2 / 3 \u25BC").arg(side).arg(count));
}

QStringList SplitConfigWidget::leftMedia() const {
    QStringList list;
    if (!leftFilename_.isEmpty())
        list << leftFilename_;
    return list;
}

QStringList SplitConfigWidget::rightMedia() const {
    QStringList list;
    if (!rightFilename_.isEmpty())
        list << rightFilename_;
    return list;
}

QStringList SplitConfigWidget::leftMetrics() const {
    QStringList list;
    for (auto *c : leftMetricCheckboxes_) {
        if (c->isChecked())
            list << c->property("protocolLabel").toString();
    }
    return list;
}

QStringList SplitConfigWidget::rightMetrics() const {
    QStringList list;
    for (auto *c : rightMetricCheckboxes_) {
        if (c->isChecked())
            list << c->property("protocolLabel").toString();
    }
    return list;
}

QString SplitConfigWidget::playMode() const {
    return playModeCombo_->currentData().toString();
}

bool SplitConfigWidget::waterfallMode() const {
    return waterfallCheck_->isChecked();
}

void SplitConfigWidget::assignToLeft(const QString &filename, const QPixmap &thumb) {
    leftFilename_ = filename;
    if (!thumb.isNull()) {
        QSize labelSize = leftPreview_->size();
        if (labelSize.width() < 50) labelSize = QSize(200, 150);
        leftPreview_->setPixmap(thumb.scaled(labelSize - QSize(8, 8),
                                             Qt::KeepAspectRatio, Qt::SmoothTransformation));
        leftPreview_->setStyleSheet(
            "QLabel { background: #1e1e2e; border: 2px solid #6c5ce7; border-radius: 8px; padding: 4px; }");
    } else {
        leftPreview_->clear();
        leftPreview_->setText(filename);
        leftPreview_->setStyleSheet(
            "QLabel { background: #1e1e2e; border: 2px solid #6c5ce7; border-radius: 8px; color: #aaa; font-size: 11px; padding: 4px; }");
    }
    leftFileLabel_->setText(filename);
}

void SplitConfigWidget::assignToRight(const QString &filename, const QPixmap &thumb) {
    rightFilename_ = filename;
    if (!thumb.isNull()) {
        QSize labelSize = rightPreview_->size();
        if (labelSize.width() < 50) labelSize = QSize(200, 150);
        rightPreview_->setPixmap(thumb.scaled(labelSize - QSize(8, 8),
                                              Qt::KeepAspectRatio, Qt::SmoothTransformation));
        rightPreview_->setStyleSheet(
            "QLabel { background: #1e1e2e; border: 2px solid #6c5ce7; border-radius: 8px; padding: 4px; }");
    } else {
        rightPreview_->clear();
        rightPreview_->setText(filename);
        rightPreview_->setStyleSheet(
            "QLabel { background: #1e1e2e; border: 2px solid #6c5ce7; border-radius: 8px; color: #aaa; font-size: 11px; padding: 4px; }");
    }
    rightFileLabel_->setText(filename);
}
