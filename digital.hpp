#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <memory>
#include <optional>
#include <vector>

enum class DigitalMode { Off, FT8, FT4, WSPR };

struct DigitalMessage {
    QString time;
    QString text;
    QString station;
    QString grid;
    int snr;
    double frequencyHz;
    DigitalMode mode;
};

std::optional<DigitalMessage> parseDigitalLine(DigitalMode mode, const QString &line, double dialHz);
qint64 slotLengthSamples(DigitalMode mode);

struct AudioSlot {
    qint64 startSample;
    QByteArray pcm;
};

class SlotAssembler {
public:
    explicit SlotAssembler(DigitalMode mode);
    std::vector<AudioSlot> feed(const QByteArray &pcmStereo, qint64 estimatedEndMs, bool &discontinuity);
private:
    qint64 period_;
    qint64 nextSample_ = -1;
    qint64 slotStart_ = -1;
    QByteArray samples_;
};

class DigitalDecoder : public QObject {
    Q_OBJECT
public:
    explicit DigitalDecoder(QObject *parent = nullptr);
    ~DigitalDecoder() override;
    void configure(DigitalMode mode, qint64 dialFrequency);
    void feed(const QByteArray &pcmStereo, qint64 captureEndMs);
    void resetCapture();

signals:
    void decoded(const DigitalMessage &message);
    void statusChanged(const QString &message);
    void failed(const QString &message);

private:
    void startDecode(const AudioSlot &slot);
    void finishDecode(int exitCode, QProcess::ExitStatus exitStatus);
    QTemporaryDir directory_;
    QProcess process_;
    std::unique_ptr<SlotAssembler> assembler_;
    DigitalMode mode_ = DigitalMode::Off;
    DigitalMode processMode_ = DigitalMode::Off;
    qint64 frequency_ = 0;
    qint64 processFrequency_ = 0;
    QString wavFile_;
};
