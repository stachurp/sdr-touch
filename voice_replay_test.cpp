#include "voice.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4) return 2;
    QFile input(argv[2]);
    if (!input.open(QIODevice::ReadOnly)) return 2;
    const VoiceMode mode = QByteArray(argv[3]) == "dmr" ? VoiceMode::DMR : VoiceMode::YSF;
    VoiceDecoder decoder(nullptr, argv[1]);
    qint64 audioBytes = 0;
    bool nonzero = false, metadata = false, failed = false;
    QObject::connect(&decoder, &VoiceDecoder::audioReady, [&](const QByteArray &pcm) {
        audioBytes += pcm.size();
        for (char c : pcm) if (c != 0) nonzero = true;
    });
    QObject::connect(&decoder, &VoiceDecoder::metadataChanged, [&](const QString &text) {
        if (text.contains(mode == VoiceMode::DMR ? "Źródło:" : "Znak źródłowy:")) metadata = true;
    });
    QObject::connect(&decoder, &VoiceDecoder::failed, [&](const QString &text) {
        std::cerr << text.toStdString() << '\n';
        failed = true;
        app.quit();
    });
    decoder.configure(mode, 145000000);
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        const QByteArray mono = input.read(1920);
        if (mono.size() != 1920 || (audioBytes > 96000 && nonzero && metadata)) {
            timer.stop();
            app.quit();
            return;
        }
        QByteArray stereo;
        stereo.reserve(mono.size() * 2);
        for (int i = 0; i < mono.size(); i += 2) {
            stereo.append(mono.constData() + i, 2);
            stereo.append(mono.constData() + i, 2);
        }
        decoder.feed(stereo);
    });
    timer.start(20);
    QTimer::singleShot(30000, &app, &QCoreApplication::quit);
    app.exec();
    std::cout << "audio bytes=" << audioBytes << " nonzero=" << nonzero << " metadata=" << metadata << '\n';
    return !failed && audioBytes > 96000 && nonzero && metadata ? 0 : 1;
}
