#include "receiver.hpp"
#include "scan_log.hpp"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Constants.h>
#include <QDateTime>
#include <QFile>
#include <QMutexLocker>
#include <QtEndian>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::int64_t minHz = 10000;
constexpr std::int64_t maxHz = 2000000000;

QString modeName(Mode mode) {
    switch (mode) {
    case Mode::LSB: return "LSB";
    case Mode::USB: return "USB";
    case Mode::DigitalUSB: return "USB";
    case Mode::DigitalVoice: return "NFM";
    case Mode::AM: return "AM";
    case Mode::NFM: return "NFM";
    case Mode::WFM: return "WFM";
    case Mode::CW: return "CW";
    }
    throw std::invalid_argument("Unknown mode");
}

void validate(const ReceiverSettings &s) {
    if (s.frequency < minHz || s.frequency > maxHz)
        throw std::invalid_argument("Czestotliwosc jest poza zakresem RSP1");
    if (s.scan && (s.scanStart < minHz || s.scanEnd > maxHz || s.scanStart > s.scanEnd ||
                   s.scanStep < 1 || s.threshold < -200 || s.threshold > 0))
        throw std::invalid_argument("Nieprawidlowe parametry skanowania");
    if (!std::isfinite(s.squelchThresholdDbfs) ||
        s.squelchThresholdDbfs < -140 || s.squelchThresholdDbfs > 0)
        throw std::invalid_argument("Nieprawidlowy prog squelch");
    if (s.meteorIq && (s.meteorCapturePath.isEmpty() || s.scan || s.sondeIq ||
                       s.mode != Mode::WFM))
        throw std::invalid_argument("Nieprawidlowa sciezka lub tryb nagrywania LRPT");
    const auto &f = s.frontend;
    if (f.rfReduction < 0 || f.rfReduction > 3 || f.ifReduction < 20 || f.ifReduction > 59 ||
        f.agcSetpoint < -60 || f.agcSetpoint > 0 || !std::isfinite(f.ppm) || std::abs(f.ppm) > 100)
        throw std::invalid_argument("Nieprawidlowe ustawienia SDRplay");
}

bool sameTuning(const ReceiverSettings &a, const ReceiverSettings &b) {
    return a.frequency == b.frequency && a.mode == b.mode && a.scan == b.scan &&
           a.scanStart == b.scanStart && a.scanEnd == b.scanEnd && a.scanStep == b.scanStep &&
           a.threshold == b.threshold && a.scanId == b.scanId;
}

struct DeviceDeleter {
    void operator()(SoapySDR::Device *device) const { if (device) SoapySDR::Device::unmake(device); }
};

struct StreamGuard {
    SoapySDR::Device *device;
    SoapySDR::Stream *stream;
    bool active = false;
    ~StreamGuard() {
        if (active) device->deactivateStream(stream);
        if (stream) device->closeStream(stream);
    }
};
}

Receiver::Receiver(QString resultPath, QObject *parent)
    : QThread(parent), resultPath_(std::move(resultPath)) {}

Receiver::~Receiver() { stop(); }

void Receiver::restart() {
    if (isRunning()) throw std::logic_error("Odbiornik jest już uruchomiony");
    stopping_ = false;
    start();
}

void Receiver::stop() {
    stopping_ = true;
    if (isRunning()) wait();
}

void Receiver::configure(const ReceiverSettings &settings) {
    validate(settings);
    QMutexLocker lock(&mutex_);
    settings_ = settings;
    ++revision_;
}

void Receiver::run() {
    try {
        std::unique_ptr<SoapySDR::Device, DeviceDeleter> device(
            SoapySDR::Device::make(SoapySDR::Kwargs{{"driver", "sdrplay"}}));
        if (!device) throw std::runtime_error("Nie znaleziono SDRplay");
        device->setSampleRate(SOAPY_SDR_RX, 0, kSampleRate);
        device->setBandwidth(SOAPY_SDR_RX, 0, 200000);
        device->setFrequency(SOAPY_SDR_RX, 0, 7100000);
        if (device->hasGainMode(SOAPY_SDR_RX, 0)) device->setGainMode(SOAPY_SDR_RX, 0, true);
        if (device->hasDCOffsetMode(SOAPY_SDR_RX, 0)) device->setDCOffsetMode(SOAPY_SDR_RX, 0, true);
        const auto gains = device->listGains(SOAPY_SDR_RX, 0);
        const auto settingInfo = device->getSettingInfo();
        if (!device->hasGainMode(SOAPY_SDR_RX, 0) || !device->hasFrequencyCorrection(SOAPY_SDR_RX, 0) ||
            std::find(gains.begin(), gains.end(), "RFGR") == gains.end() ||
            std::find(gains.begin(), gains.end(), "IFGR") == gains.end() ||
            std::none_of(settingInfo.begin(), settingInfo.end(), [](const SoapySDR::ArgInfo &info) {
                return info.key == "agc_setpoint";
            }))
            throw std::runtime_error("Sterownik nie obsluguje wymaganych ustawien SDRplay");
        StreamGuard stream{device.get(), device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32)};
        if (!stream.stream) throw std::runtime_error("Nie mozna utworzyc strumienia SDR");
        int rc = device->activateStream(stream.stream);
        if (rc != 0) throw std::runtime_error(std::string("Nie mozna uruchomic strumienia: ") + SoapySDR_errToStr(rc));
        stream.active = true;
        Processor processor;
        SpectrumAnalyzer spectrum;
        ReceiverSettings current;
        QFile meteorCapture;
        std::uint64_t revision = 0;
        std::int64_t frequency = 0;
        double shift = 0;
        bool scanning = false;
        double bandwidth = 200000;
        ScanDetector scanDetector;
        SquelchGate squelch;
        std::unique_ptr<ScanLog> resultLog;
        std::vector<std::complex<float>> samples(kBlock);
        int filled = 0;
        int timeouts = 0;
        int underruns = 0;
        auto tunedAt = std::chrono::steady_clock::now();
        auto lastLevel = tunedAt;
        auto lastBlock = tunedAt;
        auto lastSpectrum = tunedAt;
        auto tune = [&](std::int64_t target) {
            std::int64_t offset = current.mode == Mode::WFM ? 0 : 24000;
            if (target + offset > maxHz) offset = -offset;
            device->setFrequency(SOAPY_SDR_RX, 0, double(target + offset));
            const double actual = device->getFrequency(SOAPY_SDR_RX, 0);
            if (!std::isfinite(actual) || std::abs(actual - double(target + offset)) > 1000)
                throw std::runtime_error("Odbiornik nie potwierdzil ustawionej czestotliwosci");
            frequency = target;
            shift = double(offset);
            filled = 0;
            scanDetector.reset();
            squelch.reset();
            tunedAt = std::chrono::steady_clock::now();
        };
        auto applyFrontend = [&](const FrontendSettings &f) {
            const auto rfRange = device->getGainRange(SOAPY_SDR_RX, 0, "RFGR");
            const auto ifRange = device->getGainRange(SOAPY_SDR_RX, 0, "IFGR");
            if (f.rfReduction < rfRange.minimum() || f.rfReduction > rfRange.maximum() ||
                f.ifReduction < ifRange.minimum() || f.ifReduction > ifRange.maximum())
                throw std::runtime_error("Ustawienie wzmocnienia jest poza zakresem urzadzenia");
            if (device->readSetting("agc_setpoint") != std::to_string(f.agcSetpoint))
                device->writeSetting("agc_setpoint", std::to_string(f.agcSetpoint));
            if (device->getGainMode(SOAPY_SDR_RX, 0) != f.agc)
                device->setGainMode(SOAPY_SDR_RX, 0, f.agc);
            if (device->getGain(SOAPY_SDR_RX, 0, "RFGR") != f.rfReduction)
                device->setGain(SOAPY_SDR_RX, 0, "RFGR", f.rfReduction);
            if (!f.agc && device->getGain(SOAPY_SDR_RX, 0, "IFGR") != f.ifReduction)
                device->setGain(SOAPY_SDR_RX, 0, "IFGR", f.ifReduction);
            if (device->getFrequencyCorrection(SOAPY_SDR_RX, 0) != f.ppm)
                device->setFrequencyCorrection(SOAPY_SDR_RX, 0, f.ppm);
            if (device->readSetting("agc_setpoint") != std::to_string(f.agcSetpoint) ||
                device->getGainMode(SOAPY_SDR_RX, 0) != f.agc ||
                device->getGain(SOAPY_SDR_RX, 0, "RFGR") != f.rfReduction ||
                (!f.agc && device->getGain(SOAPY_SDR_RX, 0, "IFGR") != f.ifReduction) ||
                std::abs(device->getFrequencyCorrection(SOAPY_SDR_RX, 0) - f.ppm) > 0.001)
                throw std::runtime_error("Sterownik nie potwierdzil ustawien SDRplay");
            emit frontendApplied();
        };
        emit statusChanged("RSP1: odbiornik gotowy");
        while (!stopping_) {
            ReceiverSettings requested;
            std::uint64_t newRevision;
            {
                QMutexLocker lock(&mutex_);
                requested = settings_;
                newRevision = revision_;
            }
            if (newRevision != revision) {
                validate(requested);
                const bool tuningChanged = revision == 0 || !sameTuning(current, requested);
                const bool frontendChanged = revision == 0 || current.frontend != requested.frontend;
                const bool captureChanged = current.meteorIq != requested.meteorIq ||
                    current.meteorCapturePath != requested.meteorCapturePath;
                const bool squelchChanged = revision == 0 ||
                    current.squelchEnabled != requested.squelchEnabled ||
                    current.squelchThresholdDbfs != requested.squelchThresholdDbfs;
                current = requested;
                revision = newRevision;
                if (frontendChanged) applyFrontend(current.frontend);
                if (squelchChanged) squelch.reset();
                if (captureChanged) {
                    if (meteorCapture.isOpen()) {
                        if (!meteorCapture.flush())
                            throw std::runtime_error("Nie mozna zapisac pliku IQ Meteor-M");
                        const QString path = meteorCapture.fileName();
                        meteorCapture.close();
                        emit meteorCaptureStopped(path);
                    }
                    if (current.meteorIq) {
                        meteorCapture.setFileName(current.meteorCapturePath);
                        if (!meteorCapture.open(QIODevice::WriteOnly | QIODevice::Truncate))
                            throw std::runtime_error(
                                QString("Nie mozna utworzyc pliku IQ: %1")
                                    .arg(meteorCapture.errorString()).toStdString());
                        emit meteorCaptureStarted(current.frequency);
                    }
                }
                if (!tuningChanged) continue;
                processor.setMode(current.mode);
                scanning = current.scan;
                // WFM scanning also measures the spectrum beside the reception channel.
                const double wantedBandwidth = scanning && current.mode == Mode::WFM ? 300000 : 200000;
                if (bandwidth != wantedBandwidth) {
                    device->setBandwidth(SOAPY_SDR_RX, 0, wantedBandwidth);
                    bandwidth = device->getBandwidth(SOAPY_SDR_RX, 0);
                    if (std::abs(bandwidth - wantedBandwidth) > 1)
                        throw std::runtime_error("Odbiornik nie potwierdzil pasma skanowania");
                }
                if (scanning) {
                    resultLog = std::make_unique<ScanLog>(resultPath_);
                    tune(current.scanStart);
                    emit statusChanged("Skanowanie: odbior audio wstrzymany");
                } else {
                    resultLog.reset();
                    tune(current.frequency);
                    emit statusChanged("RSP1: odbior");
                }
            }
            void *buffers[] = {samples.data() + filled};
            int flags = 0;
            long long timeNs = 0;
            int count = device->readStream(stream.stream, buffers, kBlock - filled, flags, timeNs, 100000);
            if (count == SOAPY_SDR_TIMEOUT || count == 0) {
                if (++timeouts >= 50) throw std::runtime_error("Brak probek z odbiornika");
                continue;
            }
            if (count == SOAPY_SDR_OVERFLOW) {
                filled = 0;
                scanDetector.reset();
                emit audioGap();
                emit statusChanged("RSP1: przepelnienie strumienia, probki utracone");
                continue;
            }
            if (count < 0) throw std::runtime_error(std::string("Blad strumienia SDR: ") + SoapySDR_errToStr(count));
            timeouts = 0;
            filled += count;
            if (filled < kBlock) continue;
            filled = 0;
            if (current.meteorIq && meteorCapture.isOpen()) {
                QByteArray block(int(samples.size() * 2 * sizeof(qint16)), Qt::Uninitialized);
                auto *output = block.data();
                for (const auto &sample : samples) {
                    const auto scale = [](float value) {
                        const float bounded = std::clamp(value, -1.0f, 1.0f);
                        return qint16(std::lround(bounded * 32767.0f));
                    };
                    qToLittleEndian(scale(sample.real()), output);
                    output += sizeof(qint16);
                    qToLittleEndian(scale(sample.imag()), output);
                    output += sizeof(qint16);
                }
                if (meteorCapture.write(block) != block.size())
                    throw std::runtime_error(
                        QString("Blad zapisu pliku IQ Meteor-M: %1")
                            .arg(meteorCapture.errorString()).toStdString());
            }
            auto now = std::chrono::steady_clock::now();
            if (now - lastBlock >= std::chrono::milliseconds(100)) {
                emit blockReceived();
                lastBlock = now;
            }
            if (scanning && now - tunedAt <
                std::chrono::milliseconds(current.mode == Mode::WFM ? 400 : 250)) continue;
            if (!scanning && spectrumEnabled_ && now - tunedAt >= std::chrono::milliseconds(200) &&
                now - lastSpectrum >= std::chrono::milliseconds(100) &&
                !spectrumPending_.exchange(true)) {
                const auto bins = spectrum.process(samples);
                emit spectrumReady(QVector<float>(bins.begin(), bins.end()), frequency + qint64(shift), frequency);
                lastSpectrum = now;
            }
            const bool reportLevel = scanning || now - lastLevel >= std::chrono::milliseconds(200);
            const bool squelchActive = current.squelchEnabled &&
                current.mode != Mode::DigitalUSB && current.mode != Mode::DigitalVoice;
            const bool measure = reportLevel || (!scanning && squelchActive);
            const qint64 captureEndMs = QDateTime::currentMSecsSinceEpoch();
            if (!scanning && current.sondeIq)
                emit sondeIqReady(QByteArray(reinterpret_cast<const char *>(samples.data()),
                                             int(samples.size() * sizeof(samples[0]))),
                                  current.frequency, shift);
            auto result = processor.process(samples, shift, measure);
            if (reportLevel) {
                emit signalLevel(result.peakDbfs);
                lastLevel = now;
            }
            if (result.dspUnderrun) {
                emit audioGap();
                if (++underruns == 20) emit statusChanged("Ostrzezenie: zbyt malo danych audio z DSP");
            }
            if (!scanning) {
                if (current.mode == Mode::CW && !result.dspUnderrun)
                    emit morseAudioReady(QByteArray(reinterpret_cast<const char *>(result.audio.data()),
                                                   int(result.audio.size() * sizeof(std::int16_t))),
                                         current.frequency);
                if (squelchActive)
                    squelch.process(result.audio, result.peakDbfs, current.squelchThresholdDbfs);
                emit audioReady(QByteArray(reinterpret_cast<const char *>(result.audio.data()),
                                           int(result.audio.size() * sizeof(std::int16_t))), captureEndMs,
                                current.frequency, int(current.mode));
                continue;
            }
            scanDetector.add({result.peakDbfs, result.backgroundDbfs, result.scanPeakDbfs,
                              result.inBandDbfs, result.wide}, current.threshold);
            if (!scanDetector.complete()) continue;
            if (scanDetector.detected() &&
                resultLog->record(frequency, modeName(current.mode), scanDetector.level(), current.threshold))
                emit detected(current.scanId, frequency, scanDetector.level(), modeName(current.mode));
            if (current.scanEnd - frequency < current.scanStep) {
                scanning = false;
                resultLog.reset();
                emit scanFinished(current.scanId);
                tune(current.frequency);
                emit statusChanged("Skanowanie zakonczone");
            } else {
                tune(frequency + current.scanStep);
            }
        }
        if (meteorCapture.isOpen()) {
            if (!meteorCapture.flush())
                throw std::runtime_error("Nie mozna zapisac pliku IQ Meteor-M");
            const QString path = meteorCapture.fileName();
            meteorCapture.close();
            emit meteorCaptureStopped(path);
        }
    } catch (const std::exception &error) {
        emit failed(QString::fromUtf8(error.what()));
    }
}
