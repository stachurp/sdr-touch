#include "scan_log.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <cmath>
#include <stdexcept>

ScanLog::ScanLog(const QString &path) : file_(path) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw std::runtime_error("Nie mozna utworzyc katalogu wynikow");
    if (!file_.open(QIODevice::ReadWrite | QIODevice::Append | QIODevice::Text))
        throw std::runtime_error(("Nie mozna zapisac wynikow: " + file_.errorString()).toStdString());
    if (file_.size() == 0) {
        const QByteArray header = "utc,frequency_hz,mode,peak_dbfs\n";
        if (file_.write(header) != header.size() || !file_.flush())
            throw std::runtime_error(("Blad zapisu wynikow: " + file_.errorString()).toStdString());
    } else if (!file_.seek(0) || file_.readLine() != "utc,frequency_hz,mode,peak_dbfs\n" ||
               !file_.seek(file_.size() - 1) || file_.read(1) != "\n") {
        throw std::runtime_error("Nieprawidlowy lub niepelny plik wynikow CSV; nie dopisano danych");
    }
}

bool ScanLog::record(qint64 frequency, const QString &mode, double level, double threshold) {
    if (frequency < 10000 || frequency > 2000000000 || !std::isfinite(level) ||
        !std::isfinite(threshold) || (mode != "LSB" && mode != "USB" && mode != "AM" &&
                                     mode != "NFM" && mode != "WFM"))
        throw std::invalid_argument("Nieprawidlowy wynik skanowania");
    if (level < threshold) return false;
    QByteArray line = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8() + "," +
        QByteArray::number(frequency) + "," + mode.toUtf8() + "," +
        QByteArray::number(level, 'f', 1) + "\n";
    if (file_.write(line) != line.size() || !file_.flush())
        throw std::runtime_error(("Blad zapisu wynikow: " + file_.errorString()).toStdString());
    return true;
}
