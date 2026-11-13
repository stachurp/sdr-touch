#include "adsb.hpp"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>
#include <stdexcept>

namespace {
std::optional<double> number(const QJsonObject &object, const char *key) {
    const auto value = object.value(QLatin1String(key));
    if (value.isUndefined() || value.isNull()) return std::nullopt;
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        throw std::runtime_error("Nieprawidłowe dane liczbowe ADS-B");
    return value.toDouble();
}
}

QVector<Aircraft> parseAircraftJson(const QByteArray &json) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value("aircraft").isArray())
        throw std::runtime_error("Nieprawidłowe dane z dekodera ADS-B");
    QVector<Aircraft> result;
    static const QRegularExpression hex("^~?[0-9A-Fa-f]{6}$");
    for (const auto &entry : document.object().value("aircraft").toArray()) {
        if (!entry.isObject()) throw std::runtime_error("Nieprawidłowy wpis samolotu");
        const auto object = entry.toObject();
        const QString icao = object.value("hex").toString();
        const auto seen = number(object, "seen");
        if (!hex.match(icao).hasMatch() || !seen || *seen < 0)
            throw std::runtime_error("Nieprawidłowy identyfikator lub czas samolotu");
        Aircraft aircraft;
        aircraft.icao = icao.toUpper();
        const auto flight = object.value("flight");
        if (!flight.isUndefined() && !flight.isNull() && !flight.isString())
            throw std::runtime_error("Nieprawidłowy znak lotu");
        aircraft.flight = flight.toString().trimmed();
        const auto altitude = object.value("alt_baro");
        if (altitude.isString()) {
            if (altitude.toString() != "ground") throw std::runtime_error("Nieprawidłowa wysokość");
            aircraft.onGround = true;
        } else if (auto feet = number(object, "alt_baro")) {
            aircraft.altitudeMetres = *feet * 0.3048;
        }
        if (auto speed = number(object, "gs")) {
            if (*speed < 0) throw std::runtime_error("Nieprawidłowa prędkość");
            aircraft.speedKmh = *speed * 1.852;
        }
        if (auto track = number(object, "track")) {
            if (*track < 0 || *track > 360) throw std::runtime_error("Nieprawidłowy kierunek");
            aircraft.trackDegrees = *track;
        }
        const auto lat = number(object, "lat"), lon = number(object, "lon");
        if (bool(lat) != bool(lon)) throw std::runtime_error("Niepełna pozycja samolotu");
        if (lat && lon) {
            const auto age = number(object, "seen_pos");
            if (!age || *age < 0 || *lat < -90 || *lat > 90 || *lon < -180 || *lon > 180)
                throw std::runtime_error("Nieprawidłowa pozycja samolotu");
            if (*age <= 60) aircraft.position = Location{*lat, *lon};
        }
        aircraft.ageSeconds = *seen;
        result.append(aircraft);
    }
    return result;
}

AdsbDecoder::AdsbDecoder(QObject *parent) : QObject(parent), process_(this), timer_(this) {
    if (!directory_.isValid()) throw std::runtime_error("Nie można utworzyć katalogu ADS-B");
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &AdsbDecoder::poll);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        errors_.append(process_.readAllStandardError());
        if (errors_.size() > 4000) errors_ = errors_.right(4000);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!stopping_ && error == QProcess::FailedToStart)
            reportFailure("Nie można uruchomić readsb: " + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) {
        errors_.append(process_.readAllStandardError());
        if (!stopping_) reportFailure(QString("Dekoder ADS-B zakończył pracę (%1): %2")
                                      .arg(code).arg(QString::fromUtf8(errors_.right(2000)).trimmed()));
    });
}

AdsbDecoder::~AdsbDecoder() {
    if (!stop()) qWarning("Nie można zatrzymać dekodera ADS-B");
}

void AdsbDecoder::start(const QString &executable, std::optional<Location> reference) {
    if (!QFileInfo(executable).isExecutable()) {
        reportFailure("Brak dekodera readsb z obsługą SoapySDR: " + executable);
        return;
    }
    QStringList args{"--device-type=soapysdr", "--soapy-device=driver=sdrplay", "--soapy-enable-agc",
                     "--write-json=" + directory_.path(), "--write-json-every=1", "--quiet",
                     "--json-location-accuracy=0"};
    if (reference) {
        args << "--lat=" + QString::number(reference->latitude, 'f', 6)
             << "--lon=" + QString::number(reference->longitude, 'f', 6);
    }
    process_.start(executable, args);
    lastUpdate_.start();
    timer_.start();
}

bool AdsbDecoder::stop() {
    stopping_ = true;
    timer_.stop();
    if (process_.state() != QProcess::NotRunning) {
        process_.terminate();
        if (!process_.waitForFinished(3000)) {
            process_.kill();
            if (!process_.waitForFinished(3000)) return false;
        }
    }
    return true;
}

void AdsbDecoder::poll() {
    const QString path = directory_.filePath("aircraft.json");
    QFile file(path);
    if (!file.exists()) {
        if (lastUpdate_.elapsed() > 10000) reportFailure("Dekoder ADS-B nie utworzył danych samolotów");
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        reportFailure("Nie można odczytać danych ADS-B: " + file.errorString());
        return;
    }
    try {
        aircraft_ = parseAircraftJson(file.readAll());
    } catch (const std::exception &error) {
        reportFailure(QString::fromUtf8(error.what()));
        return;
    }
    const auto updated = QFileInfo(file).lastModified().toUTC();
    if (!updated.isValid() || updated.msecsTo(QDateTime::currentDateTimeUtc()) > 10000) {
        reportFailure("Dane ADS-B nie są aktualizowane");
        return;
    }
    lastUpdate_.restart();
    emit this->updated();
}

void AdsbDecoder::reportFailure(const QString &message) {
    if (reported_ || stopping_) return;
    reported_ = true;
    timer_.stop();
    emit failed(message);
}
