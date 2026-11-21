#include "dsp.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

int main() {
    constexpr double pi = 3.14159265358979323846;
    std::vector<std::complex<float>> tone(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        double phase = 2 * pi * 1875 * i / kSampleRate;
        tone[i] = 0.25f * std::complex<float>(std::cos(phase), std::sin(phase));
    }
    const auto peak = spectrumPeakDbfs(tone, 300, 3000);
    if (std::abs(peak - 20 * std::log10(0.25)) > 0.1 ||
        spectrumPeakDbfs(tone, -3000, -300) > -60) {
        std::cerr << "FFT amplitude or bandwidth test failed\n";
        return 1;
    }
    std::mt19937 rng(12345);
    std::normal_distribution<float> noise(0, 0.02f);
    for (int count : {8, 9}) {
        ScanDetector detector;
        for (int block = 0; block < 15; ++block) {
            const double level = block < count ? -30 : -90;
            detector.add({level, -80, level, -100, false}, -60);
        }
        if (detector.detected() != (count == 9)) {
            std::cerr << "Scan persistence boundary failed\n";
            return 1;
        }
    }
    {
        SquelchGate gate;
        std::vector<std::int16_t> audio(512, 12000);
        if (gate.process(audio, -100, -90) ||
            std::any_of(audio.begin(), audio.end(), [](std::int16_t sample) { return sample != 0; })) {
            std::cerr << "Closed squelch passed audio below threshold\n";
            return 1;
        }
        std::fill(audio.begin(), audio.end(), 12000);
        if (!gate.process(audio, -89, -90) || audio.back() != 12000 || audio.front() == 12000) {
            std::cerr << "Squelch did not open with a fade\n";
            return 1;
        }
        std::fill(audio.begin(), audio.end(), 12000);
        if (!gate.process(audio, -92, -90) || audio.back() != 12000) {
            std::cerr << "Squelch hysteresis closed too early\n";
            return 1;
        }
        std::fill(audio.begin(), audio.end(), 12000);
        if (gate.process(audio, -94, -90) || audio.back() != 0 || audio.front() == 12000) {
            std::cerr << "Squelch did not close with a fade\n";
            return 1;
        }
    }
    for (bool wide : {false, true}) {
        for (int trial = 0; trial < 100; ++trial) {
            ScanDetector detector;
            for (int block = 0; block < 15; ++block) {
                for (auto &sample : tone) sample = {noise(rng), noise(rng)};
                detector.add(spectrumLevels(tone, wide ? -90000 : 300, wide ? 90000 : 3000), -100);
            }
            if (detector.detected()) {
                std::cerr << "Noise-only scan detection\n";
                return 1;
            }
        }
    }
    for (int signal = 0; signal < 5; ++signal) {
        ScanDetector detector;
        double fmPhase = 0;
        for (int block = 0; block < 15; ++block) {
            for (int i = 0; i < kBlock; ++i) {
                const double t = (block * kBlock + i) / double(kSampleRate);
                std::complex<float> sample{noise(rng), noise(rng)};
                const double modulation = std::sin(2 * pi * 1000 * t);
                if (signal == 0) sample += std::complex<float>(float(0.1 * (1 + 0.4 * modulation)), 0);
                if (signal == 1) sample += 0.1f * std::complex<float>(std::cos(2 * pi * 1500 * t), std::sin(2 * pi * 1500 * t));
                if (signal == 2 || signal == 3) {
                    fmPhase += 2 * pi * (signal == 2 ? 2500 : 30000) * modulation / kSampleRate;
                    sample += 0.1f * std::complex<float>(std::cos(fmPhase), std::sin(fmPhase));
                }
                if (signal == 4 && block == 0)
                    sample += 0.5f * std::complex<float>(std::cos(2 * pi * 1500 * t), std::sin(2 * pi * 1500 * t));
                tone[i] = sample;
            }
            const double low = signal == 1 || signal == 4 ? 300 : signal == 3 ? -90000 : -6000;
            const double high = signal == 1 || signal == 4 ? 3000 : signal == 3 ? 90000 : 6000;
            detector.add(spectrumLevels(tone, low, high), -100);
        }
        if (detector.detected() != (signal != 4)) {
            std::cerr << "Scan detection failed for synthetic signal " << signal << '\n';
            return 1;
        }
    }
    {
        ScanDetector detector;
        for (int block = 0; block < 15; ++block) {
            for (int i = 0; i < kBlock; ++i) {
                const double t = (block * kBlock + i) / double(kSampleRate);
                tone[i] = std::complex<float>(noise(rng), noise(rng)) +
                    0.1f * std::complex<float>(std::cos(2 * pi * 150000 * t), std::sin(2 * pi * 150000 * t));
            }
            detector.add(spectrumLevels(tone, -90000, 90000), -100);
        }
        if (detector.detected()) {
            std::cerr << "Out-of-band FM signal detected at scan frequency\n";
            return 1;
        }
    }
    WideFm fm;
    std::vector<std::complex<float>> input(kSampleRate / 2);
    double phase = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        phase += 2 * pi * 75000 * 0.4 * std::sin(2 * pi * 1000 * i / kSampleRate) / kSampleRate;
        input[i] = {float(std::cos(phase)), float(std::sin(phase))};
    }
    auto audio = fm.process(input);
    double sinSum = 0, cosSum = 0, square = 0;
    for (std::size_t i = 2000; i < audio.size(); ++i) {
        sinSum += audio[i] * std::sin(2 * pi * 1000 * i / kAudioRate);
        cosSum += audio[i] * std::cos(2 * pi * 1000 * i / kAudioRate);
        square += audio[i] * audio[i];
    }
    double count = audio.size() - 2000;
    double amplitude = 2 * std::hypot(sinSum, cosSum) / count;
    double rms = std::sqrt(square / count);
    if (audio.size() != kAudioRate / 2 || amplitude < 0.30 || amplitude > 0.42 ||
        rms < 0.2 || rms > 0.32) {
        std::cerr << "WFM demodulation test failed: amplitude=" << amplitude << " rms=" << rms << '\n';
        return 1;
    }
    Processor processor;
    std::vector<std::complex<float>> shiftedTone(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        double p = 2 * pi * (-24000 + 1875) * i / kSampleRate;
        shiftedTone[i] = 0.25f * std::complex<float>(std::cos(p), std::sin(p));
    }
    if (std::abs(processor.process(shiftedTone, 24000, true).peakDbfs - 20 * std::log10(0.25)) > 0.1) {
        std::cerr << "Frequency translation test failed\n";
        return 1;
    }
    processor.setMode(Mode::DigitalVoice);
    for (int i = 0; i < kBlock; ++i) {
        const double p = 2 * pi * (-24000 + 1500) * i / kSampleRate;
        shiftedTone[i] = {float(std::cos(p)), float(std::sin(p))};
    }
    const auto voice = processor.process(shiftedTone, 24000, false);
    if (voice.audio.size() != kBlock / 4) return 1;
    for (std::size_t i = 100; i < voice.audio.size(); i += 2) {
        if (std::abs(voice.audio[i] - 0.3 * 32767) > 100 ||
            voice.audio[i] != voice.audio[i + 1]) {
            std::cerr << "Digital voice discriminator failed\n";
            return 1;
        }
    }
    for (Mode mode : {Mode::AM, Mode::USB, Mode::LSB, Mode::NFM}) {
        processor.setMode(mode);
        sinSum = cosSum = square = 0;
        std::size_t sampleIndex = 0, used = 0;
        int underruns = 0;
        double fmPhase = 0;
        for (int block = 0; block < 360; ++block) {
            for (int i = 0; i < kBlock; ++i) {
                double t = (block * kBlock + i) / double(kSampleRate);
                double modulation = std::sin(2 * pi * 1000 * t);
                if (mode == Mode::AM) {
                    tone[i] = {float(0.15 * (1 + 0.4 * modulation)), 0};
                } else {
                    double p;
                    if (mode == Mode::NFM) {
                        fmPhase += 2 * pi * 2500 * modulation / kSampleRate;
                        p = fmPhase;
                    } else {
                        p = 2 * pi * (mode == Mode::USB ? 1000 : -1000) * t;
                    }
                    tone[i] = 0.15f * std::complex<float>(std::cos(p), std::sin(p));
                }
            }
            auto result = processor.process(tone, 0, false);
            if (result.dspUnderrun) ++underruns;
            for (std::size_t i = 0; i < result.audio.size(); i += 2, ++sampleIndex) {
                if (block < 120) continue;
                double value = result.audio[i] / 32767.0;
                sinSum += value * std::sin(2 * pi * 1000 * sampleIndex / kAudioRate);
                cosSum += value * std::cos(2 * pi * 1000 * sampleIndex / kAudioRate);
                square += value * value;
                ++used;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
        amplitude = 2 * std::hypot(sinSum, cosSum) / used;
        rms = std::sqrt(square / used);
        if (amplitude < 0.005 || rms < 0.005 || amplitude / rms < 0.5) {
            std::cerr << "Synthetic audio test failed for mode " << int(mode)
                      << ": amplitude=" << amplitude << " rms=" << rms << " underruns=" << underruns << '\n';
            return 1;
        }
    }
    std::cout << "Spectrum, frequency shift, AM, SSB, NFM and WFM tests passed\n";
}
