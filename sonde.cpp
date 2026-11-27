#include "sonde.hpp"
#include "dsp.hpp"
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <stdexcept>

QVector<SondeCandidate> parseSondeCandidates(const QByteArray &json, Location origin,
                                              double radiusKm, const QDateTime &now) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Nieprawidłowa odpowiedź SondeHub");
    QVector<SondeCandidate> result;
    const auto objects = doc.object();
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const auto o = it.value().toObject();
        const auto lat = o.value("lat"), lon = o.value("lon"), alt = o.value("alt");
        auto freq = o.value("tx_frequency");
        if (!freq.isDouble()) freq = o.value("frequency");
        const auto received = QDateTime::fromString(o.value("time_received").toString(), Qt::ISODateWithMs);
        if (!lat.isDouble() || !lon.isDouble() || !alt.isDouble() || !freq.isDouble() ||
            !received.isValid() || received.secsTo(now) < -120 || received.secsTo(now) > 1800) continue;
        const Location position{lat.toDouble(), lon.toDouble()};
        const double frequency = freq.toDouble();
        if (position.latitude < -90 || position.latitude > 90 || position.longitude < -180 ||
            position.longitude > 180 || frequency < 400 || frequency > 406) continue;
        const double distance = distanceKm(origin, position);
        if (distance > radiusKm) continue;
        const QString serial = o.value("serial").toString(it.key());
        const QString type = o.value("type").toString("Nieznany");
        result.append({serial, type, qint64(std::llround(frequency * 1000000)), position,
                       alt.toDouble(), distance, received});
    }
    std::sort(result.begin(), result.end(), [](const SondeCandidate &a, const SondeCandidate &b) {
        return a.distance < b.distance;
    });
    return result;
}

SondeTelemetry parseSondeTelemetry(const QString &json) {
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Nieprawidłowa telemetria sondy");
    const auto data = doc.object();
    QString serial = data.value("id").toString().trimmed();
    if (serial.startsWith("M10-", Qt::CaseInsensitive) || serial.startsWith("M20-", Qt::CaseInsensitive))
        serial.remove(0, 4);
    if (serial.isEmpty()) throw std::runtime_error("Telemetria nie zawiera identyfikatora sondy");
    std::optional<Location> position;
    const auto lat = data.value("lat"), lon = data.value("lon");
    if (lat.isDouble() && lon.isDouble() && std::isfinite(lat.toDouble()) && std::isfinite(lon.toDouble()) &&
        lat.toDouble() >= -90 && lat.toDouble() <= 90 && lon.toDouble() >= -180 && lon.toDouble() <= 180)
        position = Location{lat.toDouble(), lon.toDouble()};
    return {serial.toUpper(), data, position};
}

SondeDecoder::SondeDecoder(QObject *parent, QString executable) : QObject(parent), executable_(executable) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &SondeDecoder::readOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        errors_ += process_.readAllStandardError();
        if (errors_.size() > 4096) errors_.remove(0, errors_.size() - 4096);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!stopping_) fail("Błąd dekodera sondy: " + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        if (!stopping_) fail(QString("Dekoder sondy zakończył pracę (kod %1): %2")
                             .arg(code).arg(QString::fromLocal8Bit(errors_).trimmed()));
    });
}

SondeDecoder::~SondeDecoder() { stop(); }

void SondeDecoder::start(qint64 frequency, SondeType type, double shiftHz) {
    stop();
    active_ = true;
    frequency_ = frequency;
    type_ = type;
    const QString name = type == SondeType::RS41 ? "rs41mod" :
                         type == SondeType::M20 ? "m10m20mod" : "m10mod";
    const auto executable = executable_.isEmpty() ?
        QDir::home().filePath("sdr-touch-rs/demod/mod/" + name) : executable_;
    const qint64 centerFrequency = type == SondeType::M10 ? frequency : frequency + std::llround(shiftHz);
    QStringList args{"--json", "--ptu", "--jsn_cfq", QString::number(centerFrequency)};
    if (type != SondeType::M10) {
        if (type == SondeType::RS41) args.prepend("--auto");
        args.prepend("--dc");
        args.prepend(type == SondeType::RS41 ? "7.4" : "24");
        args.prepend("--lpbw");
        args.prepend(QString::number(-shiftHz / kSampleRate, 'g', 12));
        args.prepend("--IQ");
        args << "-" << QString::number(kSampleRate) << "32";
    } else {
        args.prepend("--dc");
    }
    process_.start(executable, args);
    if (type != SondeType::M10) return;
    QByteArray header(44, '\0');
    header.replace(0, 4, "RIFF");
    qToLittleEndian<quint32>(0xffffffff, reinterpret_cast<uchar *>(header.data() + 4));
    header.replace(8, 8, "WAVEfmt ");
    qToLittleEndian<quint32>(16, reinterpret_cast<uchar *>(header.data() + 16));
    qToLittleEndian<quint16>(1, reinterpret_cast<uchar *>(header.data() + 20));
    qToLittleEndian<quint16>(1, reinterpret_cast<uchar *>(header.data() + 22));
    qToLittleEndian<quint32>(48000, reinterpret_cast<uchar *>(header.data() + 24));
    qToLittleEndian<quint32>(96000, reinterpret_cast<uchar *>(header.data() + 28));
    qToLittleEndian<quint16>(2, reinterpret_cast<uchar *>(header.data() + 32));
    qToLittleEndian<quint16>(16, reinterpret_cast<uchar *>(header.data() + 34));
    header.replace(36, 4, "data");
    qToLittleEndian<quint32>(0xffffffff, reinterpret_cast<uchar *>(header.data() + 40));
    if (process_.write(header) != header.size()) fail("Nie można przesłać nagłówka audio do dekodera sondy.");
}

void SondeDecoder::stop() {
    active_ = false;
    stopping_ = true;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    pending_.clear();
    errors_.clear();
    stopping_ = false;
}

void SondeDecoder::fail(const QString &message) {
    if (!active_) return;
    stop();
    emit failed(message);
}

void SondeDecoder::gap() {
    if (active_) fail("Utracono próbki odbiornika; uruchom odbiór sondy ponownie.");
}

void SondeDecoder::feed(const QByteArray &stereo) {
    if (!active_) return;
    if (type_ != SondeType::M10) { fail("Nieprawidłowy tor audio dekodera sondy."); return; }
    if (stereo.size() % 4) { fail("Nieprawidłowy format próbek sondy."); return; }
    if (process_.bytesToWrite() + stereo.size() / 2 > 192000) {
        fail("Przepełnienie bufora dekodera sondy; uruchom odbiór ponownie.");
        return;
    }
    QByteArray mono(stereo.size() / 2, Qt::Uninitialized);
    for (int i = 0; i < stereo.size() / 4; ++i) {
        mono[2 * i] = stereo[4 * i];
        mono[2 * i + 1] = stereo[4 * i + 1];
    }
    if (!mono.isEmpty() && process_.write(mono) != mono.size())
        fail("Nie można przesłać próbek do dekodera sondy.");
}

void SondeDecoder::feedIq(const QByteArray &iq) {
    if (!active_) return;
    if (type_ == SondeType::M10 || iq.size() % (2 * int(sizeof(float)))) {
        fail("Nieprawidłowy format próbek IQ sondy.");
        return;
    }
    if (process_.bytesToWrite() + iq.size() > 2 * kSampleRate * 2 * int(sizeof(float))) {
        fail("Przepełnienie bufora dekodera sondy; uruchom odbiór ponownie.");
        return;
    }
    if (!iq.isEmpty() && process_.write(iq) != iq.size())
        fail("Nie można przesłać próbek IQ do dekodera sondy.");
}

void SondeDecoder::readOutput() {
    if (!active_) return;
    pending_ += process_.readAllStandardOutput();
    if (pending_.size() > 65536) { fail("Zbyt długi komunikat dekodera sondy."); return; }
    int newline;
    while ((newline = pending_.indexOf('\n')) >= 0) {
        const auto line = pending_.left(newline).trimmed();
        pending_.remove(0, newline + 1);
        if (!line.startsWith('{')) continue;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            fail("Nieprawidłowa telemetria dekodera sondy.");
            return;
        }
        emit telemetry(QString::fromUtf8(QJsonDocument(doc.object()).toJson(QJsonDocument::Compact)));
    }
}
