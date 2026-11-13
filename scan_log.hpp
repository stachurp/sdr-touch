#pragma once

#include <QFile>
#include <QString>

class ScanLog {
public:
    explicit ScanLog(const QString &path);
    bool record(qint64 frequency, const QString &mode, double level, double threshold);
private:
    QFile file_;
};
