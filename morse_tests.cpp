#include "dsp.hpp"
#include "morse.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cmath>
#include <complex>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    constexpr double pi = 3.14159265358979323846;
    constexpr int dot = kSampleRate * 80 / 1000;
    std::vector<std::pair<int, bool>> segments;
    auto add = [&](int samples, bool on) { segments.emplace_back(samples, on); };
    add(2 * kSampleRate, true);
    add(2 * kSampleRate, false);
    for (int repeat = 0; repeat < 2; ++repeat) {
        for (const char *letter : {"...", "---", "...", "", "-", ".", "...", "-"}) {
            if (!*letter) { add(4 * dot, false); continue; }
            for (const char *p = letter; *p; ++p) {
                add((*p == '.' ? 1 : 3) * dot, true);
                if (p[1]) add(dot, false);
            }
            add(3 * dot, false);
        }
        add(4 * dot, false);
    }
    add(3 * kSampleRate, false);
    MorseDecoder decoder;
    Processor processor;
    processor.setMode(Mode::CW);
    std::vector<std::complex<float>> outside(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        const double phase = 2 * pi * (-24000 + 2000) * i / kSampleRate;
        outside[i] = 0.15f * std::complex<float>(std::cos(phase), std::sin(phase));
    }
    if (processor.process(outside, 24000, true).peakDbfs > -50) {
        std::cerr << "Out-of-band CW signal detected\n";
        return 1;
    }
    QString decoded;
    bool failed = false;
    QObject::connect(&decoder, &MorseDecoder::textReceived, &app, [&](const QString &text) { decoded += text; });
    const auto failureConnection = QObject::connect(&decoder, &MorseDecoder::failed, &app, [&](const QString &message) {
        std::cerr << message.toStdString() << '\n';
        failed = true;
        app.exit(1);
    });
    decoder.configure(true, 7025000);
    int segment = 0, remaining = segments.front().first;
    qint64 sampleNumber = 0, audioNumber = 0;
    double sine = 0, cosine = 0, energy = 0, peak = -200;
    int used = 0, underruns = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (segment >= int(segments.size())) {
            timer.stop();
            QTimer::singleShot(1000, &app, &QCoreApplication::quit);
            return;
        }
        std::vector<std::complex<float>> iq(kBlock);
        for (auto &sample : iq) {
            if (segment < int(segments.size()) && segments[segment].second) {
                const double phase = -2 * pi * 24000 * sampleNumber / kSampleRate;
                sample = 0.15f * std::complex<float>(std::cos(phase), std::sin(phase));
            }
            ++sampleNumber;
            if (segment < int(segments.size()) && --remaining == 0) {
                ++segment;
                if (segment < int(segments.size())) remaining = segments[segment].first;
            }
        }
        const bool measure = sampleNumber > kSampleRate && sampleNumber < 2 * kSampleRate;
        const auto block = processor.process(iq, 24000, measure);
        if (measure)
            peak = std::max(peak, block.peakDbfs);
        for (std::size_t i = 0; i < block.audio.size(); i += 2, ++audioNumber) {
            if (audioNumber < kAudioRate || audioNumber >= 2 * kAudioRate) continue;
            const double value = block.audio[i] / 32767.0;
            sine += value * std::sin(2 * pi * 700 * audioNumber / kAudioRate);
            cosine += value * std::cos(2 * pi * 700 * audioNumber / kAudioRate);
            energy += value * value;
            ++used;
        }
        if (block.dspUnderrun) {
            ++underruns;
            decoder.gap();
        } else {
            decoder.feed(QByteArray(reinterpret_cast<const char *>(block.audio.data()),
                                    int(block.audio.size() * sizeof(std::int16_t))));
        }
    });
    timer.start(6);
    QTimer::singleShot(90000, &app, [&] {
        std::cerr << "CW test timed out\n";
        failed = true;
        app.exit(1);
    });
    const int result = app.exec();
    const double amplitude = used ? 2 * std::hypot(sine, cosine) / used : 0;
    const double rms = used ? std::sqrt(energy / used) : 0;
    if (result || failed || !decoded.contains("SOS TEST") || amplitude < 0.005 ||
        rms < 0.005 || amplitude / rms < 1.0 || peak < -25) {
        std::cerr << "CW failed: text=" << decoded.toStdString() << " amplitude=" << amplitude
                  << " rms=" << rms << " peak=" << peak << " underruns=" << underruns << '\n';
        return 1;
    }
    QObject::disconnect(failureConnection);
    int restarts = 0;
    QObject::connect(&decoder, &MorseDecoder::started, &app, [&](qint64 frequency) {
        if (frequency == 7025000) ++restarts;
    });
    decoder.gap();
    if (restarts != 1) {
        std::cerr << "CW decoder did not restart after a stream gap\n";
        return 1;
    }
    bool malformedReported = false;
    QObject::connect(&decoder, &MorseDecoder::failed, &app, [&](const QString &) { malformedReported = true; });
    decoder.feed(QByteArray(1, '\0'));
    if (!malformedReported) {
        std::cerr << "CW decoder accepted malformed audio\n";
        return 1;
    }
    MorseDecoder missing(nullptr, "/nonexistent/multimon-ng");
    QEventLoop errorLoop;
    bool missingReported = false;
    QObject::connect(&missing, &MorseDecoder::failed, &errorLoop, [&](const QString &) {
        missingReported = true;
        errorLoop.quit();
    });
    QTimer::singleShot(3000, &errorLoop, &QEventLoop::quit);
    missing.configure(true, 7025000);
    if (!missingReported) errorLoop.exec();
    if (!missingReported) {
        std::cerr << "Missing CW decoder was not reported\n";
        return 1;
    }
    std::cout << "CW IQ produced a 700 Hz tone and decoded SOS TEST; underruns=" << underruns << '\n';
    return 0;
}
