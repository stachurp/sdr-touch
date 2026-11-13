#include "voice.hpp"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtEndian>

VoiceDecoder::VoiceDecoder(QObject *parent, QString executable)
    : QObject(parent), executable_(executable.isEmpty() ?
          QDir::home().filePath("sdr-touch-dsd-fme/build/dsd-fme") : QFileInfo(executable).absoluteFilePath()) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &VoiceDecoder::readAudio);
    connect(&process_, &QProcess::readyReadStandardError, this, &VoiceDecoder::readMetadata);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!stopping_) fail("Błąd procesu DSD-FME: " + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        if (!stopping_ && mode_ != VoiceMode::Off)
            fail(QString("DSD-FME zakończył pracę (kod %1)").arg(code));
    });
}

VoiceDecoder::~VoiceDecoder() { stop(); }

void VoiceDecoder::stop() {
    stopping_ = true;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    audioPending_.clear();
    logPending_.clear();
    color_.clear();
    call_.clear();
    havePrevious_ = false;
    stopping_ = false;
}

void VoiceDecoder::configure(VoiceMode mode, qint64 frequency) {
    if (mode_ == mode && (mode == VoiceMode::Off || frequency_ == frequency)) return;
    stop();
    mode_ = mode;
    frequency_ = frequency;
    emit metadataChanged("Brak danych");
    if (mode == VoiceMode::Off) return;
    if (!directory_.isValid()) {
        fail("Nie można utworzyć katalogu tymczasowego dekodera.");
        return;
    }
    process_.setWorkingDirectory(directory_.path());
    process_.start(executable_, {mode == VoiceMode::DMR ? "-fs" : "-fy", "-i", "-", "-o", "-"});
    emit statusChanged("Oczekiwanie na sygnał cyfrowy");
}

void VoiceDecoder::fail(const QString &message) {
    if (mode_ == VoiceMode::Off) return;
    stop();
    mode_ = VoiceMode::Off;
    emit failed(message);
}

void VoiceDecoder::gap() {
    if (mode_ != VoiceMode::Off) fail("Utracono próbki odbiornika; włącz dekoder ponownie.");
}

void VoiceDecoder::feed(const QByteArray &stereo) {
    if (mode_ == VoiceMode::Off) return;
    if (stereo.size() % 4 != 0) {
        fail("Nieprawidłowy format próbek dekodera.");
        return;
    }
    if (process_.bytesToWrite() + stereo.size() / 2 > 96000) {
        fail("Przepełnienie bufora dekodera; włącz dekoder ponownie.");
        return;
    }
    QByteArray mono(stereo.size() / 2, Qt::Uninitialized);
    for (int i = 0; i < stereo.size() / 4; ++i) {
        mono[2 * i] = stereo[4 * i];
        mono[2 * i + 1] = stereo[4 * i + 1];
    }
    if (process_.write(mono) != mono.size()) fail("Nie można przesłać próbek do DSD-FME.");
}

void VoiceDecoder::readAudio() {
    if (stopping_) return;
    audioPending_ += process_.readAllStandardOutput();
    if (audioPending_.size() > 32000) {
        fail("Przepełnienie bufora audio dekodera.");
        return;
    }
    const int frameSize = mode_ == VoiceMode::YSF ? 2 : 4;
    const int frames = audioPending_.size() / frameSize;
    QByteArray output;
    output.reserve(frames * 24);
    for (int i = 0; i < frames; ++i) {
        const auto *p = reinterpret_cast<const uchar *>(audioPending_.constData() + i * frameSize);
        const qint16 left = qFromLittleEndian<qint16>(p);
        const qint16 right = frameSize == 2 ? left : qFromLittleEndian<qint16>(p + 2);
        if (!havePrevious_) {
            previousLeft_ = left;
            previousRight_ = right;
            havePrevious_ = true;
        }
        for (int j = 1; j <= 6; ++j) {
            const qint16 l = qint16((int(previousLeft_) * (6 - j) + int(left) * j) / 6);
            const qint16 r = qint16((int(previousRight_) * (6 - j) + int(right) * j) / 6);
            char frame[4];
            qToLittleEndian(l, reinterpret_cast<uchar *>(frame));
            qToLittleEndian(r, reinterpret_cast<uchar *>(frame + 2));
            output.append(frame, 4);
        }
        previousLeft_ = left;
        previousRight_ = right;
    }
    audioPending_.remove(0, frames * frameSize);
    if (!output.isEmpty()) emit audioReady(output);
}

void VoiceDecoder::readMetadata() {
    if (stopping_) return;
    logPending_ += process_.readAllStandardError();
    if (logPending_.size() > 65536) {
        fail("Zbyt długi komunikat DSD-FME.");
        return;
    }
    static const QRegularExpression color("Color Code=(\\d{1,2})");
    static const QRegularExpression dmr("SLOT ([12]) TGT=(\\d+) SRC=(\\d+)");
    static const QRegularExpression ysf("SRC:\\s*([A-Za-z0-9/\\-]+)");
    int newline;
    while ((newline = logPending_.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(logPending_.left(newline));
        logPending_.remove(0, newline + 1);
        if (line.contains("CRC ERR") || line.contains("FEC ERR")) continue;
        if (mode_ == VoiceMode::DMR) {
            const auto c = color.match(line);
            if (c.hasMatch()) color_ = "Color code: " + c.captured(1);
            const auto m = dmr.match(line);
            if (m.hasMatch())
                call_ = QString("Slot %1 | Cel: %2 | Źródło: %3").arg(m.captured(1), m.captured(2), m.captured(3));
        } else if (mode_ == VoiceMode::YSF) {
            const auto m = ysf.match(line);
            if (m.hasMatch()) call_ = "Znak źródłowy: " + m.captured(1);
        }
        if (!color_.isEmpty() || !call_.isEmpty())
            emit metadataChanged(color_.isEmpty() ? call_ : color_ + (call_.isEmpty() ? "" : " | " + call_));
    }
}
