#!/usr/bin/env python3
import argparse
import csv
import datetime
from html.parser import HTMLParser
import io
import json
import math
import re
import sys
import urllib.request

from sgp4.api import Satrec, jday
from sgp4.omm import initialize


CELESTRAK_URL = (
    "https://celestrak.org/NORAD/elements/gp.php"
    "?GROUP=weather&FORMAT=csv"
)
FREQUENCY_STATUS_URL = "https://orbitnight.com/weather-satellites"
EARTH_RADIUS_KM = 6378.137
EARTH_ECCENTRICITY_SQUARED = 0.00669437999014
PASS_STEP_SECONDS = 30


def elevation_degrees(satellite, latitude, longitude, when):
    jd, fraction = jday(
        when.year,
        when.month,
        when.day,
        when.hour,
        when.minute,
        when.second + when.microsecond / 1_000_000,
    )
    error, position, _ = satellite.sgp4(jd, fraction)
    if error:
        return None

    centuries = (jd + fraction - 2451545.0) / 36525.0
    sidereal_degrees = (
        280.46061837
        + 360.98564736629 * (jd + fraction - 2451545.0)
        + 0.000387933 * centuries * centuries
        - centuries * centuries * centuries / 38710000.0
    ) % 360.0
    sidereal = math.radians(sidereal_degrees)
    cosine, sine = math.cos(sidereal), math.sin(sidereal)
    x, y, z = position
    x = cosine * x + sine * y
    y = -sine * x + cosine * y

    latitude = math.radians(latitude)
    longitude = math.radians(longitude)
    sin_latitude, cos_latitude = math.sin(latitude), math.cos(latitude)
    normal = EARTH_RADIUS_KM / math.sqrt(
        1.0 - EARTH_ECCENTRICITY_SQUARED * sin_latitude * sin_latitude
    )
    observer_x = normal * cos_latitude * math.cos(longitude)
    observer_y = normal * cos_latitude * math.sin(longitude)
    observer_z = normal * (1.0 - EARTH_ECCENTRICITY_SQUARED) * sin_latitude
    dx, dy, dz = x - observer_x, y - observer_y, z - observer_z
    up = (
        cos_latitude * math.cos(longitude) * dx
        + cos_latitude * math.sin(longitude) * dy
        + sin_latitude * dz
    )
    east = -math.sin(longitude) * dx + math.cos(longitude) * dy
    north = (
        -sin_latitude * math.cos(longitude) * dx
        - sin_latitude * math.sin(longitude) * dy
        + cos_latitude * dz
    )
    return math.degrees(math.atan2(up, math.hypot(east, north)))


def refine_crossing(satellite, latitude, longitude, low, high, threshold, rising):
    for _ in range(18):
        middle = low + (high - low) / 2
        elevation = elevation_degrees(satellite, latitude, longitude, middle)
        if elevation is not None and (elevation >= threshold) == rising:
            high = middle
        else:
            low = middle
    return high


def find_passes(satellite, name, latitude, longitude, start, hours, threshold):
    end = start + datetime.timedelta(hours=hours)
    previous_time = start
    previous_elevation = elevation_degrees(
        satellite, latitude, longitude, previous_time
    )
    in_pass = previous_elevation is not None and previous_elevation >= threshold
    pass_start = start if in_pass else None
    maximum_elevation = previous_elevation if in_pass else -90.0
    maximum_time = start if in_pass else None
    passes = []

    for step in range(1, hours * 3600 // PASS_STEP_SECONDS + 1):
        current_time = min(
            start + datetime.timedelta(seconds=step * PASS_STEP_SECONDS), end
        )
        current_elevation = elevation_degrees(
            satellite, latitude, longitude, current_time
        )
        visible = current_elevation is not None and current_elevation >= threshold

        if visible and not in_pass:
            pass_start = refine_crossing(
                satellite, latitude, longitude, previous_time, current_time,
                threshold, True
            )
            maximum_elevation = current_elevation
            maximum_time = current_time
            in_pass = True
        elif visible and current_elevation > maximum_elevation:
            maximum_elevation = current_elevation
            maximum_time = current_time
        elif in_pass and not visible:
            pass_end = refine_crossing(
                satellite, latitude, longitude, previous_time, current_time,
                threshold, False
            )
            passes.append({
                "satellite": name,
                "start_utc": pass_start.isoformat(timespec="seconds").replace("+00:00", "Z"),
                "maximum_utc": maximum_time.isoformat(timespec="seconds").replace("+00:00", "Z"),
                "end_utc": pass_end.isoformat(timespec="seconds").replace("+00:00", "Z"),
                "maximum_elevation": round(maximum_elevation, 1),
            })
            in_pass = False

        previous_time = current_time
        previous_elevation = current_elevation

    if in_pass:
        passes.append({
            "satellite": name,
            "start_utc": pass_start.isoformat(timespec="seconds").replace("+00:00", "Z"),
            "maximum_utc": maximum_time.isoformat(timespec="seconds").replace("+00:00", "Z"),
            "end_utc": end.isoformat(timespec="seconds").replace("+00:00", "Z"),
            "maximum_elevation": round(maximum_elevation, 1),
        })
    return passes


class VisibleTextParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.hidden_depth = 0
        self.parts = []

    def handle_starttag(self, tag, attrs):
        if tag in {"script", "style"}:
            self.hidden_depth += 1

    def handle_endtag(self, tag):
        if tag in {"script", "style"} and self.hidden_depth:
            self.hidden_depth -= 1

    def handle_data(self, data):
        if not self.hidden_depth:
            self.parts.append(data)


def parse_frequency_age(text):
    match = re.search(
        r"(\d+)\s*(minutes?|mins?|m|hours?|hrs?|h|days?|d)\s+ago",
        text,
        re.IGNORECASE,
    )
    if not match:
        if "just now" in text.lower():
            return 0
        return None

    amount = int(match.group(1))
    unit = match.group(2).lower()
    if unit.startswith("m"):
        return amount * 60
    if unit.startswith("h"):
        return amount * 3600
    return amount * 86400


def parse_frequency_status(html):
    parser = VisibleTextParser()
    parser.feed(html)
    text = re.sub(r"\s+", " ", " ".join(parser.parts))
    observations = []
    for satellite_number in (3, 4):
        pattern = re.compile(
            rf"METEOR M2-{satellite_number}\s+Now on\s+"
            r"(\d+(?:\.\d+)?)\s+MHz\s+Last decoded\s+"
            r"(.+?)\s+by\s+(.+?)\s+\(SatNOGS\)",
            re.IGNORECASE,
        )
        match = pattern.search(text)
        if not match:
            continue
        frequency_hz = round(float(match.group(1)) * 1_000_000)
        if not 137_000_000 <= frequency_hz <= 138_000_000:
            continue
        observations.append({
            "satellite": f"METEOR-M2 {satellite_number}",
            "frequency_hz": frequency_hz,
            "age_seconds": parse_frequency_age(match.group(2)),
            "station": match.group(3).strip(),
        })
    return observations


def frequency_status():
    request = urllib.request.Request(
        FREQUENCY_STATUS_URL,
        headers={"User-Agent": "SDRTouch/1.0"},
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            html = response.read().decode("utf-8")
        observations = parse_frequency_status(html)
    except (OSError, UnicodeError, ValueError) as error:
        return [], f"Nie można pobrać danych o odbiorach SatNOGS: {error}"
    if not observations:
        return [], "Źródło nie zwróciło aktualnych danych o odbiorach Meteor-M."
    return observations, ""


def predict(latitude, longitude, hours=24, threshold=10):
    request = urllib.request.Request(
        CELESTRAK_URL,
        headers={"User-Agent": "SDRTouch/1.0"},
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        text = response.read().decode("utf-8")

    satellites = []
    for fields in csv.DictReader(io.StringIO(text)):
        name = fields.get("OBJECT_NAME", "").strip()
        if not name.upper().startswith("METEOR-M2"):
            continue
        satellite = Satrec()
        initialize(satellite, fields)
        satellites.append((name, satellite))
    if not satellites:
        raise RuntimeError("CelesTrak nie zwrócił aktualnych elementów satelitów Meteor-M.")

    start = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    passes = []
    for name, satellite in satellites:
        passes.extend(
            find_passes(satellite, name, latitude, longitude, start, hours, threshold)
        )
    passes.sort(key=lambda item: item["start_utc"])
    observations, frequency_error = frequency_status()
    return {
        "updated_utc": start.isoformat(timespec="seconds").replace("+00:00", "Z"),
        "passes": passes,
        "frequency_observations": observations,
        "frequency_error": frequency_error,
    }


def self_test():
    fields = {
        "OBJECT_NAME": "TEST ORBIT",
        "OBJECT_ID": "1998-067A",
        "EPOCH": "2026-01-01T00:00:00.000000",
        "MEAN_MOTION": "15.5",
        "ECCENTRICITY": "0.0005",
        "INCLINATION": "51.6",
        "RA_OF_ASC_NODE": "0.0",
        "ARG_OF_PERICENTER": "0.0",
        "MEAN_ANOMALY": "0.0",
        "EPHEMERIS_TYPE": "0",
        "CLASSIFICATION_TYPE": "U",
        "NORAD_CAT_ID": "25544",
        "ELEMENT_SET_NO": "999",
        "REV_AT_EPOCH": "1",
        "BSTAR": "0.0001",
        "MEAN_MOTION_DOT": "0.0001",
        "MEAN_MOTION_DDOT": "0",
    }
    satellite = Satrec()
    initialize(satellite, fields)
    when = datetime.datetime(2026, 1, 1, tzinfo=datetime.timezone.utc)
    elevation = elevation_degrees(satellite, 52.0, 21.0, when)
    if elevation is None or not -90.0 <= elevation <= 90.0:
        raise RuntimeError("SGP4 self-test failed to calculate a valid elevation.")
    crossing = refine_crossing(
        satellite, 52.0, 21.0, when, when + datetime.timedelta(seconds=1),
        -90.0, True
    )
    if abs((crossing - when).total_seconds()) > 1:
        raise RuntimeError("Pass-boundary self-test failed.")
    observations = parse_frequency_status(
        "<div>METEOR M2-4 Now on 137.900 MHz Last decoded 3 h ago "
        "by Station Four (SatNOGS)</div>"
        "<div>METEOR M2-3 Now on 137.9125 MHz Last decoded 15 min ago "
        "by Station Three (SatNOGS)</div>"
    )
    observations_by_satellite = {
        observation["satellite"]: observation for observation in observations
    }
    if len(observations_by_satellite) != 2 or \
            observations_by_satellite.get("METEOR-M2 4", {}).get("age_seconds") != 10800 or \
            observations_by_satellite.get("METEOR-M2 3", {}).get("frequency_hz") != 137912500:
        raise RuntimeError("Meteor frequency status self-test failed.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--predict", nargs=2, type=float, metavar=("LAT", "LON"))
    parser.add_argument("--hours", type=int, default=24)
    parser.add_argument("--threshold", type=float, default=10)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        print("Meteor pass predictor self-test passed")
        return 0
    if args.predict is None:
        parser.error("use --predict LAT LON or --self-test")
    latitude, longitude = args.predict
    if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
        parser.error("latitude or longitude is out of range")
    if not 1 <= args.hours <= 48:
        parser.error("--hours must be between 1 and 48")
    if not 0 <= args.threshold < 90:
        parser.error("--threshold must be between 0 and 90 degrees")

    try:
        print(json.dumps(
            predict(latitude, longitude, args.hours, args.threshold),
            separators=(",", ":"),
        ))
    except (OSError, UnicodeError, csv.Error, TypeError, ValueError, RuntimeError) as error:
        print(f"Meteor pass prediction failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
