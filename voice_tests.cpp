#include "voice.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QtEndian>
#include <cstdio>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc > 1) {
        const bool ysf = QByteArray(argv[1]) == "-fy";
        char frame[4];
        qToLittleEndian<qint16>(1000, reinterpret_cast<uchar *>(frame));
        qToLittleEndian<qint16>(-1000, reinterpret_cast<uchar *>(frame + 2));
        std::fwrite(frame, 1, 1, stdout);
        std::fflush(stdout);
        QTimer::singleShot(20, [frame, ysf] {
            std::fwrite(frame + 1, 1, ysf ? 1 : 3, stdout);
            std::fflush(stdout);
            std::fputs(ysf ? "Sync: +YSF SRC: TEST1\n" :
                       "Sync: +DMR Color Code=08 FEC ERR\nSync: +DMR Color Code=04\n SLOT 2 TGT=91 SRC=123 Group Call\n", stderr);
            std::fflush(stderr);
        });
        QTimer::singleShot(2000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    for (const auto mode : {VoiceMode::DMR, VoiceMode::YSF}) {
        VoiceDecoder decoder(nullptr, QCoreApplication::applicationFilePath());
        QByteArray audio;
        QString metadata;
        QString error;
        QEventLoop loop;
        QObject::connect(&decoder, &VoiceDecoder::audioReady, [&](const QByteArray &pcm) {
            audio += pcm;
            if (!metadata.isEmpty()) loop.quit();
        });
        QObject::connect(&decoder, &VoiceDecoder::metadataChanged, [&](const QString &text) {
            if (text.contains(mode == VoiceMode::DMR ? "Źródło:" : "Znak źródłowy:")) metadata = text;
            if (!audio.isEmpty() && !metadata.isEmpty()) loop.quit();
        });
        QObject::connect(&decoder, &VoiceDecoder::failed, [&](const QString &text) {
            error = text;
            loop.quit();
        });
        decoder.configure(mode, 145000000);
        QTimer::singleShot(1500, &loop, &QEventLoop::quit);
        loop.exec();
        if (!error.isEmpty() || audio.size() != 24 || metadata.isEmpty() ||
            (mode == VoiceMode::DMR && !metadata.contains("Color code: 04")) ||
            qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(audio.constData())) != 1000 ||
            qFromLittleEndian<qint16>(reinterpret_cast<const uchar *>(audio.constData() + 2)) !=
                (mode == VoiceMode::DMR ? -1000 : 1000)) {
            std::cerr << "Voice stream or metadata test failed\n";
            return 1;
        }
        decoder.feed(QByteArray(3, 'x'));
        if (error.isEmpty()) {
            std::cerr << "Invalid voice input was not reported\n";
            return 1;
        }
    }
    VoiceDecoder missing(nullptr, "/nonexistent/sdr-touch-dsd-fme");
    QEventLoop loop;
    bool failed = false;
    QObject::connect(&missing, &VoiceDecoder::failed, [&](const QString &) { failed = true; loop.quit(); });
    missing.configure(VoiceMode::DMR, 145000000);
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    if (!failed) loop.exec();
    if (!failed) {
        std::cerr << "Missing decoder was not reported\n";
        return 1;
    }
    return 0;
}
