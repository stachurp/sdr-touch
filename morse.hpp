#pragma once

#include "pcm_resampler.hpp"
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTextCodec>
#include <memory>

class MorseDecoder : public QObject {
    Q_OBJECT
public:
    explicit MorseDecoder(QObject *parent = nullptr, QString executable = "multimon-ng");
    ~MorseDecoder() override;
    void configure(bool enabled, qint64 frequency);
    void feed(const QByteArray &stereo);
    void gap();
    bool isRunning() const { return enabled_ && process_.state() == QProcess::Running; }

signals:
    void textReceived(const QString &text);
    void started(qint64 frequency);
    void statusChanged(const QString &message);
    void failed(const QString &message);

private:
    void stop();
    void fail(const QString &message);
    QProcess process_;
    QString executable_;
    PcmResampler resampler_;
    std::unique_ptr<QTextDecoder> textDecoder_;
    qint64 frequency_ = 0;
    bool enabled_ = false;
    bool stopping_ = false;
};
