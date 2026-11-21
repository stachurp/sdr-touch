#include "dsp.hpp"
#include <fftw3.h>
#include <wdsp.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
constexpr double pi = 3.14159265358979323846;

std::pair<double, double> passband(Mode mode) {
    switch (mode) {
    case Mode::LSB: return {-3000, -300};
    case Mode::USB: return {300, 3000};
    case Mode::DigitalUSB: return {200, 4000};
    case Mode::DigitalVoice: return {-7500, 7500};
    case Mode::CW: return {400, 1000};
    case Mode::AM: return {-5000, 5000};
    case Mode::NFM: return {-6000, 6000};
    case Mode::WFM: return {-90000, 90000};
    }
    throw std::logic_error("Unknown modulation");
}

std::int16_t pcm(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("DSP returned a non-finite audio sample");
    value = std::clamp(value, -1.0, 1.0);
    return static_cast<std::int16_t>(std::lround(value * 32767));
}
}

SpectrumLevels spectrumLevels(const std::vector<std::complex<float>> &samples, double lowHz, double highHz) {
    if (samples.size() != kBlock || !std::isfinite(lowHz) || !std::isfinite(highHz) ||
        lowHz < -kSampleRate / 2 || highHz > kSampleRate / 2 || lowHz >= highHz)
        throw std::invalid_argument("Invalid spectrum input");
    auto *input = fftwf_alloc_complex(kBlock);
    auto *output = fftwf_alloc_complex(kBlock);
    if (!input || !output) {
        fftwf_free(input);
        fftwf_free(output);
        throw std::bad_alloc();
    }
    auto plan = fftwf_plan_dft_1d(kBlock, input, output, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan) {
        fftwf_free(input);
        fftwf_free(output);
        throw std::runtime_error("Cannot initialize FFT");
    }
    double windowSum = 0;
    for (int i = 0; i < kBlock; ++i) {
        const double window = 0.5 - 0.5 * std::cos(2 * pi * i / (kBlock - 1));
        windowSum += window;
        input[i][0] = samples[i].real() * window;
        input[i][1] = samples[i].imag() * window;
    }
    fftwf_execute(plan);
    double peak = 0, scanPeak = 0;
    const bool wide = highHz - lowHz > 100000;
    std::vector<double> background;
    std::vector<double> inBand;
    for (int i = 0; i < kBlock; ++i) {
        int bin = i <= kBlock / 2 ? i : i - kBlock;
        double frequency = bin * double(kSampleRate) / kBlock;
        double power = double(output[i][0]) * output[i][0] + double(output[i][1]) * output[i][1];
        if (frequency >= lowHz && frequency <= highHz) {
            peak = std::max(peak, power);
            if (!wide || (std::abs(frequency) >= 600 && std::abs(frequency) <= 20000))
                scanPeak = std::max(scanPeak, power);
        }
        if (wide && std::abs(frequency) <= 80000) inBand.push_back(power);
        if ((wide && std::abs(frequency) >= 120000 && std::abs(frequency) <= 160000) ||
            (!wide && std::abs(frequency) >= 10000 && std::abs(frequency) <= 85000 &&
             (frequency < lowHz || frequency > highHz)))
            background.push_back(power);
    }
    fftwf_destroy_plan(plan);
    fftwf_free(input);
    fftwf_free(output);
    if (background.empty()) throw std::runtime_error("Brak pasma do pomiaru tla");
    const auto middle = background.begin() + background.size() / 2;
    std::nth_element(background.begin(), middle, background.end());
    const double scale = windowSum * windowSum;
    double inBandPower = 0;
    if (wide) {
        const auto bandMiddle = inBand.begin() + inBand.size() / 2;
        std::nth_element(inBand.begin(), bandMiddle, inBand.end());
        inBandPower = *bandMiddle;
    }
    return {10 * std::log10(std::max(peak / scale, 1e-20)),
            10 * std::log10(std::max(*middle / scale, 1e-20)),
            10 * std::log10(std::max(scanPeak / scale, 1e-20)),
            10 * std::log10(std::max(inBandPower / scale, 1e-20)), wide};
}

double spectrumPeakDbfs(const std::vector<std::complex<float>> &samples, double lowHz, double highHz) {
    return spectrumLevels(samples, lowHz, highHz).peakDbfs;
}

void ScanDetector::reset() {
    blocks_ = hits_ = 0;
    peak_ = -200;
}

void ScanDetector::add(const SpectrumLevels &levels, double threshold) {
    if (complete() || !std::isfinite(levels.peakDbfs) || !std::isfinite(levels.scanPeakDbfs) ||
        !std::isfinite(levels.inBandDbfs) ||
        !std::isfinite(levels.backgroundDbfs) || !std::isfinite(threshold))
        throw std::invalid_argument("Invalid scan measurement");
    ++blocks_;
    const bool aboveBackground = levels.wide ?
        (levels.inBandDbfs >= levels.backgroundDbfs + 2 ||
         levels.scanPeakDbfs >= levels.backgroundDbfs + 20) :
        levels.scanPeakDbfs >= levels.backgroundDbfs + 12;
    if (levels.peakDbfs >= threshold && aboveBackground) {
        ++hits_;
        peak_ = std::max(peak_, levels.peakDbfs);
    }
}

bool ScanDetector::complete() const { return blocks_ == 15; }
bool ScanDetector::detected() const { return complete() && hits_ >= 9; }
double ScanDetector::level() const {
    if (!detected()) throw std::logic_error("No detected scan signal");
    return peak_;
}

void SquelchGate::reset() { open_ = false; }

bool SquelchGate::process(std::vector<std::int16_t> &audio, double levelDbfs, double thresholdDbfs) {
    if (!std::isfinite(levelDbfs) || !std::isfinite(thresholdDbfs))
        throw std::invalid_argument("Invalid squelch level");
    const bool wasOpen = open_;
    if (open_) {
        if (levelDbfs < thresholdDbfs - 3.0) open_ = false;
    } else if (levelDbfs >= thresholdDbfs) {
        open_ = true;
    }

    if (open_ == wasOpen) {
        if (!open_) std::fill(audio.begin(), audio.end(), 0);
        return open_;
    }

    const std::size_t frames = (audio.size() + 1) / 2;
    const std::size_t fadeFrames = std::min<std::size_t>(64, frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double gain = open_ ?
            (frame < fadeFrames ? double(frame + 1) / fadeFrames : 1.0) :
            (frame < fadeFrames ? double(fadeFrames - frame - 1) / fadeFrames : 0.0);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const std::size_t index = frame * 2 + channel;
            if (index >= audio.size()) break;
            audio[index] = static_cast<std::int16_t>(std::lround(audio[index] * gain));
        }
    }
    return open_;
}

WideFm::WideFm() : taps_(161), history_(taps_.size(), 0) {
    constexpr double cutoff = 15000.0 / kSampleRate;
    const int center = int(taps_.size() / 2);
    double sum = 0;
    for (int i = 0; i < int(taps_.size()); ++i) {
        int x = i - center;
        const double sinc = x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
        taps_[i] = sinc * (0.5 - 0.5 * std::cos(2 * pi * i / (taps_.size() - 1)));
        sum += taps_[i];
    }
    for (auto &tap : taps_) tap /= sum;
}

std::vector<float> WideFm::process(const std::vector<std::complex<float>> &samples) {
    std::vector<float> result;
    result.reserve(samples.size() / (kSampleRate / kAudioRate) + 1);
    constexpr int decimation = kSampleRate / kAudioRate;
    const double alpha = 1 - std::exp(-1.0 / (kAudioRate * 50e-6));
    for (const auto sample : samples) {
        double discriminator = 0;
        if (havePrevious_) {
            auto product = sample * std::conj(previous_);
            discriminator = std::atan2(product.imag(), product.real()) * kSampleRate / (2 * pi * 75000.0);
        }
        previous_ = sample;
        havePrevious_ = true;
        history_[position_] = discriminator;
        position_ = (position_ + 1) % history_.size();
        if (++decimation_ == decimation) {
            decimation_ = 0;
            double value = 0;
            for (std::size_t i = 0; i < taps_.size(); ++i)
                value += taps_[i] * history_[(position_ + history_.size() - 1 - i) % history_.size()];
            deemphasis_ += alpha * (value - deemphasis_);
            result.push_back(static_cast<float>(deemphasis_));
        }
    }
    return result;
}

class VoiceFm {
public:
    VoiceFm() : taps_(321), history_(taps_.size()) {
        constexpr double cutoff = 7500.0 / kSampleRate;
        const int center = int(taps_.size() / 2);
        double sum = 0;
        for (int i = 0; i < int(taps_.size()); ++i) {
            const int x = i - center;
            const double sinc = x == 0 ? 2 * cutoff : std::sin(2 * pi * cutoff * x) / (pi * x);
            taps_[i] = sinc * (0.5 - 0.5 * std::cos(2 * pi * i / (taps_.size() - 1)));
            sum += taps_[i];
        }
        for (auto &tap : taps_) tap /= sum;
    }

    std::vector<std::int16_t> process(const std::vector<std::complex<float>> &samples) {
        std::vector<std::int16_t> output;
        output.reserve(samples.size() / 4);
        for (const auto sample : samples) {
            history_[position_] = sample;
            position_ = (position_ + 1) % history_.size();
            if (++decimation_ != kSampleRate / kAudioRate) continue;
            decimation_ = 0;
            std::complex<double> filtered{};
            for (std::size_t i = 0; i < taps_.size(); ++i)
                filtered += std::complex<double>(history_[(position_ + history_.size() - 1 - i) % history_.size()]) * taps_[i];
            double deviation = 0;
            if (havePrevious_) {
                const auto product = filtered * std::conj(previous_);
                deviation = std::atan2(product.imag(), product.real());
            }
            previous_ = filtered;
            havePrevious_ = true;
            const auto value = pcm(deviation * (0.6 * kAudioRate / (2 * pi * 3000)));
            output.push_back(value);
            output.push_back(value);
        }
        return output;
    }
private:
    std::vector<double> taps_;
    std::vector<std::complex<float>> history_;
    std::complex<double> previous_{};
    std::size_t position_ = 0;
    int decimation_ = 0;
    bool havePrevious_ = false;
};

struct Processor::Impl {
    Mode mode = Mode::AM;
    std::vector<double> input = std::vector<double>(2 * kBlock);
    std::vector<double> output = std::vector<double>(2 * kBlock / 8);
    double phase = 0;
    WideFm wideFm;
    VoiceFm voiceFm;
    Impl() {
        OpenChannel(0, kBlock, 2048, kSampleRate, kAudioRate, kAudioRate, 0, 1,
                    0.01, 0.01, 0.01, 0.01, 0);
        SetRXAAGCMode(0, 2);
        SetRXABandpassRun(0, 1);
    }
    ~Impl() { CloseChannel(0); }
};

Processor::Processor() : impl_(std::make_unique<Impl>()) { setMode(Mode::AM); }
Processor::~Processor() = default;

void Processor::setMode(Mode mode) {
    impl_->mode = mode;
    int wdspMode = 6;
    double low = -5000, high = 5000;
    switch (mode) {
    case Mode::LSB: wdspMode = 0; low = -3000; high = -300; break;
    case Mode::USB: wdspMode = 1; low = 300; high = 3000; break;
    case Mode::DigitalUSB: wdspMode = 1; low = 200; high = 4000; break;
    case Mode::NFM: wdspMode = 5; low = -6000; high = 6000; break;
    case Mode::DigitalVoice: break;
    case Mode::CW: wdspMode = 1; low = 400; high = 1000; break;
    case Mode::AM:
    case Mode::WFM: break;
    }
    SetRXAMode(0, wdspMode);
    RXASetPassband(0, low, high);
    if (mode == Mode::NFM) SetRXAFMDeviation(0, 5000);
    if (mode == Mode::WFM) impl_->wideFm = WideFm();
    if (mode == Mode::DigitalVoice) impl_->voiceFm = VoiceFm();
}

SignalBlock Processor::process(const std::vector<std::complex<float>> &samples, double shiftHz, bool measure) {
    if (samples.size() != kBlock || !std::isfinite(shiftHz))
        throw std::invalid_argument("Invalid IQ block");
    std::vector<std::complex<float>> shifted(kBlock);
    const double step = 2 * pi * (shiftHz + (impl_->mode == Mode::CW ? 700 : 0)) / kSampleRate;
    for (int i = 0; i < kBlock; ++i) {
        const std::complex<float> oscillator(float(std::cos(impl_->phase)), float(std::sin(impl_->phase)));
        shifted[i] = samples[i] * oscillator;
        impl_->phase += step;
        if (impl_->phase > pi) impl_->phase -= 2 * pi;
        if (impl_->phase < -pi) impl_->phase += 2 * pi;
    }
    const auto limits = passband(impl_->mode);
    const auto levels = measure ? spectrumLevels(shifted, limits.first, limits.second) :
                                  SpectrumLevels{0, 0, 0, 0, false};
    SignalBlock result{{}, levels.peakDbfs, levels.backgroundDbfs, levels.scanPeakDbfs,
                       levels.inBandDbfs, levels.wide, false};
    result.audio.reserve(2 * kBlock / 8);
    if (impl_->mode == Mode::DigitalVoice) {
        result.audio = impl_->voiceFm.process(shifted);
    } else if (impl_->mode == Mode::WFM) {
        for (auto sample : impl_->wideFm.process(shifted)) {
            const auto value = pcm(sample * 0.8);
            result.audio.push_back(value);
            result.audio.push_back(value);
        }
    } else {
        for (int i = 0; i < kBlock; ++i) {
            impl_->input[2 * i] = shifted[i].real();
            // WDSP's positive passbands select negative-frequency complex input.
            impl_->input[2 * i + 1] = -shifted[i].imag();
        }
        int error = 0;
        fexchange0(0, impl_->input.data(), impl_->output.data(), &error);
        if (error != 0 && error != -2) throw std::runtime_error("WDSP exchange failed: " + std::to_string(error));
        result.dspUnderrun = error == -2;
        for (std::size_t i = 0; i < impl_->output.size(); i += 2) {
            const auto value = pcm(impl_->output[i] * 0.04);
            result.audio.push_back(value);
            result.audio.push_back(value);
        }
    }
    return result;
}
