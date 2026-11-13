#include "station_lists.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <stdexcept>

namespace {
void validate(const QVector<StationList> &lists) {
    QSet<QString> names;
    for (const auto &list : lists) {
        const QString key = list.name.trimmed().toCaseFolded();
        if (key.isEmpty() || key != list.name.toCaseFolded() || names.contains(key))
            throw std::invalid_argument("Nazwa listy jest pusta, powtórzona lub zawiera spacje na brzegach");
        names.insert(key);
        QSet<QString> stations;
        for (const auto &station : list.stations) {
            if (station.frequencyHz < 10000 || station.frequencyHz > 2000000000 ||
                (station.mode != "LSB" && station.mode != "USB" && station.mode != "AM" &&
                 station.mode != "NFM" && station.mode != "WFM"))
                throw std::invalid_argument("Nieprawidłowa częstotliwość lub modulacja na liście");
            const QString key = QString::number(station.frequencyHz) + "/" + station.mode;
            if (stations.contains(key)) throw std::invalid_argument("Powtórzona częstotliwość i modulacja");
            stations.insert(key);
        }
    }
}
}

int addDigitalLists(QVector<StationList> &lists) {
    const QVector<StationList> defaults{
        {"Cyfrowe FT8", {
            {"160 m", 1840000, "USB"}, {"80 m", 3573000, "USB"}, {"60 m", 5357000, "USB"},
            {"40 m", 7074000, "USB"}, {"30 m", 10136000, "USB"}, {"20 m", 14074000, "USB"},
            {"17 m", 18100000, "USB"}, {"15 m", 21074000, "USB"}, {"12 m", 24915000, "USB"},
            {"10 m", 28074000, "USB"}, {"6 m", 50313000, "USB"}, {"2 m", 144174000, "USB"}}},
        {"Cyfrowe FT4", {
            {"80 m", 3575000, "USB"}, {"40 m", 7047500, "USB"}, {"30 m", 10140000, "USB"},
            {"20 m", 14080000, "USB"}, {"17 m", 18104000, "USB"}, {"15 m", 21140000, "USB"},
            {"12 m", 24919000, "USB"}, {"10 m", 28180000, "USB"}, {"6 m", 50318000, "USB"},
            {"2 m", 144170000, "USB"}}},
        {"Cyfrowe WSPR", {
            {"160 m", 1836600, "USB"}, {"80 m", 3568600, "USB"}, {"40 m", 7038600, "USB"},
            {"30 m", 10138700, "USB"}, {"20 m", 14095600, "USB"}, {"17 m", 18104600, "USB"},
            {"15 m", 21094600, "USB"}, {"12 m", 24924600, "USB"}, {"10 m", 28124600, "USB"}}}
    };
    int added = 0;
    for (const auto &candidate : defaults) {
        bool exists = false;
        for (const auto &list : lists)
            if (list.name.toCaseFolded() == candidate.name.toCaseFolded()) { exists = true; break; }
        if (!exists) { lists.append(candidate); ++added; }
    }
    return added;
}

StationLists::StationLists(QString path) : path_(std::move(path)) {
    QFile file(path_);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Nie można odczytać list: " + file.errorString()).toStdString());
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Nieprawidłowy plik list częstotliwości");
    const auto root = doc.object();
    if (root.value("version").toInt() != 1 || !root.value("lists").isArray())
        throw std::runtime_error("Nieobsługiwany format list częstotliwości");
    for (const auto &value : root.value("lists").toArray()) {
        if (!value.isObject()) throw std::runtime_error("Nieprawidłowy wpis listy");
        const auto object = value.toObject();
        if (!object.value("name").isString() || !object.value("stations").isArray())
            throw std::runtime_error("Nieprawidłowy wpis listy");
        StationList list{object.value("name").toString(), {}};
        for (const auto &item : object.value("stations").toArray()) {
            if (!item.isObject()) throw std::runtime_error("Nieprawidłowy wpis częstotliwości");
            const auto station = item.toObject();
            if (!station.value("name").isString() || !station.value("mode").isString() ||
                !station.value("frequency_hz").isDouble())
                throw std::runtime_error("Nieprawidłowy wpis częstotliwości");
            double frequency = station.value("frequency_hz").toDouble();
            if (frequency < 10000 || frequency > 2000000000 || frequency != qint64(frequency))
                throw std::runtime_error("Nieprawidłowa częstotliwość");
            list.stations.append({station.value("name").toString(), qint64(frequency),
                                  station.value("mode").toString()});
        }
        lists_.append(list);
    }
    validate(lists_);
}

void StationLists::save(const QVector<StationList> &lists) {
    validate(lists);
    QJsonArray rows;
    for (const auto &list : lists) {
        QJsonArray stations;
        for (const auto &station : list.stations) {
            stations.append(QJsonObject{{"name", station.name}, {"frequency_hz", double(station.frequencyHz)},
                                        {"mode", station.mode}});
        }
        rows.append(QJsonObject{{"name", list.name}, {"stations", stations}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1}, {"lists", rows}}).toJson();
    if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
        throw std::runtime_error("Nie można utworzyć katalogu list");
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error(("Nie można zapisać list: " + file.errorString()).toStdString());
    lists_ = lists;
}
