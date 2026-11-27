#include "spectrum_widget.hpp"
#include "dsp.hpp"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
constexpr int spanHz = 200000;
constexpr int columns = 512;
constexpr int historyLines = 200;
constexpr float floorDb = -130;
constexpr float ceilingDb = 0;

QColor waterfallColor(float db) {
    const float t = std::clamp((db - floorDb) / (ceilingDb - floorDb), 0.0f, 1.0f);
    return QColor::fromRgbF(std::clamp(3.0f * t - 1.3f, 0.0f, 1.0f),
                            std::clamp(3.0f * t - 0.6f, 0.0f, 1.0f),
                            std::clamp(3.0f * t, 0.0f, 1.0f));
}
}

SpectrumWidget::SpectrumWidget(QWidget *parent) : QWidget(parent),
    waterfall_(columns, historyLines, QImage::Format_RGB32),
    message_("Oczekiwanie na próbki odbiornika") {
    setMinimumHeight(200);
    waterfall_.fill(Qt::black);
}

void SpectrumWidget::clear(const QString &message) {
    levels_.clear();
    center_ = tunedFrequency_ = 0;
    waterfall_.fill(Qt::black);
    message_ = message;
    update();
}

void SpectrumWidget::setFrame(const QVector<float> &levels, qint64 center, qint64 tunedFrequency) {
    if (levels.size() != kBlock || center <= 0 ||
        std::any_of(levels.cbegin(), levels.cend(), [](float level) { return !std::isfinite(level); })) {
        clear("Nieprawidłowe dane widma");
        return;
    }
    if (center_ != center || tunedFrequency_ != tunedFrequency) waterfall_.fill(Qt::black);
    center_ = center;
    tunedFrequency_ = tunedFrequency;
    levels_.resize(columns);
    for (int x = 0; x < columns; ++x) {
        const int first = int((0.5 - spanHz / (2.0 * kSampleRate) +
                               x * double(spanHz) / (columns * kSampleRate)) * kBlock);
        const int last = int((0.5 - spanHz / (2.0 * kSampleRate) +
                              (x + 1) * double(spanHz) / (columns * kSampleRate)) * kBlock);
        levels_[x] = *std::max_element(levels.cbegin() + first, levels.cbegin() + std::max(first + 1, last));
    }
    for (int y = historyLines - 1; y > 0; --y)
        std::copy_n(reinterpret_cast<const QRgb *>(waterfall_.constScanLine(y - 1)), columns,
                    reinterpret_cast<QRgb *>(waterfall_.scanLine(y)));
    auto *line = reinterpret_cast<QRgb *>(waterfall_.scanLine(0));
    for (int x = 0; x < columns; ++x) line[x] = waterfallColor(levels_[x]).rgb();
    message_.clear();
    update();
}

void SpectrumWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#10161b"));
    if (levels_.isEmpty()) {
        painter.setPen(QColor("#b8c5ce"));
        painter.drawText(rect(), Qt::AlignCenter, message_);
        return;
    }
    const int graphHeight = height() / 2;
    const int axisHeight = 24;
    const QRect graph(0, 0, width(), graphHeight - axisHeight);
    const QRect waterfall(0, graphHeight, width(), height() - graphHeight);
    painter.drawImage(waterfall, waterfall_);
    painter.setPen(QColor("#37434d"));
    for (int i = 0; i <= 4; ++i) {
        const int x = i * (width() - 1) / 4;
        painter.drawLine(x, 0, x, graph.bottom());
        painter.drawLine(x, waterfall.top(), x, waterfall.bottom());
    }
    for (int i = 1; i < 4; ++i) {
        const int y = i * graph.height() / 4;
        painter.drawLine(0, y, width(), y);
    }
    painter.setPen(QColor("#9aadb8"));
    for (int i = 1; i < 4; ++i) {
        const int y = i * graph.height() / 4;
        painter.drawText(3, y - 2, QString::number(int(ceilingDb - i * (ceilingDb - floorDb) / 4)) + " dBFS");
    }
    QPolygonF trace;
    trace.reserve(columns);
    for (int i = 0; i < columns; ++i) {
        const double value = std::clamp(double(levels_[i]), double(floorDb), double(ceilingDb));
        trace.append(QPointF(i * double(width() - 1) / (columns - 1),
                             graph.top() + (ceilingDb - value) * graph.height() / (ceilingDb - floorDb)));
    }
    painter.setPen(QPen(QColor("#59d6e8"), 1));
    painter.drawPolyline(trace);
    const int marker = int((tunedFrequency_ - center_ + spanHz / 2.0) * (width() - 1) / spanHz);
    if (marker >= 0 && marker < width()) {
        painter.setPen(QPen(QColor("#f5d166"), 1));
        painter.drawLine(marker, 0, marker, height());
    }
    painter.setPen(QColor("#c0cbd2"));
    for (int i = 0; i <= 4; ++i) {
        const double mhz = (center_ - spanHz / 2.0 + i * spanHz / 4.0) / 1000000.0;
        const QString label = QString::number(mhz, 'f', 3);
        const int x = i * (width() - 1) / 4;
        const int labelWidth = painter.fontMetrics().horizontalAdvance(label);
        painter.drawText(std::clamp(x - labelWidth / 2, 0, std::max(0, width() - labelWidth)),
                         graphHeight - 5, label);
    }
}

void SpectrumWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || levels_.isEmpty() || width() < 2) return;
    const double position = std::clamp(event->localPos().x(), 0.0, double(width() - 1));
    const qint64 frequency = center_ - spanHz / 2 +
        std::llround(position * spanHz / (width() - 1));
    if (frequency >= 10000 && frequency <= 2000000000) emit frequencySelected(frequency);
}
