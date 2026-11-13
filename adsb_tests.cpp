#include "adsb.hpp"
#include <QFile>
#include <cmath>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
    if (argc == 2) {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::ReadOnly)) return 1;
        const auto result = parseAircraftJson(file.readAll());
        for (const auto &aircraft : result) {
            if (aircraft.icao == "40621D" && aircraft.position &&
                std::abs(aircraft.position->latitude - 52.265780) < 0.001 &&
                std::abs(aircraft.position->longitude - 3.938913) < 0.001) {
                std::cout << "Synthetic IQ decoded to expected aircraft position\n";
                return 0;
            }
        }
        std::cerr << "Expected decoded aircraft position missing\n";
        return 1;
    }
    const QByteArray json = R"({"aircraft":[
      {"hex":"40621d","flight":" TEST123 ","alt_baro":38000,"gs":200,"track":90,
       "lat":52.265780,"lon":3.938913,"seen_pos":1.5,"seen":2},
      {"hex":"~abcdef","alt_baro":"ground","lat":52,"lon":20,"seen_pos":61,"seen":3},
      {"hex":"123abc","seen":0.2}
    ]})";
    const auto aircraft = parseAircraftJson(json);
    if (aircraft.size() != 3 || aircraft[0].icao != "40621D" || aircraft[0].flight != "TEST123" ||
        !aircraft[0].altitudeMetres || std::abs(*aircraft[0].altitudeMetres - 11582.4) > 0.01 ||
        !aircraft[0].speedKmh || std::abs(*aircraft[0].speedKmh - 370.4) > 0.01 ||
        !aircraft[0].position || distanceKm(*aircraft[0].position, Location{52.26, 3.92}) < 1 ||
        !aircraft[1].onGround || aircraft[1].position || aircraft[2].position) return 1;
    for (const QByteArray &bad : {QByteArray("{"), QByteArray(R"({"aircraft":[{"hex":"bad","seen":1}]})"),
                                  QByteArray(R"({"aircraft":[{"hex":"123abc","seen":1,"lat":91,"lon":2,"seen_pos":1}]})")}) {
        try { parseAircraftJson(bad); return 1; } catch (const std::runtime_error &) {}
    }
    std::cout << "Aircraft JSON, units, positions and invalid data tests passed\n";
}
