#include "geography.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <unicode/uloc.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

std::optional<Location> maidenheadLocation(const QString &locator) {
    const QString s = locator.trimmed().toUpper();
    static const QRegularExpression valid("^[A-R]{2}[0-9]{2}([A-X]{2}([0-9]{2})?)?$");
    if (!valid.match(s).hasMatch()) return std::nullopt;
    double lon = -180 + (s[0].unicode() - 'A') * 20 + (s[2].unicode() - '0') * 2;
    double lat = -90 + (s[1].unicode() - 'A') * 10 + (s[3].unicode() - '0');
    double lonWidth = 2, latWidth = 1;
    if (s.size() >= 6) {
        lonWidth /= 24;
        latWidth /= 24;
        lon += (s[4].unicode() - 'A') * lonWidth;
        lat += (s[5].unicode() - 'A') * latWidth;
    }
    if (s.size() == 8) {
        lonWidth /= 10;
        latWidth /= 10;
        lon += (s[6].unicode() - '0') * lonWidth;
        lat += (s[7].unicode() - '0') * latWidth;
    }
    return Location{lat + latWidth / 2, lon + lonWidth / 2};
}

double distanceKm(Location a, Location b) {
    constexpr double rad = 3.14159265358979323846 / 180;
    const double dlat = (b.latitude - a.latitude) * rad;
    const double dlon = (b.longitude - a.longitude) * rad;
    const double h = std::pow(std::sin(dlat / 2), 2) +
        std::cos(a.latitude * rad) * std::cos(b.latitude * rad) * std::pow(std::sin(dlon / 2), 2);
    return 6371.0088 * 2 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
}

namespace {
QMap<QString, QString> aliases() {
    return {
        {"Agalega & St. Brandon","MU"}, {"Rodriguez Island","MU"}, {"Annobon Island","GQ"},
        {"Conway Reef","FJ"}, {"Rotuma Island","FJ"}, {"Kingdom of Eswatini","SZ"},
        {"Bouvet","BV"}, {"Peter 1 Island","AQ"}, {"Timor - Leste","TL"},
        {"West Malaysia","MY"}, {"East Malaysia","MY"}, {"Dem. Rep. of the Congo","CD"},
        {"Trinidad & Tobago","TT"}, {"Pratas Island","TW"}, {"The Gambia","GM"},
        {"San Felix & San Ambrosio","CL"}, {"Easter Island","CL"}, {"Juan Fernandez Islands","CL"},
        {"Madeira Islands","PT"}, {"Azores","PT"}, {"Sable Island","CA"},
        {"St. Paul Island","CA"}, {"Cape Verde","CV"}, {"Fed. Rep. of Germany","DE"},
        {"Palestine","PS"}, {"North Cook Islands","CK"}, {"South Cook Islands","CK"},
        {"Bosnia-Herzegovina","BA"}, {"Balearic Islands","ES"}, {"Canary Islands","ES"},
        {"Ceuta & Melilla","ES"}, {"St. Barthelemy","BL"}, {"Chesterfield Islands","NC"},
        {"Austral Islands","PF"}, {"Clipperton Island","CP"}, {"Marquesas Islands","PF"},
        {"St. Pierre & Miquelon","PM"}, {"Reunion Island","RE"}, {"St. Martin","MF"},
        {"Glorioso Islands","TF"}, {"Juan de Nova, Europa","TF"}, {"Tromelin Island","TF"},
        {"Crozet Island","TF"}, {"Kerguelen Islands","TF"}, {"Amsterdam & St. Paul Is.","TF"},
        {"Wallis & Futuna Islands","WF"}, {"England","GB"}, {"Northern Ireland","GB"},
        {"Scotland","GB"}, {"Shetland Islands","GB"}, {"Wales","GB"},
        {"Temotu Province","SB"}, {"Galapagos Islands","EC"}, {"San Andres & Providencia","CO"},
        {"Malpelo Island","CO"}, {"Republic of Korea","KR"}, {"Vatican City","VA"},
        {"African Italy","IT"}, {"Sardinia","IT"}, {"Sicily","IT"}, {"St. Lucia","LC"},
        {"St. Vincent","VC"}, {"Minami Torishima","JP"}, {"Ogasawara","JP"},
        {"Svalbard","SJ"}, {"Bear Island","SJ"}, {"Jan Mayen","SJ"}, {"Guantanamo Bay","CU"},
        {"Mariana Islands","MP"}, {"Baker & Howland Islands","UM"}, {"Johnston Island","UM"},
        {"Midway Island","UM"}, {"Palmyra & Jarvis Islands","UM"}, {"Hawaii","US"},
        {"Kure Island","UM"}, {"Swains Island","AS"}, {"Wake Island","UM"}, {"Alaska","US"},
        {"Navassa Island","UM"}, {"US Virgin Islands","VI"}, {"Desecheo Island","PR"},
        {"Aland Islands","AX"}, {"Czech Republic","CZ"}, {"Slovak Republic","SK"},
        {"DPR of Korea","KP"}, {"Curacao","CW"}, {"Bonaire","BQ"}, {"Saba & St. Eustatius","BQ"},
        {"Sint Maarten","SX"}, {"Fernando de Noronha","BR"}, {"St. Peter & St. Paul","BR"},
        {"Trindade & Martim Vaz","BR"}, {"Franz Josef Land","RU"}, {"Sao Tome & Principe","ST"},
        {"Mount Athos","GR"}, {"Dodecanese","GR"}, {"Crete","GR"}, {"Western Kiribati","KI"},
        {"Central Kiribati","KI"}, {"Eastern Kiribati","KI"}, {"Banaba Island","KI"},
        {"Asiatic Turkey","TR"}, {"European Turkey","TR"}, {"Cocos Island","CR"},
        {"Corsica","FR"}, {"Republic of the Congo","CG"}, {"Cote d'Ivoire","CI"},
        {"European Russia","RU"}, {"Kaliningrad","RU"}, {"Asiatic Russia","RU"},
        {"Antigua & Barbuda","AG"}, {"St. Kitts & Nevis","KN"}, {"Micronesia","FM"},
        {"Heard Island","HM"}, {"Macquarie Island","AU"}, {"Lord Howe Island","AU"},
        {"Mellish Reef","AU"}, {"Willis Island","AU"}, {"British Virgin Islands","VG"},
        {"Turks & Caicos Islands","TC"}, {"Pitcairn Island","PN"}, {"Ducie Island","PN"},
        {"Falkland Islands","FK"}, {"South Georgia Island","GS"}, {"South Sandwich Islands","GS"},
        {"Chagos Islands","IO"}, {"Andaman & Nicobar Is.","IN"}, {"Lakshadweep Islands","IN"},
        {"Revillagigedo","MX"}, {"Aves Island","VE"}, {"Republic of Kosovo","XK"},
        {"Republic of South Sudan","SS"}, {"UK Base Areas on Cyprus","CY"}, {"St. Helena","SH"},
        {"Ascension Island","SH"}, {"Tristan da Cunha & Gough","SH"}, {"Tokelau Islands","TK"},
        {"Chatham Islands","NZ"}, {"Kermadec Islands","NZ"}, {"N.Z. Subantarctic Is.","NZ"},
        {"Pr. Edward & Marion Is.","ZA"}
    };
}

Country countryForCode(const QString &code) {
    if (code.size() != 2 || code[0] < 'A' || code[0] > 'Z' || code[1] < 'A' || code[1] > 'Z')
        throw std::runtime_error("Nieprawidlowy kod kraju");
    QByteArray locale = ("und_" + code).toLatin1();
    UChar text[256];
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = uloc_getDisplayCountry(locale.constData(), "pl", text, 256, &status);
    if (U_FAILURE(status) || length <= 0 || length >= 256)
        throw std::runtime_error("Nie mozna przetlumaczyc nazwy kraju");
    uint points[] = {uint(0x1f1e6 + code[0].unicode() - 'A'), uint(0x1f1e6 + code[1].unicode() - 'A')};
    return {code, QString::fromUtf16(reinterpret_cast<const ushort *>(text), length),
            QString::fromUcs4(points, 2)};
}
}

struct Countries::Impl {
    QMap<QString, Country> prefixes;
    QMap<QString, Country> exact;
};

Countries::Countries(const QString &ctyPath, const QString &isoPath) : impl_(std::make_unique<Impl>()) {
    QFile isoFile(isoPath);
    if (!isoFile.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Brak danych ISO krajow: " + isoFile.errorString()).toStdString());
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(isoFile.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Nieprawidlowe dane ISO krajow");
    QMap<QString, QString> countryCodes = aliases();
    for (const auto &value : doc.object().value("3166-1").toArray()) {
        const auto entry = value.toObject();
        const QString code = entry.value("alpha_2").toString();
        const QString name = entry.value("name").toString();
        if (!code.isEmpty() && !name.isEmpty() && !countryCodes.contains(name)) countryCodes.insert(name, code);
        const QString common = entry.value("common_name").toString();
        if (!code.isEmpty() && !common.isEmpty() && !countryCodes.contains(common)) countryCodes.insert(common, code);
    }
    if (countryCodes.size() < 200) throw std::runtime_error("Niepelne dane ISO krajow");
    QFile file(ctyPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        throw std::runtime_error(("Brak bazy prefiksow: " + file.errorString()).toStdString());
    QTextStream stream(&file);
    const QRegularExpression attribute("[\\(\\[\\{<~]");
    std::optional<Country> country;
    bool inEntry = false;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (line.isEmpty()) continue;
        if (!line[0].isSpace()) {
            const auto fields = line.split(':');
            if (fields.size() < 8) throw std::runtime_error("Nieprawidlowa baza prefiksow");
            QString name = fields[0].trimmed();
            if (name.startsWith('*')) name.remove(0, 1);
            const auto code = countryCodes.value(name);
            country = code.isEmpty() ? std::nullopt : std::optional<Country>(countryForCode(code));
            inEntry = true;
            line = fields[7].trimmed();
        } else if (!inEntry) {
            throw std::runtime_error("Nieprawidlowy wpis bazy prefiksow");
        }
        bool last = line.contains(';');
        line.replace(';', ',');
        for (auto prefix : line.split(',', Qt::SkipEmptyParts)) {
            prefix = prefix.trimmed().toUpper();
            int cut = prefix.indexOf(attribute);
            if (cut >= 0) prefix.truncate(cut);
            if (prefix.startsWith('*')) prefix.remove(0, 1);
            if (prefix.isEmpty()) continue;
            const Country value = country.value_or(Country{});
            if (prefix.startsWith('=')) impl_->exact.insert(prefix.mid(1), value);
            else impl_->prefixes.insert(prefix, value);
        }
        if (last) inEntry = false;
    }
    if (stream.status() != QTextStream::Ok || inEntry || impl_->prefixes.isEmpty())
        throw std::runtime_error("Niepelna baza prefiksow");
}

Countries::~Countries() = default;

std::optional<Country> Countries::lookup(const QString &callsign) const {
    QString call = callsign.trimmed().toUpper();
    if (call.startsWith('<') && call.endsWith('>')) call = call.mid(1, call.size() - 2);
    static const QRegularExpression valid("^[A-Z0-9]+(/[A-Z0-9]+)*$");
    if (!valid.match(call).hasMatch()) return std::nullopt;
    auto exact = impl_->exact.constFind(call);
    if (exact != impl_->exact.cend())
        return exact->code.isEmpty() ? std::nullopt : std::optional<Country>(*exact);
    QStringList candidates{call};
    const QStringList pieces = call.split('/');
    for (const auto &piece : pieces) {
        if (piece != "P" && piece != "M" && piece != "MM" && piece != "AM" && piece != "QRP")
            candidates.append(piece);
    }
    int best = 0;
    std::optional<Country> result;
    for (const auto &candidate : candidates) {
        for (int length = candidate.size(); length > best; --length) {
            auto it = impl_->prefixes.constFind(candidate.left(length));
            if (it != impl_->prefixes.cend()) {
                best = length;
                result = *it;
                break;
            }
        }
    }
    return result && !result->code.isEmpty() ? result : std::nullopt;
}
