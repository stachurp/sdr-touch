#pragma once

#include <QByteArray>
#include <QtEndian>

class PcmResampler {
public:
    void reset() { phase_ = 0; havePrevious_ = false; }

    QByteArray convert(const QByteArray &stereo) {
        QByteArray mono;
        mono.reserve(stereo.size() / 4);
        for (int i = 0; i < stereo.size(); i += 4) {
            const qint16 sample = qFromLittleEndian<qint16>(
                reinterpret_cast<const uchar *>(stereo.constData() + i));
            if (havePrevious_) {
                phase_ += 147;
                if (phase_ >= 320) {
                    phase_ -= 320;
                    const int value = (int(previous_) * phase_ + int(sample) * (147 - phase_)) / 147;
                    char bytes[2];
                    qToLittleEndian(qint16(value), reinterpret_cast<uchar *>(bytes));
                    mono.append(bytes, 2);
                }
            } else {
                havePrevious_ = true;
            }
            previous_ = sample;
        }
        return mono;
    }

private:
    int phase_ = 0;
    qint16 previous_ = 0;
    bool havePrevious_ = false;
};
