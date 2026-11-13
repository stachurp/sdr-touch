#include "digital.hpp"
#include "dsp.hpp"
#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstring>
#include <iostream>
#include <thread>

QByteArray readWav(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open WAV");
    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    char magic[4];
    quint32 size;
    if (in.readRawData(magic, 4) != 4 || std::memcmp(magic, "RIFF", 4) != 0) throw std::runtime_error("Not RIFF");
    in >> size;
    if (in.readRawData(magic, 4) != 4 || std::memcmp(magic, "WAVE", 4) != 0) throw std::runtime_error("Not WAV");
    bool formatOK = false;
    while (!in.atEnd()) {
        if (in.readRawData(magic, 4) != 4) break;
        in >> size;
        if (std::memcmp(magic, "fmt ", 4) == 0) {
            quint16 format, channels, align, bits;
            quint32 rate, byteRate;
            if (size < 16) throw std::runtime_error("Invalid WAV format");
            in >> format >> channels >> rate >> byteRate >> align >> bits;
            formatOK = format == 1 && channels == 1 && rate == 12000 && bits == 16;
            if (!file.seek(file.pos() + size - 16 + (size % 2))) throw std::runtime_error("WAV seek failed");
        } else if (std::memcmp(magic, "data", 4) == 0) {
            if (!formatOK) throw std::runtime_error("Expected 12 kHz mono PCM");
            QByteArray data = file.read(size);
            if (data.size() != int(size)) throw std::runtime_error("Truncated WAV data");
            return data;
        } else if (!file.seek(file.pos() + size + (size % 2))) {
            throw std::runtime_error("WAV seek failed");
        }
    }
    throw std::runtime_error("No WAV data");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4 && args.size() != 5) return 2;
    const bool throughDsp = args.size() == 5 && args[4] == "--dsp";
    DigitalMode mode = args[1] == "FT8" ? DigitalMode::FT8 :
                       args[1] == "FT4" ? DigitalMode::FT4 : DigitalMode::WSPR;
    QByteArray samples;
    try { samples = readWav(args[2]); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
    DigitalDecoder decoder;
    decoder.configure(mode, 14095600);
    bool found = false;
    QObject::connect(&decoder, &DigitalDecoder::decoded, &app, [&](const DigitalMessage &message) {
        std::cout << message.station.toStdString() << " " << message.text.toStdString() << '\n';
        if (message.station == args[3]) found = true;
    });
    QObject::connect(&decoder, &DigitalDecoder::failed, &app, [&](const QString &error) {
        std::cerr << error.toStdString() << '\n';
        app.exit(1);
    });
    QObject::connect(&decoder, &DigitalDecoder::statusChanged, &app, [&](const QString &status) {
        if (status.startsWith(QString::fromUtf8("Dekodowanie zakończone"))) {
            std::cout << status.toStdString() << '\n';
            app.exit(found ? 0 : 1);
        }
    });
    const qint64 total = slotLengthSamples(mode);
    const qint64 startMs = 1704067200000LL;
    std::unique_ptr<Processor> processor;
    if (throughDsp) {
        processor = std::make_unique<Processor>();
        processor->setMode(Mode::DigitalUSB);
    }
    const qint64 warmup = throughDsp ? 12000 : 4;
    const qint64 extra = throughDsp ? 3000 : 0;
    auto next = std::chrono::steady_clock::now();
    for (qint64 block = 0; block * 64 < total + warmup + extra; ++block) {
        QByteArray stereo(1024, '\0');
        if (throughDsp) {
            std::vector<std::complex<float>> iq(kBlock);
            for (int i = 0; i < kBlock; ++i) {
                qint64 source = block * 64 + i / 32 - warmup;
                if (source >= 0 && source * 2 + 1 < samples.size()) {
                    qint16 value = qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(samples.constData() + source * 2));
                    iq[i] = {float(value / 32768.0 * 0.2), 0};
                }
            }
            const auto result = processor->process(iq, 0, false);
            stereo = QByteArray(reinterpret_cast<const char *>(result.audio.data()), int(result.audio.size() * 2));
            next += std::chrono::microseconds(5333);
            std::this_thread::sleep_until(next);
            decoder.feed(stereo, startMs - 1000 + ((block + 1) * 64) / 12);
        } else {
            for (int i = 0; i < 64; ++i) {
                qint64 source = block * 64 + i - warmup;
                if (source < 0 || source * 2 + 1 >= samples.size()) continue;
                for (int repeat = 0; repeat < 4; ++repeat) {
                    const int offset = i * 16 + repeat * 4;
                    stereo[offset] = stereo[offset + 2] = samples[int(source * 2)];
                    stereo[offset + 1] = stereo[offset + 3] = samples[int(source * 2 + 1)];
                }
            }
            decoder.feed(stereo, startMs + 130 + (block * 64) / 12);
        }
    }
    QTimer::singleShot(90000, &app, [&] {
        std::cerr << "Decoder replay timed out\n";
        app.exit(1);
    });
    return app.exec();
}
