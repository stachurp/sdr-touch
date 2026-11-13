#include "digital.hpp"
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>
#include <stdexcept>

namespace {
constexpr int outputRate = 12000;
constexpr int inputRate = 48000;
constexpr qint64 dspLatencyMs = 125;

bool validCall(const QString &token) {
    static const QRegularExpression call("^<?[A-Z0-9]+(/[A-Z0-9]+)*>?$");
    static const QRegularExpression digit("[0-9]");
    static const QRegularExpression letter("[A-Z]");
    return token != "RR73" && token != "RRR" && token != "73" &&
        call.match(token).hasMatch() && digit.match(token).hasMatch() && letter.match(token).hasMatch();
}

bool validGrid(const QString &token) {
    static const QRegularExpression grid("^[A-R]{2}[0-9]{2}([A-X]{2})?$");
    return token != "RR73" && grid.match(token).hasMatch();
}

QString stationFromMessage(const QStringList &tokens, int gridIndex) {
    if (tokens.isEmpty()) return {};
    if (tokens[0] == "CQ" || tokens[0] == "QRZ" || tokens[0] == "DE") {
        int end = gridIndex >= 0 ? gridIndex : tokens.size();
        for (int i = end - 1; i >= 1; --i) if (validCall(tokens[i])) return tokens[i];
    } else if (tokens.size() >= 2 && validCall(tokens[0]) && validCall(tokens[1])) {
        return tokens[1];
    }
    return {};
}
}

qint64 slotLengthSamples(DigitalMode mode) {
    switch (mode) {
    case DigitalMode::FT8: return 15 * outputRate;
    case DigitalMode::FT4: return 15 * outputRate / 2;
    case DigitalMode::WSPR: return 120 * outputRate;
    case DigitalMode::Off: throw std::invalid_argument("Digital mode is off");
    }
    throw std::invalid_argument("Invalid digital mode");
}

std::optional<DigitalMessage> parseDigitalLine(DigitalMode mode, const QString &line, double dialHz) {
    static const QRegularExpression ft(
        "^\\s*(\\d{6})\\s+(-?\\d+)\\s+[-+]?\\d+(?:\\.\\d+)?\\s+(\\d+)\\s+[~+]\\s+(.+?)\\s*$");
    static const QRegularExpression wspr(
        "^\\s*(\\d{4})\\s+(-?\\d+)\\s+[-+]?\\d+(?:\\.\\d+)?\\s+(\\d+\\.\\d+)\\s+[-+]?\\d+\\s+(.+?)\\s*$");
    const auto match = (mode == DigitalMode::WSPR ? wspr : ft).match(line);
    if (!match.hasMatch()) return std::nullopt;
    bool snrOk = false, freqOk = false;
    const int snr = match.captured(2).toInt(&snrOk);
    double number = match.captured(3).toDouble(&freqOk);
    if (!snrOk || !freqOk || !std::isfinite(number)) return std::nullopt;
    const QString text = match.captured(4).simplified();
    const auto tokens = text.toUpper().split(' ', Qt::SkipEmptyParts);
    int gridIndex = -1;
    for (int i = 0; i < tokens.size(); ++i) {
        if (validGrid(tokens[i])) { gridIndex = i; break; }
    }
    QString station;
    if (mode == DigitalMode::WSPR) {
        if (!tokens.isEmpty() && validCall(tokens[0])) station = tokens[0];
    } else {
        station = stationFromMessage(tokens, gridIndex);
    }
    return DigitalMessage{match.captured(1), text, station, gridIndex < 0 ? QString() : tokens[gridIndex],
                          snr, mode == DigitalMode::WSPR ? number * 1000000.0 : dialHz + number, mode};
}

SlotAssembler::SlotAssembler(DigitalMode mode) : period_(slotLengthSamples(mode)) {}

std::vector<AudioSlot> SlotAssembler::feed(const QByteArray &pcmStereo, qint64 estimatedEndMs,
                                            bool &discontinuity) {
    if (pcmStereo.size() % 16 != 0 || pcmStereo.isEmpty() || estimatedEndMs < 0)
        throw std::invalid_argument("Nieprawidlowy blok audio");
    const qint64 frames = pcmStereo.size() / 4;
    const qint64 outputSamples = frames / (inputRate / outputRate);
    const qint64 observedStart = estimatedEndMs * (outputRate / 1000) - outputSamples;
    discontinuity = false;
    if (nextSample_ < 0) nextSample_ = observedStart;
    if (std::abs(observedStart - nextSample_) > outputRate / 10) {
        discontinuity = true;
        samples_.clear();
        slotStart_ = -1;
        nextSample_ = observedStart;
    }
    std::vector<AudioSlot> result;
    for (int i = 0; i < pcmStereo.size(); i += 16, ++nextSample_) {
        if (slotStart_ < 0 && nextSample_ % period_ == 0) {
            slotStart_ = nextSample_;
            samples_.reserve(int(period_ * 2));
        }
        if (slotStart_ >= 0) {
            samples_.append(pcmStereo.constData() + i, 2);
            if (samples_.size() == period_ * 2) {
                result.push_back({slotStart_, std::move(samples_)});
                samples_.clear();
                slotStart_ = -1;
            }
        }
    }
    return result;
}

DigitalDecoder::DigitalDecoder(QObject *parent) : QObject(parent), process_(this) {
    if (!directory_.isValid()) throw std::runtime_error("Nie mozna utworzyc katalogu dekodera");
    process_.setWorkingDirectory(directory_.path());
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &DigitalDecoder::finishDecode);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit failed("Nie mozna uruchomic dekodera: " + process_.errorString());
    });
}

DigitalDecoder::~DigitalDecoder() {
    disconnect(&process_, nullptr, this, nullptr);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}

void DigitalDecoder::configure(DigitalMode mode, qint64 dialFrequency) {
    if (mode == mode_ && dialFrequency == frequency_) return;
    if (process_.state() != QProcess::NotRunning) {
        disconnect(&process_, nullptr, this, nullptr);
        process_.kill();
        process_.waitForFinished(3000);
        connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, &DigitalDecoder::finishDecode);
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                emit failed("Nie mozna uruchomic dekodera: " + process_.errorString());
        });
    }
    if (!wavFile_.isEmpty()) { QFile::remove(wavFile_); wavFile_.clear(); }
    mode_ = mode;
    frequency_ = dialFrequency;
    assembler_ = mode == DigitalMode::Off ? nullptr : std::make_unique<SlotAssembler>(mode);
    emit statusChanged(mode == DigitalMode::Off ? "Dekodowanie wyłączone" :
                       "Oczekiwanie na początek pełnego przedziału UTC");
}

void DigitalDecoder::feed(const QByteArray &pcmStereo, qint64 captureEndMs) {
    if (!assembler_) return;
    try {
        bool discontinuity = false;
        const auto completed = assembler_->feed(pcmStereo, captureEndMs - dspLatencyMs, discontinuity);
        if (discontinuity) emit statusChanged("Przerwa w audio: oczekiwanie na następny pełny przedział");
        for (const auto &slot : completed) startDecode(slot);
    } catch (const std::exception &error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}

void DigitalDecoder::resetCapture() {
    if (!assembler_) return;
    assembler_ = std::make_unique<SlotAssembler>(mode_);
    emit statusChanged("Utracono próbki: oczekiwanie na kolejny przedział");
}

void DigitalDecoder::startDecode(const AudioSlot &slot) {
    if (process_.state() != QProcess::NotRunning) {
        emit statusChanged("Dekoder zajęty: pominięto przedział");
        return;
    }
    const QString executable = QStandardPaths::findExecutable(mode_ == DigitalMode::WSPR ? "wsprd" : "jt9");
    if (executable.isEmpty()) {
        emit failed("Brak programu dekodera WSJT-X");
        return;
    }
    const QDateTime start = QDateTime::fromMSecsSinceEpoch(slot.startSample / 12, Qt::UTC);
    wavFile_ = QDir(directory_.path()).filePath(start.toString(mode_ == DigitalMode::WSPR ?
                                                                "yyMMdd_HHmm" : "yyMMdd_HHmmss") + ".wav");
    QFile file(wavFile_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed("Nie mozna utworzyc pliku audio: " + file.errorString());
        return;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + slot.pcm.size());
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(outputRate) << quint32(outputRate * 2)
        << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(slot.pcm.size());
    if (out.writeRawData(slot.pcm.constData(), slot.pcm.size()) != slot.pcm.size() ||
        out.status() != QDataStream::Ok || !file.flush()) {
        emit failed("Blad zapisu pliku audio: " + file.errorString());
        return;
    }
    file.close();
    processMode_ = mode_;
    processFrequency_ = frequency_;
    QStringList args;
    if (mode_ == DigitalMode::WSPR) {
        args << "-a" << directory_.path() << "-f" << QString::number(frequency_ / 1000000.0, 'f', 6) << wavFile_;
    } else {
        args << (mode_ == DigitalMode::FT8 ? "-8" : "-5") << "-p"
             << (mode_ == DigitalMode::FT8 ? "15" : "7") << "-a" << directory_.path()
             << "-t" << directory_.path() << "-e" << QFileInfo(executable).absolutePath() << wavFile_;
    }
    emit statusChanged("Dekodowanie przedziału " + start.toString("HH:mm:ss") + " UTC");
    process_.start(executable, args);
}

void DigitalDecoder::finishDecode(int exitCode, QProcess::ExitStatus exitStatus) {
    const QString output = QString::fromUtf8(process_.readAllStandardOutput());
    const QString errors = QString::fromUtf8(process_.readAllStandardError()).trimmed();
    if (!wavFile_.isEmpty()) { QFile::remove(wavFile_); wavFile_.clear(); }
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        emit failed(QString("Dekoder zakończył się błędem %1: %2").arg(exitCode).arg(errors));
        return;
    }
    int count = 0;
    for (const auto &line : output.split('\n')) {
        const auto message = parseDigitalLine(processMode_, line, processFrequency_);
        if (message) { emit decoded(*message); ++count; }
    }
    emit statusChanged(QString("Dekodowanie zakończone: %1 wiadomości").arg(count));
}
