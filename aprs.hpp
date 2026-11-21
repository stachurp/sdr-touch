#pragma once

#include "pcm_resampler.hpp"
#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

class AprsDecoder : public QObject {
    Q_OBJECT
public:
    explicit AprsDecoder(QObject *parent = nullptr, QString executable = "multimon-ng");
    ~AprsDecoder() override;
    void configure(bool enabled, qint64 frequency);
    void feed(const QByteArray &stereo);
    void gap();
    bool isRunning() const { return process_.state() == QProcess::Running; }

signals:
    void packetReceived(const QString &packet, qint64 frequency);
    void statusChanged(const QString &message);
    void failed(const QString &message);

private:
    void stop();
    void fail(const QString &message);
    void readOutput();
    QProcess process_;
    QString executable_;
    QByteArray pending_;
    qint64 frequency_ = 0;
    PcmResampler resampler_;
    bool enabled_ = false;
    bool stopping_ = false;
};
