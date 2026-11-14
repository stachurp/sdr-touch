#pragma once

#include "dsp.hpp"
#include <QByteArray>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <cstdint>

struct FrontendSettings {
    bool agc = true;
    int rfReduction = 0;
    int ifReduction = 50;
    int agcSetpoint = -30;
    double ppm = 0;
    bool operator==(const FrontendSettings &other) const {
        return agc == other.agc && rfReduction == other.rfReduction &&
               ifReduction == other.ifReduction && agcSetpoint == other.agcSetpoint && ppm == other.ppm;
    }
    bool operator!=(const FrontendSettings &other) const { return !(*this == other); }
};

struct ReceiverSettings {
    std::int64_t frequency = 7100000;
    Mode mode = Mode::LSB;
    bool scan = false;
    std::int64_t scanStart = 7000000;
    std::int64_t scanEnd = 7200000;
    std::int64_t scanStep = 1000;
    double threshold = -60;
    quint64 scanId = 0;
    FrontendSettings frontend;
};

class Receiver : public QThread {
    Q_OBJECT
public:
    explicit Receiver(QString resultPath, QObject *parent = nullptr);
    ~Receiver() override;
    void configure(const ReceiverSettings &settings);
    void restart();
    void stop();

signals:
    void statusChanged(const QString &status);
    void failed(const QString &message);
    void audioReady(const QByteArray &pcm, qint64 captureEndMs, qint64 frequency, int mode);
    void audioGap();
    void signalLevel(double dbfs);
    void detected(quint64 scanId, qint64 frequency, double dbfs, const QString &mode);
    void scanFinished(quint64 scanId);
    void blockReceived();
    void frontendApplied();

protected:
    void run() override;

private:
    QString resultPath_;
    QMutex mutex_;
    ReceiverSettings settings_;
    std::uint64_t revision_ = 1;
    std::atomic<bool> stopping_{false};
};
