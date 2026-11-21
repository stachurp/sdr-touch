#include "station_lists.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

int main() {
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString path = directory.filePath("lists.json");
    StationLists store(path);
    QVector<StationList> lists{{"Radio", {{"Stacja", 100100000, "WFM"}, {"", 7100000, "LSB"}}},
                               {"Krótkie", {{"", 14074000, "USB"}, {"CW", 7025000, "CW"}}}};
    store.save(lists);
    StationLists loaded(path);
    if (loaded.lists().size() != 2 || loaded.lists()[0].stations.size() != 2 ||
        loaded.lists()[0].stations[0].frequencyHz != 100100000 ||
        loaded.lists()[1].stations[0].mode != "USB" ||
        loaded.lists()[1].stations[1].mode != "CW") return 1;
    lists[0].stations.append(lists[0].stations[0]);
    try { loaded.save(lists); return 1; } catch (const std::invalid_argument &) {}
    StationLists afterFailedSave(path);
    if (afterFailedSave.lists()[0].stations.size() != 2) return 1;
    QVector<StationList> presets{{"cyfrowe ft8", {{"Własna", 14074000, "USB"}}}};
    if (addDefaultLists(presets) != 3 || presets.size() != 4 ||
        presets[0].stations.size() != 1 || addDefaultLists(presets) != 0) return 1;
    const auto pmr = presets[3].stations;
    if (presets[3].name != "PMR446" || pmr.size() != 16 ||
        pmr.front().frequencyHz != 446006250 || pmr.back().frequencyHz != 446193750) return 1;
    for (int i = 0; i < pmr.size(); ++i)
        if (pmr[i].name != QString("Kanał %1").arg(i + 1) || pmr[i].mode != "NFM" ||
            pmr[i].frequencyHz != 446006250 + i * 12500) return 1;
    store.save(presets);
    StationLists presetReload(path);
    if (presetReload.lists().size() != 4 || presetReload.lists()[0].stations[0].name != "Własna" ||
        presetReload.lists()[3].stations.size() != 16 ||
        presetReload.lists()[3].stations[0].frequencyHz != 446006250) return 1;
    const QString badPath = directory.filePath("bad.json");
    QFile bad(badPath);
    if (!bad.open(QIODevice::WriteOnly) || bad.write("{bad") != 4) return 1;
    bad.close();
    try { StationLists invalid(badPath); return 1; } catch (const std::runtime_error &) {}
    if (!bad.open(QIODevice::ReadOnly) || bad.readAll() != "{bad") return 1;
    std::cout << "Station lists persistence and invalid data tests passed\n";
}
