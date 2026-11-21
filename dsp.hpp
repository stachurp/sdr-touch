#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

constexpr int kSampleRate = 384000;
constexpr int kAudioRate = 48000;
constexpr int kBlock = 2048;

enum class Mode { LSB, USB, AM, NFM, WFM, DigitalUSB, DigitalVoice, CW };

struct SignalBlock {
    std::vector<std::int16_t> audio;
    double peakDbfs;
    double backgroundDbfs;
    double scanPeakDbfs;
    double inBandDbfs;
    bool wide;
    bool dspUnderrun;
};

struct SpectrumLevels {
    double peakDbfs;
    double backgroundDbfs;
    double scanPeakDbfs;
    double inBandDbfs;
    bool wide;
};

class ScanDetector {
public:
    void reset();
    void add(const SpectrumLevels &levels, double threshold);
    bool complete() const;
    bool detected() const;
    double level() const;
private:
    int blocks_ = 0;
    int hits_ = 0;
    double peak_ = -200;
};

class SquelchGate {
public:
    void reset();
    bool process(std::vector<std::int16_t> &audio, double levelDbfs, double thresholdDbfs);
private:
    bool open_ = false;
};

class Processor {
public:
    Processor();
    ~Processor();
    Processor(const Processor &) = delete;
    Processor &operator=(const Processor &) = delete;
    void setMode(Mode mode);
    SignalBlock process(const std::vector<std::complex<float>> &samples, double shiftHz, bool measure);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

double spectrumPeakDbfs(const std::vector<std::complex<float>> &samples, double lowHz, double highHz);
SpectrumLevels spectrumLevels(const std::vector<std::complex<float>> &samples, double lowHz, double highHz);

class WideFm {
public:
    WideFm();
    std::vector<float> process(const std::vector<std::complex<float>> &samples);
private:
    std::vector<double> taps_;
    std::vector<double> history_;
    std::complex<float> previous_{};
    bool havePrevious_ = false;
    std::size_t position_ = 0;
    int decimation_ = 0;
    double deemphasis_ = 0;
};
