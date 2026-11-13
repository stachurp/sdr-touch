#pragma once

#include "geography.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <optional>

struct Aircraft {
    QString icao;
    QString flight;
    std::optional<double> altitudeMetres;
    bool onGround = false;
    std::optional<double> speedKmh;
    std::optional<double> trackDegrees;
    std::optional<Location> position;
    double ageSeconds;
};

QVector<Aircraft> parseAircraftJson(const QByteArray &json);

class AdsbDecoder : public QObject {
    Q_OBJECT
public:
    explicit AdsbDecoder(QObject *parent = nullptr);
    ~AdsbDecoder() override;
    void start(const QString &executable, std::optional<Location> reference);
    bool stop();
    const QVector<Aircraft> &aircraft() const { return aircraft_; }

signals:
    void updated();
    void failed(const QString &message);

private:
    void poll();
    void reportFailure(const QString &message);
    QTemporaryDir directory_;
    QProcess process_;
    QTimer timer_;
    QElapsedTimer lastUpdate_;
    QVector<Aircraft> aircraft_;
    QByteArray errors_;
    bool stopping_ = false;
    bool reported_ = false;
};
