#pragma once

#include "geography.hpp"
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>
#include <optional>

struct SondeCandidate {
    QString serial;
    QString type;
    qint64 frequencyHz;
    Location position;
    double altitude;
    double distance;
    QDateTime received;
};

QVector<SondeCandidate> parseSondeCandidates(const QByteArray &json, Location origin,
                                              double radiusKm, const QDateTime &now);

enum class SondeType { RS41, M10, M20 };

struct SondeTelemetry {
    QString serial;
    QJsonObject data;
    std::optional<Location> position;
};

SondeTelemetry parseSondeTelemetry(const QString &json);

class SondeDecoder : public QObject {
    Q_OBJECT
public:
    explicit SondeDecoder(QObject *parent = nullptr, QString executable = {});
    ~SondeDecoder() override;
    void start(qint64 frequency, SondeType type = SondeType::RS41, double shiftHz = 0);
    void stop();
    void feed(const QByteArray &stereo);
    void feedIq(const QByteArray &iq);
    void gap();
    bool isActive() const { return active_; }
    qint64 frequency() const { return frequency_; }
    SondeType type() const { return type_; }

signals:
    void telemetry(const QString &text);
    void failed(const QString &message);

private:
    void fail(const QString &message);
    void readOutput();
    QProcess process_;
    QString executable_;
    SondeType type_ = SondeType::RS41;
    QByteArray pending_;
    QByteArray errors_;
    qint64 frequency_ = 0;
    bool active_ = false;
    bool stopping_ = false;
};
