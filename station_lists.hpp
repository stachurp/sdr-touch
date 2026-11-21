#pragma once

#include <QString>
#include <QVector>

struct Station {
    QString name;
    qint64 frequencyHz;
    QString mode;
};

struct StationList {
    QString name;
    QVector<Station> stations;
};

int addDefaultLists(QVector<StationList> &lists);

class StationLists {
public:
    explicit StationLists(QString path);
    const QVector<StationList> &lists() const { return lists_; }
    void save(const QVector<StationList> &lists);

private:
    QString path_;
    QVector<StationList> lists_;
};
