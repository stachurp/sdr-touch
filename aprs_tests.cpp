#include "aprs.hpp"
#include "dsp.hpp"
#include <QCoreApplication>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

namespace {
QByteArray signal(bool corrupt) {
    QByteArray frame;
    auto address = [&frame](const char *call, char ssid) {
        for (int i = 0; i < 6; ++i) frame.append(char(call[i] << 1));
        frame.append(ssid);
    };
    address("APRS  ", 0x60);
    address("N0CALL", 0x60);
    address("WIDE1 ", char(0xe3));
    frame.append(char(0x03));
    frame.append(char(0xf0));
    frame.append(">TEST APRS");
    quint16 crc = 0xffff;
    for (unsigned char byte : frame) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? quint16((crc >> 1) ^ 0x8408) : quint16(crc >> 1);
    }
    crc ^= 0xffff;
    if (corrupt) crc ^= 1;
    frame.append(char(crc & 0xff));
    frame.append(char(crc >> 8));
    std::vector<int> bits;
    auto flag = [&bits] {
        for (int i = 0; i < 8; ++i) bits.push_back((0x7e >> i) & 1);
    };
    for (int i = 0; i < 80; ++i) flag();
    int ones = 0;
    for (unsigned char byte : frame) {
        for (int i = 0; i < 8; ++i) {
            const int bit = (byte >> i) & 1;
            bits.push_back(bit);
            ones = bit ? ones + 1 : 0;
            if (ones == 5) { bits.push_back(0); ones = 0; }
        }
    }
    for (int i = 0; i < 15; ++i) flag();
    QByteArray result;
    constexpr double pi = 3.14159265358979323846;
    double phase = 0;
    bool mark = false;
    for (int bit : bits) {
        if (!bit) mark = !mark;
        for (int i = 0; i < 40; ++i) {
            phase += 2 * pi * (mark ? 1200 : 2200) / 48000;
            const qint16 sample = qint16(std::lround(20000 * std::sin(phase)));
            char stereo[4];
            qToLittleEndian(sample, reinterpret_cast<uchar *>(stereo));
            qToLittleEndian(sample, reinterpret_cast<uchar *>(stereo + 2));
            result.append(stereo, 4);
        }
    }
    result.append(QByteArray(48000 * 4 / 2, '\0'));
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    AprsDecoder decoder;
    QByteArray audio = signal(true) + signal(false);
    int position = 0;
    Processor processor;
    processor.setMode(Mode::DigitalVoice);
    double phase = 0;
    int packets = 0;
    bool failed = false;
    QObject::connect(&decoder, &AprsDecoder::packetReceived, &app, [&](const QString &packet, qint64 frequency) {
        ++packets;
        if (packet != "N0CALL>APRS,WIDE1-1*:>TEST APRS" || frequency != 144800000) {
            std::cerr << "Unexpected APRS packet: " << packet.toStdString() << '\n';
            failed = true;
        }
    });
    QObject::connect(&decoder, &AprsDecoder::failed, &app, [&](const QString &message) {
        std::cerr << message.toStdString() << '\n';
        failed = true;
        app.exit(1);
    });
    decoder.configure(true, 144800000);
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (position < audio.size()) {
            std::vector<std::complex<float>> iq(kBlock);
            constexpr double pi = 3.14159265358979323846;
            for (int i = 0; i < kBlock; ++i) {
                const int offset = position + (i / 8) * 4;
                const qint16 sample = offset + 1 < audio.size() ?
                    qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(audio.constData() + offset)) : 0;
                phase += 2 * pi * 2500 * sample / (20000.0 * kSampleRate);
                iq[i] = {float(0.5 * std::cos(phase)), float(0.5 * std::sin(phase))};
            }
            position += kBlock / 8 * 4;
            const auto block = processor.process(iq, 0, false);
            decoder.feed(QByteArray(reinterpret_cast<const char *>(block.audio.data()),
                                    int(block.audio.size() * sizeof(std::int16_t))));
        } else {
            timer.stop();
            QTimer::singleShot(1000, &app, &QCoreApplication::quit);
        }
    });
    timer.start(2);
    QTimer::singleShot(10000, &app, [&] {
        std::cerr << "APRS test timed out\n";
        failed = true;
        app.exit(1);
    });
    const int result = app.exec();
    if (result || failed || packets != 1) {
        std::cerr << "Expected one valid packet, received " << packets << '\n';
        return 1;
    }
    std::cout << "APRS decoder accepted a valid frame and rejected a corrupt CRC\n";
    return 0;
}
