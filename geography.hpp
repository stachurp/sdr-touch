#pragma once

#include <QString>
#include <memory>
#include <optional>

struct Location {
    double latitude;
    double longitude;
};

struct Country {
    QString code;
    QString polishName;
    QString flag;
};

std::optional<Location> maidenheadLocation(const QString &locator);
double distanceKm(Location first, Location second);

class Countries {
public:
    Countries(const QString &ctyPath, const QString &isoPath);
    ~Countries();
    Countries(const Countries &) = delete;
    Countries &operator=(const Countries &) = delete;
    std::optional<Country> lookup(const QString &callsign) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
