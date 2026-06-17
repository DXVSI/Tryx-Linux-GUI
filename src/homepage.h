#pragma once

#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QTimer>
#include <QFrame>
#include <QVBoxLayout>
#include <QPainter>
#include <QVector>

#include "systemmonitor.h"

// Custom widget: semi-circular gauge for CPU/GPU
class GaugeWidget : public QWidget {
    Q_OBJECT
public:
    explicit GaugeWidget(QWidget *parent = nullptr);
    void setValue(double percent);
    double value() const { return value_; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double value_ = 0.0;
};

// Custom widget: rolling line graph for network
class GraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit GraphWidget(const QColor &lineColor, QWidget *parent = nullptr);
    void addValue(double val);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<double> data_;
    QColor lineColor_;
    static const int MAX_POINTS = 30;
};

class Homepage : public QWidget {
    Q_OBJECT
public:
    explicit Homepage(QWidget *parent = nullptr);

    void setCurrentLanguage(const QString &language);

signals:
    void languageChanged(const QString &language);

private slots:
    void onMetricsUpdated(const SystemMetrics &metrics);

private:
    void setupUi();
    QFrame *createCard();
    void populateLanguageCombo();

    SystemMonitor *monitor_;
    QTimer *updateTimer_;
    QComboBox *languageCombo_;

    // CPU card
    QLabel *cpuUsageLabel_;
    QLabel *cpuTempLabel_;
    GaugeWidget *cpuGauge_;

    // GPU card
    QLabel *gpuUsageLabel_;
    QLabel *gpuTempLabel_;
    GaugeWidget *gpuGauge_;

    // Memory card
    QLabel *memUsageLabel_;
    QLabel *memDetailLabel_;
    QFrame *memBar_;
    QFrame *memBarFill_;

    // Disk card
    QLabel *diskUsageLabel_;
    QLabel *diskDetailLabel_;
    QFrame *diskBar_;
    QFrame *diskBarFill_;

    // Network card
    QLabel *netDownloadLabel_;
    QLabel *netUploadLabel_;
    GraphWidget *downloadGraph_;
    GraphWidget *uploadGraph_;
};
