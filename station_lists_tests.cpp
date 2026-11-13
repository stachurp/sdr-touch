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
                               {"Krótkie", {{"", 14074000, "USB"}}}};
    store.save(lists);
    StationLists loaded(path);
    if (loaded.lists().size() != 2 || loaded.lists()[0].stations.size() != 2 ||
        loaded.lists()[0].stations[0].frequencyHz != 100100000 ||
        loaded.lists()[1].stations[0].mode != "USB") return 1;
    lists[0].stations.append(lists[0].stations[0]);
    try { loaded.save(lists); return 1; } catch (const std::invalid_argument &) {}
    StationLists afterFailedSave(path);
    if (afterFailedSave.lists()[0].stations.size() != 2) return 1;
    QVector<StationList> presets{{"cyfrowe ft8", {{"Własna", 14074000, "USB"}}}};
    if (addDigitalLists(presets) != 2 || presets.size() != 3 ||
        presets[0].stations.size() != 1 || addDigitalLists(presets) != 0) return 1;
    store.save(presets);
    StationLists presetReload(path);
    if (presetReload.lists().size() != 3 || presetReload.lists()[0].stations[0].name != "Własna") return 1;
    const QString badPath = directory.filePath("bad.json");
    QFile bad(badPath);
    if (!bad.open(QIODevice::WriteOnly) || bad.write("{bad") != 4) return 1;
    bad.close();
    try { StationLists invalid(badPath); return 1; } catch (const std::runtime_error &) {}
    if (!bad.open(QIODevice::ReadOnly) || bad.readAll() != "{bad") return 1;
    std::cout << "Station lists persistence and invalid data tests passed\n";
}
