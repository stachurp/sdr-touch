#include "scan_log.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

int main() {
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QString path = directory.filePath("signals.csv");
    {
        ScanLog log(path);
        if (log.record(7100000, "LSB", -61, -60) ||
            !log.record(7101000, "LSB", -60, -60) ||
            !log.record(7102000, "LSB", -59, -60)) {
            std::cerr << "Threshold boundary failed\n";
            return 1;
        }
    }
    {
        ScanLog log(path);
        if (!log.record(7103000, "AM", -50, -60)) return 1;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return 1;
    auto lines = file.readAll().trimmed().split('\n');
    if (lines.size() != 4 || lines[0] != "utc,frequency_hz,mode,peak_dbfs" ||
        !lines[1].contains(",7101000,LSB,-60.0") ||
        !lines[2].contains(",7102000,LSB,-59.0") ||
        !lines[3].contains(",7103000,AM,-50.0")) {
        std::cerr << "Persistence or CSV shape failed\n";
        return 1;
    }
    file.close();
    QFile corrupt(directory.filePath("corrupt.csv"));
    if (!corrupt.open(QIODevice::WriteOnly) || corrupt.write("partial") != 7) return 1;
    corrupt.close();
    try {
        ScanLog bad(corrupt.fileName());
        std::cerr << "Corrupt file was accepted\n";
        return 1;
    } catch (const std::runtime_error &) {
        if (!corrupt.open(QIODevice::ReadOnly) || corrupt.readAll() != "partial") return 1;
    }
    std::cout << "Threshold, append, CSV and corrupt-file tests passed\n";
}
