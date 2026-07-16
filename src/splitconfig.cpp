#include "splitconfig.h"

#include <cstdio>
#include <cstdlib>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QColorDialog>
#include <QSignalBlocker>

namespace {

const QString kCustomColorAction =
    QStringLiteral("__choose_custom_color__");

}  // namespace

static const char *METRIC_LABELS[] = {
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Temperature"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Frequency"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Usage"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "CPU Power"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Temperature"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Frequency"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Usage"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "GPU Power"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Memory Frequency"),
    QT_TRANSLATE_NOOP("SplitConfigWidget", "Memory Usage"),
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

    auto *badgesLayout = new QHBoxLayout;
    badgesLayout->setSpacing(12);
    badgesLayout->addWidget(new QLabel(tr("Badges:")));
    leftCpuBadge_ = new QCheckBox(tr("Left CPU"));
    leftGpuBadge_ = new QCheckBox(tr("Left GPU"));
    rightCpuBadge_ = new QCheckBox(tr("Right CPU"));
    rightGpuBadge_ = new QCheckBox(tr("Right GPU"));
    const QList<QCheckBox *> badges{
        leftCpuBadge_, leftGpuBadge_, rightCpuBadge_, rightGpuBadge_};
    for (QCheckBox *badge : badges) {
        badge->setStyleSheet("QCheckBox { color: #ccc; }");
        badgesLayout->addWidget(badge);
    }
    badgesLayout->addStretch();
    mainLayout->addLayout(badgesLayout);

    const auto addAreaSettings =
        [this, mainLayout](
            const QString &side, QComboBox **positionCombo,
            QComboBox **colorCombo, QComboBox **alignmentCombo) {
            auto *layout = new QHBoxLayout;
            layout->setSpacing(10);
            auto *sideLabel = new QLabel(side);
            sideLabel->setStyleSheet(
                "color: #ccc; font-weight: bold;");
            layout->addWidget(sideLabel);

            layout->addWidget(new QLabel(tr("Position:")));
            *positionCombo = new QComboBox;
            (*positionCombo)->addItem(tr("Top"), "Top");
            (*positionCombo)->addItem(tr("Bottom"), "Bottom");
            layout->addWidget(*positionCombo);

            layout->addWidget(new QLabel(tr("Color:")));
            *colorCombo = new QComboBox;
            (*colorCombo)->addItem(tr("Light"), "#dcdcdc");
            (*colorCombo)->addItem(tr("Black"), "#000000");
            (*colorCombo)->addItem(tr("Custom..."),
                                   kCustomColorAction);
            (*colorCombo)->setProperty(
                "selectedColor", QStringLiteral("#dcdcdc"));
            connect(
                *colorCombo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this, [colorCombo](int index) {
                    const QString value =
                        (*colorCombo)->itemData(index).toString();
                    const QColor color(value);
                    if (color.isValid()) {
                        (*colorCombo)->setProperty(
                            "selectedColor", color.name());
                    }
                });
            connect(
                *colorCombo, qOverload<int>(&QComboBox::activated),
                this, [this, colorCombo](int index) {
                    if ((*colorCombo)->itemData(index).toString() ==
                        kCustomColorAction) {
                        chooseCustomColor(*colorCombo);
                    }
                });
            layout->addWidget(*colorCombo);

            layout->addWidget(new QLabel(tr("Align:")));
            *alignmentCombo = new QComboBox;
            (*alignmentCombo)->addItem(tr("Left"), "Left");
            (*alignmentCombo)->addItem(tr("Center"), "Center");
            (*alignmentCombo)->addItem(tr("Right"), "Right");
            layout->addWidget(*alignmentCombo);
            layout->addStretch();
            mainLayout->addLayout(layout);
        };
    addAreaSettings(
        tr("Left"), &leftPositionCombo_, &leftColorCombo_,
        &leftAlignmentCombo_);
    addAreaSettings(
        tr("Right"), &rightPositionCombo_, &rightColorCombo_,
        &rightAlignmentCombo_);
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

QStringList SplitConfigWidget::checkedBadges(QCheckBox *cpu,
                                              QCheckBox *gpu) {
    QStringList badges;
    if (cpu && cpu->isChecked()) {
        badges.append(QStringLiteral("CPU Badge"));
    }
    if (gpu && gpu->isChecked()) {
        badges.append(QStringLiteral("GPU Badge"));
    }
    return badges;
}

QStringList SplitConfigWidget::leftBadges() const {
    return checkedBadges(leftCpuBadge_, leftGpuBadge_);
}

QStringList SplitConfigWidget::rightBadges() const {
    return checkedBadges(rightCpuBadge_, rightGpuBadge_);
}

QString SplitConfigWidget::leftPosition() const {
    return leftPositionCombo_->currentData().toString();
}

QString SplitConfigWidget::rightPosition() const {
    return rightPositionCombo_->currentData().toString();
}

QString SplitConfigWidget::leftColor() const {
    return colorComboValue(leftColorCombo_);
}

QString SplitConfigWidget::rightColor() const {
    return colorComboValue(rightColorCombo_);
}

QString SplitConfigWidget::colorComboValue(
    const QComboBox *combo) {
    if (!combo) {
        return QStringLiteral("#dcdcdc");
    }
    const QColor color(
        combo->property("selectedColor").toString());
    return color.isValid()
        ? color.name()
        : QStringLiteral("#dcdcdc");
}

void SplitConfigWidget::chooseCustomColor(QComboBox *combo) {
    if (!combo) {
        return;
    }
    const QString previous = colorComboValue(combo);
    const QColor selected = QColorDialog::getColor(
        QColor(previous), this, tr("Text Color"));
    if (!selected.isValid()) {
        setColorComboValue(combo, previous);
        return;
    }
    setColorComboValue(combo, selected.name());
}

void SplitConfigWidget::setColorComboValue(
    QComboBox *combo, const QString &colorValue) {
    if (!combo) {
        return;
    }
    const QColor color(colorValue);
    if (!color.isValid()) {
        return;
    }
    const QString normalized = color.name();
    int index = combo->findData(normalized);
    if (index < 0) {
        const QString previousCustom =
            combo->property("customColor").toString();
        const int previousIndex =
            combo->findData(previousCustom);
        if (!previousCustom.isEmpty() &&
            previousIndex >= 0) {
            combo->removeItem(previousIndex);
        }
        const int actionIndex =
            combo->findData(kCustomColorAction);
        const int insertIndex =
            actionIndex >= 0 ? actionIndex : combo->count();
        combo->insertItem(
            insertIndex,
            tr("Selected: %1").arg(normalized),
            normalized);
        combo->setProperty("customColor", normalized);
        index = insertIndex;
    }
    combo->setProperty("selectedColor", normalized);
    const QSignalBlocker blocker(combo);
    combo->setCurrentIndex(index);
}

QString SplitConfigWidget::leftAlignment() const {
    return leftAlignmentCombo_->currentData().toString();
}

QString SplitConfigWidget::rightAlignment() const {
    return rightAlignmentCombo_->currentData().toString();
}

QString SplitConfigWidget::playMode() const {
    return playModeCombo_->currentData().toString();
}

void SplitConfigWidget::assignToLeft(const QString &filename, const QPixmap &thumb) {
    leftFilename_ = filename;
    if (filename.isEmpty()) {
        leftPreview_->clear();
        leftPreview_->setText(tr("Drop media here"));
        leftPreview_->setStyleSheet(
            "QLabel { background: #1e1e2e; border: 2px dashed #555; border-radius: 8px; color: #555; font-size: 12px; }");
        leftFileLabel_->clear();
        return;
    }
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
    if (filename.isEmpty()) {
        rightPreview_->clear();
        rightPreview_->setText(tr("Drop media here"));
        rightPreview_->setStyleSheet(
            "QLabel { background: #1e1e2e; border: 2px dashed #555; border-radius: 8px; color: #555; font-size: 12px; }");
        rightFileLabel_->clear();
        return;
    }
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

void SplitConfigWidget::setMetricSelection(
    const QList<QCheckBox *> &checkboxes,
    const QStringList &metrics) {
    for (QCheckBox *checkbox : checkboxes) {
        const QSignalBlocker blocker(checkbox);
        checkbox->setChecked(
            metrics.contains(
                checkbox->property("protocolLabel").toString()));
    }
}

void SplitConfigWidget::setConfiguration(
    const QString &leftMedia, const QString &rightMedia,
    const QStringList &leftMetrics,
    const QStringList &rightMetrics,
    const QStringList &leftBadges,
    const QStringList &rightBadges,
    const QString &playMode) {
    assignToLeft(leftMedia, {});
    assignToRight(rightMedia, {});
    setMetricSelection(leftMetricCheckboxes_, leftMetrics);
    setMetricSelection(rightMetricCheckboxes_, rightMetrics);
    rebuildMetricsButtonCb(leftMetricsBtn_, leftMetricCheckboxes_,
                           tr("Left"));
    rebuildMetricsButtonCb(rightMetricsBtn_, rightMetricCheckboxes_,
                           tr("Right"));
    {
        const QSignalBlocker blocker(leftCpuBadge_);
        leftCpuBadge_->setChecked(
            leftBadges.contains(QStringLiteral("CPU Badge")));
    }
    {
        const QSignalBlocker blocker(leftGpuBadge_);
        leftGpuBadge_->setChecked(
            leftBadges.contains(QStringLiteral("GPU Badge")));
    }
    {
        const QSignalBlocker blocker(rightCpuBadge_);
        rightCpuBadge_->setChecked(
            rightBadges.contains(QStringLiteral("CPU Badge")));
    }
    {
        const QSignalBlocker blocker(rightGpuBadge_);
        rightGpuBadge_->setChecked(
            rightBadges.contains(QStringLiteral("GPU Badge")));
    }
    const int playModeIndex = playModeCombo_->findData(playMode);
    if (playModeIndex >= 0) {
        const QSignalBlocker blocker(playModeCombo_);
        playModeCombo_->setCurrentIndex(playModeIndex);
    }
}

void SplitConfigWidget::setAreaSettings(
    const QString &leftPosition, const QString &leftColor,
    const QString &leftAlignment, const QString &rightPosition,
    const QString &rightColor, const QString &rightAlignment) {
    const auto setCombo = [](QComboBox *combo, const QString &value) {
        const int index = combo->findData(value);
        if (index >= 0) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(index);
        }
    };
    setCombo(leftPositionCombo_, leftPosition);
    setColorComboValue(leftColorCombo_, leftColor);
    setCombo(leftAlignmentCombo_, leftAlignment);
    setCombo(rightPositionCombo_, rightPosition);
    setColorComboValue(rightColorCombo_, rightColor);
    setCombo(rightAlignmentCombo_, rightAlignment);
}

void SplitConfigWidget::setAvailableMetrics(
    const QStringList &metrics) {
    const auto update = [&metrics](const QList<QCheckBox *> &checkboxes) {
        for (QCheckBox *checkbox : checkboxes) {
            const QString label =
                checkbox->property("protocolLabel").toString();
            checkbox->setEnabled(
                checkbox->isChecked() || metrics.isEmpty() ||
                metrics.contains(label));
        }
    };
    update(leftMetricCheckboxes_);
    update(rightMetricCheckboxes_);
}

void SplitConfigWidget::setPaseMode(bool enabled) {
    const QSignalBlocker blocker(playModeCombo_);
    if (enabled) {
        const int singleIndex =
            playModeCombo_->findData(QStringLiteral("Single"));
        if (singleIndex >= 0) {
            playModeCombo_->setCurrentIndex(singleIndex);
        }
    }
    playModeCombo_->setEnabled(!enabled);
}
