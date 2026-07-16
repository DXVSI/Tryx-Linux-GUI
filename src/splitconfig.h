#pragma once

#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QWidgetAction>
#include <QCheckBox>
#include <QPixmap>
#include <QStringList>

class SplitConfigWidget : public QWidget {
    Q_OBJECT
public:
    explicit SplitConfigWidget(QWidget *parent = nullptr);

    QStringList leftMedia() const;
    QStringList rightMedia() const;
    QStringList leftMetrics() const;
    QStringList rightMetrics() const;
    QStringList leftBadges() const;
    QStringList rightBadges() const;
    QString leftPosition() const;
    QString rightPosition() const;
    QString leftColor() const;
    QString rightColor() const;
    QString leftAlignment() const;
    QString rightAlignment() const;
    QString playMode() const;

    void assignToLeft(const QString &filename, const QPixmap &thumb);
    void assignToRight(const QString &filename, const QPixmap &thumb);
    void setConfiguration(const QString &leftMedia,
                          const QString &rightMedia,
                          const QStringList &leftMetrics,
                          const QStringList &rightMetrics,
                          const QStringList &leftBadges,
                          const QStringList &rightBadges,
                          const QString &playMode);
    void setAreaSettings(const QString &leftPosition,
                         const QString &leftColor,
                         const QString &leftAlignment,
                         const QString &rightPosition,
                         const QString &rightColor,
                         const QString &rightAlignment);
    void setAvailableMetrics(const QStringList &metrics);
    void setPaseMode(bool enabled);

private:
    void setupUi();
    void rebuildMetricsButtonCb(QToolButton *btn, const QList<QCheckBox *> &checkboxes, const QString &side);
    void setMetricSelection(const QList<QCheckBox *> &checkboxes,
                            const QStringList &metrics);
    void chooseCustomColor(QComboBox *combo);
    void setColorComboValue(QComboBox *combo,
                            const QString &color);
    static QString colorComboValue(const QComboBox *combo);
    static QStringList checkedBadges(QCheckBox *cpu, QCheckBox *gpu);

    // Preview frames
    QLabel *leftPreview_;
    QLabel *rightPreview_;
    QLabel *leftFileLabel_;
    QLabel *rightFileLabel_;

    // Settings
    QComboBox *playModeCombo_;
    QToolButton *leftMetricsBtn_;
    QToolButton *rightMetricsBtn_;
    QMenu *leftMetricsMenu_;
    QMenu *rightMetricsMenu_;
    QList<QCheckBox *> leftMetricCheckboxes_;
    QList<QCheckBox *> rightMetricCheckboxes_;
    QCheckBox *leftCpuBadge_;
    QCheckBox *leftGpuBadge_;
    QCheckBox *rightCpuBadge_;
    QCheckBox *rightGpuBadge_;
    QComboBox *leftPositionCombo_;
    QComboBox *leftColorCombo_;
    QComboBox *leftAlignmentCombo_;
    QComboBox *rightPositionCombo_;
    QComboBox *rightColorCombo_;
    QComboBox *rightAlignmentCombo_;

    // Media assignments
    QString leftFilename_;
    QString rightFilename_;
};
