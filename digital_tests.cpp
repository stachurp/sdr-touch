#include "digital.hpp"
#include "geography.hpp"
#include <cmath>
#include <iostream>

int main() {
    auto own = maidenheadLocation("ko02");
    auto other = maidenheadLocation("FN42");
    if (!own || !other || std::abs(own->latitude - 52.5) > 0.0001 ||
        std::abs(own->longitude - 21.0) > 0.0001 ||
        maidenheadLocation("ZZ99") || maidenheadLocation("KO0") ||
        !maidenheadLocation("KO02AA") || !maidenheadLocation("KO02AA12") ||
        distanceKm(*own, *other) < 6000 || distanceKm(*own, *other) > 7000 ||
        distanceKm(*own, *own) > 0.001) {
        std::cerr << "Maidenhead location or distance failed\n";
        return 1;
    }
    Countries countries("/usr/share/wsjtx/cty.dat", "/usr/share/iso-codes/json/iso_3166-1.json");
    for (const auto &pair : {std::pair<QString, QString>{"SP9ABC", "PL"}, {"DL1ABC", "DE"},
                              {"K1ABC", "US"}, {"JA1ABC", "JP"}, {"EA8/SP9ABC", "ES"}}) {
        auto result = countries.lookup(pair.first);
        if (!result || result->code != pair.second || result->polishName.isEmpty() || result->flag.isEmpty()) {
            std::cerr << "Country lookup failed for " << pair.first.toStdString() << '\n';
            return 1;
        }
    }
    auto poland = countries.lookup("SP9ABC");
    if (poland->polishName != QString::fromUtf8("Polska") || countries.lookup("1S0ABC")) {
        std::cerr << "Polish country name or unknown entity handling failed\n";
        return 1;
    }
    auto ft8 = parseDigitalLine(DigitalMode::FT8, "000001 -11 -0.0 1500 ~ CQ K1ABC FN42", 14074000);
    auto ft4 = parseDigitalLine(DigitalMode::FT4, "000000 -7 -0.1 560 + CQ RU AB5XS EM12", 14080000);
    auto wspr = parseDigitalLine(DigitalMode::WSPR, "0918 -9 1.1 14.097046 0 ND6P DM04 30", 14095600);
    auto noGrid = parseDigitalLine(DigitalMode::FT8, "123456 -5 0.2 1000 ~ SP9ABC K1ABC RR73", 14074000);
    if (!ft8 || ft8->station != "K1ABC" || ft8->grid != "FN42" || ft8->frequencyHz != 14075500 ||
        !ft4 || ft4->station != "AB5XS" || ft4->grid != "EM12" ||
        !wspr || wspr->station != "ND6P" || wspr->grid != "DM04" ||
        std::abs(wspr->frequencyHz - 14097046) > 0.1 ||
        !noGrid || noGrid->station != "K1ABC" || !noGrid->grid.isEmpty() ||
        parseDigitalLine(DigitalMode::FT8, "<DecodeFinished>", 14074000)) {
        std::cerr << "Decoder output parsing failed\n";
        return 1;
    }
    SlotAssembler assembler(DigitalMode::FT4);
    QByteArray stereo(1024, '\0');
    bool discontinuity = false;
    int completed = 0;
    qint64 origin = 9000000;
    for (int i = 0; i < 3000; ++i) {
        qint64 endMs = (origin + qint64(i + 1) * 64) / 12;
        auto completedSlots = assembler.feed(stereo, endMs, discontinuity);
        if (discontinuity) return 1;
        for (const auto &slot : completedSlots) {
            if (slot.startSample % 90000 != 0 || slot.pcm.size() != 180000) return 1;
            ++completed;
        }
    }
    if (completed < 1) {
        std::cerr << "UTC slot assembly failed\n";
        return 1;
    }
    const qint64 expectedEndMs = (origin + qint64(3001) * 64) / 12;
    assembler.feed(stereo, expectedEndMs + 150, discontinuity);
    if (!discontinuity) {
        std::cerr << "Discontinuity was not detected\n";
        return 1;
    }
    std::cout << "Country, locator, output parser and UTC slot tests passed\n";
}
