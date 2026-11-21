#include "morse.hpp"
#include <utility>

MorseDecoder::MorseDecoder(QObject *parent, QString executable)
    : QObject(parent), executable_(std::move(executable)),
      textDecoder_(QTextCodec::codecForName("UTF-8")->makeDecoder()) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        if (stopping_) return;
        const QByteArray bytes = process_.readAllStandardOutput();
        emit textReceived(textDecoder_->toUnicode(bytes));
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto error = process_.readAllStandardError().trimmed();
        if (!error.isEmpty()) emit statusChanged("CW: " + QString::fromLocal8Bit(error.left(512)));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!stopping_) fail("Błąd dekodera CW: " + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        if (!stopping_ && enabled_) fail(QString("Dekoder CW zakończył pracę (kod %1).").arg(code));
    });
}

MorseDecoder::~MorseDecoder() { stop(); }

void MorseDecoder::stop() {
    stopping_ = true;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    resampler_.reset();
    textDecoder_.reset(QTextCodec::codecForName("UTF-8")->makeDecoder());
    stopping_ = false;
}

void MorseDecoder::configure(bool enabled, qint64 frequency) {
    if (enabled_ == enabled && (!enabled || frequency_ == frequency)) return;
    stop();
    enabled_ = enabled;
    frequency_ = frequency;
    if (!enabled_) return;
    process_.start(executable_, {"-q", "-c", "-a", "MORSE_CW", "-t", "raw", "-"});
    emit started(frequency_);
    emit statusChanged("CW: oczekiwanie na sygnał Morse'a");
}

void MorseDecoder::fail(const QString &message) {
    if (!enabled_) return;
    enabled_ = false;
    stop();
    emit failed(message);
}

void MorseDecoder::gap() {
    if (!enabled_) return;
    const qint64 frequency = frequency_;
    configure(false, frequency);
    configure(true, frequency);
    emit statusChanged("CW: utracono próbki, dekoder uruchomiono ponownie.");
}

void MorseDecoder::feed(const QByteArray &stereo) {
    if (!enabled_) return;
    if (stereo.size() % 4 != 0) {
        fail("Nieprawidłowy format próbek CW.");
        return;
    }
    if (process_.bytesToWrite() + stereo.size() / 2 > 88200) {
        fail("Przepełnienie bufora CW; wybierz ponownie tryb CW.");
        return;
    }
    const QByteArray mono = resampler_.convert(stereo);
    if (!mono.isEmpty() && process_.write(mono) != mono.size())
        fail("Nie można przesłać próbek do dekodera CW.");
}
