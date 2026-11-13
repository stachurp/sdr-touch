#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>

enum class VoiceMode { Off, DMR, YSF };

class VoiceDecoder : public QObject {
    Q_OBJECT
public:
    explicit VoiceDecoder(QObject *parent = nullptr, QString executable = {});
    ~VoiceDecoder() override;
    void configure(VoiceMode mode, qint64 frequency);
    void feed(const QByteArray &stereo);
    void gap();
    bool isRunning() const { return process_.state() == QProcess::Running; }

signals:
    void audioReady(const QByteArray &stereo48k);
    void metadataChanged(const QString &text);
    void statusChanged(const QString &text);
    void failed(const QString &text);

private:
    void stop();
    void fail(const QString &message);
    void readAudio();
    void readMetadata();
    QProcess process_;
    QTemporaryDir directory_;
    QString executable_;
    VoiceMode mode_ = VoiceMode::Off;
    qint64 frequency_ = 0;
    QByteArray audioPending_;
    QByteArray logPending_;
    QString color_;
    QString call_;
    qint16 previousLeft_ = 0;
    qint16 previousRight_ = 0;
    bool havePrevious_ = false;
    bool stopping_ = false;
};
