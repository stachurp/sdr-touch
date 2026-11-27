#include "sonde.hpp"
#include "dsp.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <complex>
#include <iostream>

static QByteArray syntheticMeteomodem(bool m20) {
    QByteArray frame(121, '\0');
    auto *bytes = reinterpret_cast<uchar *>(frame.data());
    if (m20) {
        bytes[0] = 0x45; bytes[1] = 0x20;
        const quint32 altitude = 1200000, time = 123456, serial = 80 + 128 + (2055 << 10);
        for (int i = 0; i < 3; ++i) {
            bytes[8 + i] = uchar(altitude >> (8 * (2 - i)));
            bytes[15 + i] = uchar(time >> (8 * (2 - i)));
            bytes[18 + i] = uchar(serial >> (8 * i));
        }
        qToBigEndian<quint16>(2300, bytes + 26);
        qToBigEndian<quint32>(52000000, bytes + 28);
        qToBigEndian<quint32>(21000000, bytes + 32);
        bytes[67] = 7;
    } else {
        bytes[0] = 0x64; bytes[1] = 0x9f; bytes[2] = 0x20;
        qToBigEndian<quint32>(123456000, bytes + 10);
        qToBigEndian<quint32>(quint32(std::llround(52.0 * 4294967296.0 / 360.0)), bytes + 14);
        qToBigEndian<quint32>(quint32(std::llround(21.0 * 4294967296.0 / 360.0)), bytes + 18);
        qToBigEndian<quint32>(12000000, bytes + 22);
        bytes[30] = 9; bytes[31] = 18;
        qToBigEndian<quint16>(2300, bytes + 32);
        bytes[93] = 1; bytes[94] = 2; bytes[95] = 0x31; bytes[96] = 4; bytes[97] = 5;
    }
    unsigned checksum = 0;
    const int checksumPos = m20 ? 68 : 99;
    for (int i = 0; i < checksumPos; ++i) {
        unsigned b = (bytes[i] >> 1) | ((bytes[i] & 1) << 7);
        b ^= b >> 2;
        const unsigned t6 = (checksum ^ (checksum >> 2) ^ (checksum >> 4)) & 1;
        const unsigned t7 = ((checksum >> 1) ^ (checksum >> 3) ^ (checksum >> 5)) & 1;
        const unsigned t = (checksum & 0x3f) | (t6 << 6) | (t7 << 7);
        unsigned s = (checksum >> 7) & 0xff;
        s ^= s >> 2;
        checksum = (((checksum & 0xff) << 8) | (b ^ t ^ s)) & 0xffff;
    }
    qToBigEndian<quint16>(quint16(checksum), bytes + checksumPos);
    std::vector<int> symbols;
    for (int i = 0; i < 100; ++i) { symbols.push_back(0); symbols.push_back(1); }
    for (char bit : QByteArray("10011001100110010100110010011001")) symbols.push_back(bit - '0');
    int previous = 0;
    for (int i = 0; i < frame.size(); ++i)
        for (int shift = 7; shift >= 0; --shift) {
            const int bit = previous ^ 1 ^ ((bytes[i] >> shift) & 1);
            symbols.push_back(1 - bit);
            symbols.push_back(bit);
            previous = bit;
        }
    for (int i = 0; i < 1000; ++i) { symbols.push_back(0); symbols.push_back(1); }
    const auto oneFrame = symbols;
    for (int i = 1; i < 5; ++i) symbols.insert(symbols.end(), oneFrame.begin(), oneFrame.end());
    const int baud = m20 ? 9600 : 9615;
    const int count = int(std::ceil(symbols.size() * 48000.0 / baud));
    QByteArray pcm(count * 2, '\0');
    for (int i = 0; i < count; ++i)
        qToLittleEndian<qint16>(symbols[i * baud / 48000] ? 22000 : -22000,
                               reinterpret_cast<uchar *>(pcm.data() + i * 2));
    return pcm;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto now = QDateTime::fromString("2026-01-01T12:00:00.000Z", Qt::ISODateWithMs);
    const QByteArray json = R"({
      "A":{"serial":"A","type":"RS41","lat":52.0,"lon":21.0,"alt":12000,"frequency":403.01,"tx_frequency":403.0,"time_received":"2026-01-01T11:59:00.000Z"},
      "B":{"serial":"B","type":"DFM","lat":53.0,"lon":21.0,"alt":10000,"frequency":402.0,"time_received":"2026-01-01T11:59:00.000Z"},
      "C":{"type":"RS41","lat":60.0,"lon":21.0,"alt":10000,"frequency":402.0,"time_received":"2026-01-01T11:59:00.000Z"},
      "D":{"type":"RS41","lat":52.0,"lon":21.0,"alt":10000,"frequency":402.0,"time_received":"2026-01-01T10:00:00.000Z"},
      "E":{"type":"RS41","lat":52.0,"lon":21.0,"alt":10000,"frequency":900.0,"time_received":"2026-01-01T11:59:00.000Z"}
    })";
    const auto list = parseSondeCandidates(json, Location{52, 21}, 500, now);
    if (list.size() != 2 || list[0].serial != "A" || list[0].frequencyHz != 403000000 ||
        list[1].type != "DFM" || list[1].distance < 100) return 1;
    const QByteArray boundaries = R"({
      "boundary":{"type":"RS41","lat":53.0,"lon":21.0,"alt":1,"frequency":400.0,"time_received":"2026-01-01T11:30:00.000000Z"},
      "old":{"lat":52.0,"lon":21.0,"alt":1,"frequency":400.0,"time_received":"2026-01-01T11:29:59.000000Z"},
      "future":{"lat":52.0,"lon":21.0,"alt":1,"frequency":406.0,"time_received":"2026-01-01T12:02:01.000000Z"}
    })";
    const double boundaryDistance = distanceKm(Location{52, 21}, Location{53, 21});
    const auto atBoundary = parseSondeCandidates(boundaries, Location{52, 21}, boundaryDistance, now);
    if (atBoundary.size() != 1 || atBoundary[0].serial != "boundary" ||
        !parseSondeCandidates(boundaries, Location{52, 21}, boundaryDistance - 0.001, now).isEmpty()) return 1;
    try { parseSondeCandidates("broken", Location{52, 21}, 500, now); return 1; }
    catch (const std::runtime_error &) {}
    const auto local = parseSondeTelemetry(R"({"id":"M10-412-2-10657","lat":52.5,"lon":21.5})");
    if (local.serial != "412-2-10657" || !local.position || local.position->latitude != 52.5) return 1;
    if (parseSondeTelemetry(R"({"id":"M20-609-2-02055","lat":52,"lon":21})").serial != "609-2-02055") return 1;
    if (parseSondeTelemetry(R"({"id":"A","lat":91,"lon":21})").position) return 1;
    try { parseSondeTelemetry(R"({"lat":52,"lon":21})"); return 1; }
    catch (const std::runtime_error &) {}
    SondeDecoder missing(nullptr, "/nonexistent/sdr-touch-rs41mod");
    QEventLoop failureLoop;
    bool missingReported = false;
    QObject::connect(&missing, &SondeDecoder::failed, &failureLoop, [&](const QString &message) {
        missingReported = !message.isEmpty();
        failureLoop.quit();
    });
    missing.start(403000000);
    if (!missingReported) {
        QTimer::singleShot(3000, &failureLoop, &QEventLoop::quit);
        failureLoop.exec();
    }
    if (!missingReported || missing.isActive()) return 1;
    missingReported = false;
    missing.start(403000000, SondeType::M20);
    if (!missingReported) {
        QTimer::singleShot(3000, &failureLoop, &QEventLoop::quit);
        failureLoop.exec();
    }
    if (!missingReported || missing.isActive()) return 1;
    missingReported = false;
    missing.start(403000000, SondeType::M10);
    if (!missingReported) {
        QTimer::singleShot(3000, &failureLoop, &QEventLoop::quit);
        failureLoop.exec();
    }
    if (!missingReported || missing.isActive()) return 1;
    if (argc < 2) {
        std::cout << "SondeHub filtering tests passed\n";
        return 0;
    }
    const bool syntheticM10 = QByteArray(argv[1]) == "--m10-synthetic";
    const bool syntheticM20 = QByteArray(argv[1]) == "--m20-synthetic";
    const bool synthetic = syntheticM10 || syntheticM20;
    const bool capturedIq = QByteArray(argv[1]) == "--rs41-iq";
    if (capturedIq && argc < 4) return 1;
    QByteArray wav;
    int data = -8, bits = 16;
    if (capturedIq) {
        QFile file(QString::fromLocal8Bit(argv[2]));
        if (!file.open(QIODevice::ReadOnly)) return 1;
        wav = file.readAll();
        if (wav.size() < kBlock * int(sizeof(std::complex<float>)) ||
            wav.size() % int(sizeof(std::complex<float>))) return 1;
    } else if (synthetic) {
        wav = syntheticMeteomodem(syntheticM20);
    } else {
        QFile file(QString::fromLocal8Bit(argv[1]));
        if (!file.open(QIODevice::ReadOnly)) return 1;
        wav = file.readAll();
        data = wav.indexOf("data");
        if (wav.size() < 44 || data < 0 || data + 8 >= wav.size()) return 1;
        const auto *header = reinterpret_cast<const uchar *>(wav.constData());
        bits = qFromLittleEndian<quint16>(header + 34);
        if (qFromLittleEndian<quint16>(header + 22) != 1 ||
            qFromLittleEndian<quint32>(header + 24) != 48000 || (bits != 8 && bits != 16)) return 1;
    }
    const int bytesPerSample = bits / 8;
    const bool m10 = syntheticM10 || (!capturedIq && !synthetic && argc >= 3 && QByteArray(argv[2]) == "M10");
    const auto type = syntheticM20 ? SondeType::M20 : m10 ? SondeType::M10 : SondeType::RS41;
    Processor processor;
    processor.setMode(Mode::DigitalVoice);
    SondeDecoder decoder;
    bool decoded = false, failed = false;
    QObject::connect(&decoder, &SondeDecoder::telemetry, &app, [&](const QString &text) {
        if (syntheticM20) {
            const auto telemetry = parseSondeTelemetry(text);
            if (telemetry.serial == "609-2-02055" && telemetry.data.value("type").toString() == "M20" &&
                telemetry.position && std::abs(telemetry.position->latitude - 52) < 0.00001 &&
                std::abs(telemetry.position->longitude - 21) < 0.00001 &&
                std::abs(telemetry.data.value("alt").toDouble() - 12000) < 0.01 &&
                telemetry.data.value("freq").toInt() == 403000) {
                decoded = true;
                app.quit();
            }
        } else {
            const QString expected = capturedIq ? QString::fromLocal8Bit(argv[3]) :
                                                 QString::fromLatin1(m10 ? "M10-301-1-01284" : "K1930308");
            if (text.contains(expected)) { decoded = true; app.quit(); }
        }
    });
    QObject::connect(&decoder, &SondeDecoder::failed, &app, [&](const QString &message) {
        std::cerr << message.toStdString() << '\n';
        failed = true;
        app.quit();
    });
    decoder.start(m10 || syntheticM20 ? 403000000 : 402000000, type,
                  capturedIq || syntheticM20 ? 24000 : 0);
    int position = capturedIq ? 0 : data + 8;
    double phase = 0;
    constexpr double pi = 3.14159265358979323846;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (capturedIq) {
            const int size = kBlock * int(sizeof(std::complex<float>));
            if (position + size > wav.size()) { app.quit(); return; }
            decoder.feedIq(wav.mid(position, size));
            position += size;
            return;
        }
        if (position + kBlock / 8 * bytesPerSample >= wav.size()) { app.quit(); return; }
        std::vector<std::complex<float>> iq(kBlock);
        for (int i = 0; i < kBlock; ++i) {
            const int offset = position + i / 8 * bytesPerSample;
            const double sample = bits == 8 ? (static_cast<unsigned char>(wav[offset]) - 128) / 128.0 :
                qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(wav.constData() + offset)) / 32768.0;
            phase += 2 * pi * (3000 * sample - (syntheticM20 ? 24000 : 0)) / kSampleRate;
            iq[i] = {float(0.5 * std::cos(phase)), float(0.5 * std::sin(phase))};
        }
        position += kBlock / 8 * bytesPerSample;
        if (m10) {
            const auto block = processor.process(iq, 0, false);
            decoder.feed(QByteArray(reinterpret_cast<const char *>(block.audio.data()),
                                    int(block.audio.size() * sizeof(std::int16_t))));
        } else {
            decoder.feedIq(QByteArray(reinterpret_cast<const char *>(iq.data()),
                                      int(iq.size() * sizeof(iq[0]))));
        }
    });
    timer.start(6);
    QTimer::singleShot(90000, &app, &QCoreApplication::quit);
    app.exec();
    if (failed || !decoded) {
        std::cerr << (syntheticM20 ? "M20" : m10 ? "M10" : "RS41") << " signal did not decode\n";
        return 1;
    }
    std::cout << (syntheticM20 ? "M20" : m10 ? "M10" : "RS41") << " signal decoded\n";
    return 0;
}
