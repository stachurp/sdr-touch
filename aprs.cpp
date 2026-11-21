#include "aprs.hpp"
#include <QRegularExpression>
#include <utility>

AprsDecoder::AprsDecoder(QObject *parent, QString executable)
    : QObject(parent), executable_(std::move(executable)) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &AprsDecoder::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto error = process_.readAllStandardError().trimmed();
        if (!error.isEmpty()) emit statusChanged("APRS: " + QString::fromLocal8Bit(error.left(512)));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!stopping_) fail("Błąd dekodera APRS: " + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        if (!stopping_ && enabled_) fail(QString("Dekoder APRS zakończył pracę (kod %1).").arg(code));
    });
}

AprsDecoder::~AprsDecoder() { stop(); }

void AprsDecoder::stop() {
    stopping_ = true;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    pending_.clear();
    resampler_.reset();
    stopping_ = false;
}

void AprsDecoder::configure(bool enabled, qint64 frequency) {
    if (enabled_ == enabled && (!enabled || frequency_ == frequency)) return;
    stop();
    enabled_ = enabled;
    frequency_ = frequency;
    if (!enabled_) return;
    process_.start(executable_, {"-q", "-c", "-a", "AFSK1200", "-A", "-t", "raw", "-"});
    emit statusChanged("APRS: oczekiwanie na pakiety 1200 AFSK");
}

void AprsDecoder::fail(const QString &message) {
    if (!enabled_) return;
    enabled_ = false;
    stop();
    emit failed(message);
}

void AprsDecoder::gap() {
    if (enabled_) fail("Utracono próbki APRS; włącz dekoder ponownie.");
}

void AprsDecoder::feed(const QByteArray &stereo) {
    if (!enabled_) return;
    if (stereo.size() % 4 != 0) {
        fail("Nieprawidłowy format próbek APRS.");
        return;
    }
    if (process_.bytesToWrite() + stereo.size() / 2 > 88200) {
        fail("Przepełnienie bufora APRS; włącz dekoder ponownie.");
        return;
    }
    const QByteArray mono = resampler_.convert(stereo);
    if (!mono.isEmpty() && process_.write(mono) != mono.size())
        fail("Nie można przesłać próbek do dekodera APRS.");
}

void AprsDecoder::readOutput() {
    if (stopping_) return;
    pending_ += process_.readAllStandardOutput();
    if (pending_.size() > 65536) {
        fail("Zbyt długi komunikat dekodera APRS.");
        return;
    }
    static const QRegularExpression packet("^[A-Za-z0-9-]+>[A-Za-z0-9,*-]+:");
    int newline;
    while ((newline = pending_.indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(pending_.left(newline)).trimmed();
        pending_.remove(0, newline + 1);
        if (line.startsWith("APRS: ")) line.remove(0, 6);
        if (!packet.match(line).hasMatch()) continue;
        for (int i = 0; i < line.size(); ++i)
            if (line.at(i).category() == QChar::Other_Control) line[i] = QChar::ReplacementCharacter;
        emit packetReceived(line, frequency_);
    }
}
