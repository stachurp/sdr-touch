#include "sonde_map.hpp"
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QHBoxLayout>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int tileSize = 256;
}

SondeMap::SondeMap(bool networkEnabled, QWidget *parent)
    : QWidget(parent), network_(new QNetworkAccessManager(this)), networkEnabled_(networkEnabled) {
    setMinimumSize(260, 180);
    tileTimer_ = new QTimer(this);
    tileTimer_->setSingleShot(true);
    tileTimer_->setInterval(150);
    connect(tileTimer_, &QTimer::timeout, this, [this] { loadVisibleTiles(); });
    controls_ = new QWidget(this);
    controls_->setStyleSheet("QWidget { background: transparent; } QPushButton { background: white; color: black; "
                             "border: 1px solid #59636a; min-height: 32px; min-width: 32px; padding: 0; }");
    auto *layout = new QHBoxLayout(controls_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *out = new QPushButton("−");
    auto *in = new QPushButton("+");
    auto *fit = new QPushButton("⌂");
    out->setObjectName("mapZoomOut");
    in->setObjectName("mapZoomIn");
    fit->setObjectName("mapFit");
    out->setToolTip("Pomniejsz mapę");
    in->setToolTip("Powiększ mapę");
    fit->setToolTip("Pokaż sondę i swoją pozycję");
    layout->addWidget(out);
    layout->addWidget(in);
    layout->addWidget(fit);
    controls_->adjustSize();
    connect(out, &QPushButton::clicked, this, [this] { zoomBy(-1); });
    connect(in, &QPushButton::clicked, this, [this] { zoomBy(1); });
    connect(fit, &QPushButton::clicked, this, [this] { fitPositions(); });
    if (networkEnabled_) {
        auto *cache = new QNetworkDiskCache(network_);
        cache->setCacheDirectory(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                                    .filePath("sonde-map"));
        cache->setMaximumCacheSize(50 * 1024 * 1024);
        network_->setCache(cache);
    }
}

bool SondeMap::loaded() const {
    return !tileTimer_->isActive() && pending_.isEmpty() && !tiles_.isEmpty() && error_.isEmpty();
}

QPointF SondeMap::project(Location location, int zoom) const {
    const double scale = tileSize * double(1 << zoom);
    const double latitude = std::clamp(location.latitude, -85.05112878, 85.05112878) * pi / 180;
    return {(location.longitude + 180) / 360 * scale,
            (1 - std::asinh(std::tan(latitude)) / pi) / 2 * scale};
}

void SondeMap::geometry(int &zoom, QPointF &topLeft) const {
    const auto target = sonde_ ? sonde_ : local_;
    if (!origin_ || !target) { topLeft = {}; return; }
    if (zoom_ >= 0) {
        zoom = zoom_;
        topLeft = center_ * (tileSize * double(1 << zoom)) - QPointF(width() / 2.0, height() / 2.0);
        return;
    }
    zoom = 12;
    for (; zoom > 2; --zoom) {
        const auto a = project(*origin_, zoom), b = project(*target, zoom);
        if (std::abs(a.x() - b.x()) <= std::max(1, width() - 70) &&
            std::abs(a.y() - b.y()) <= std::max(1, height() - 70)) break;
    }
    topLeft = (project(*origin_, zoom) + project(*target, zoom)) / 2 -
              QPointF(width() / 2.0, height() / 2.0);
}

int SondeMap::zoomLevel() const {
    if (!origin_ || (!sonde_ && !local_)) return -1;
    int zoom;
    QPointF topLeft;
    geometry(zoom, topLeft);
    return zoom;
}

QPointF SondeMap::viewCenter() const {
    if (!origin_ || (!sonde_ && !local_)) return {};
    int zoom;
    QPointF topLeft;
    geometry(zoom, topLeft);
    return (topLeft + QPointF(width() / 2.0, height() / 2.0)) / (tileSize * double(1 << zoom));
}

void SondeMap::zoomBy(int steps) {
    if (!origin_ || (!sonde_ && !local_)) return;
    const auto center = viewCenter();
    zoom_ = std::clamp(zoomLevel() + steps, 2, 18);
    center_ = center;
    error_.clear();
    tileTimer_->start();
    update();
}

void SondeMap::fitPositions() {
    zoom_ = -1;
    error_.clear();
    tileTimer_->start();
    update();
}

void SondeMap::setPositions(std::optional<Location> origin, std::optional<Location> sonde, bool fit) {
    if (fit || !origin_ || (!sonde_ && !local_) || !origin || (!sonde && !local_)) zoom_ = -1;
    origin_ = origin;
    sonde_ = sonde;
    error_.clear();
    loadVisibleTiles();
    update();
}

void SondeMap::setLocalPosition(std::optional<Location> local) {
    if (!sonde_ && (!local_ || !local)) zoom_ = -1;
    local_ = local;
    loadVisibleTiles();
    update();
}

void SondeMap::loadVisibleTiles() {
    if (!networkEnabled_ || !isVisible() || !origin_ || (!sonde_ && !local_)) return;
    int zoom;
    QPointF topLeft;
    geometry(zoom, topLeft);
    const int count = 1 << zoom;
    const int x0 = int(std::floor(topLeft.x() / tileSize));
    const int y0 = int(std::floor(topLeft.y() / tileSize));
    const int x1 = int(std::floor((topLeft.x() + width() - 1) / tileSize));
    const int y1 = int(std::floor((topLeft.y() + height() - 1) / tileSize));
    for (int y = std::max(0, y0); y <= std::min(count - 1, y1); ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int wrappedX = (x % count + count) % count;
            const QString key = QString("%1/%2/%3").arg(zoom).arg(wrappedX).arg(y);
            if (tiles_.contains(key) || pending_.contains(key)) continue;
            pending_.insert(key);
            QNetworkRequest request(QUrl("https://tile.openstreetmap.org/" + key + ".png"));
            request.setRawHeader("User-Agent", "SDRTouch/1.0 (+https://github.com/stachurp/sdr-touch)");
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            auto *reply = network_->get(request);
            QTimer::singleShot(20000, reply, &QNetworkReply::abort);
            connect(reply, &QNetworkReply::finished, this, [this, reply, key] {
                pending_.remove(key);
                QImage image;
                if (reply->error() != QNetworkReply::NoError || !image.loadFromData(reply->readAll())) {
                    error_ = "Nie można pobrać mapy OpenStreetMap.";
                } else {
                    if (tiles_.size() >= 128) tiles_.clear();
                    tiles_.insert(key, image);
                }
                reply->deleteLater();
                update();
            });
        }
    }
}

void SondeMap::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#dce4e4"));
    if (!origin_ || (!sonde_ && !local_)) {
        painter.setPen(Qt::black);
        painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignCenter | Qt::TextWordWrap,
                         "Wybierz sondę, aby zobaczyć mapę.");
        return;
    }
    int zoom;
    QPointF topLeft;
    geometry(zoom, topLeft);
    const int count = 1 << zoom;
    const int x0 = int(std::floor(topLeft.x() / tileSize));
    const int y0 = int(std::floor(topLeft.y() / tileSize));
    const int x1 = int(std::floor((topLeft.x() + width() - 1) / tileSize));
    const int y1 = int(std::floor((topLeft.y() + height() - 1) / tileSize));
    for (int y = std::max(0, y0); y <= std::min(count - 1, y1); ++y)
        for (int x = x0; x <= x1; ++x) {
            const QString key = QString("%1/%2/%3").arg(zoom).arg((x % count + count) % count).arg(y);
            if (tiles_.contains(key))
                painter.drawImage(QPointF(x * tileSize, y * tileSize) - topLeft, tiles_.value(key));
        }
    const double world = tileSize * double(1 << zoom);
    auto visiblePoint = [&](Location location) {
        QPointF point = project(location, zoom);
        point.rx() += std::round((topLeft.x() + width() / 2.0 - point.x()) / world) * world;
        return point - topLeft;
    };
    const QPointF a = visiblePoint(*origin_), b = visiblePoint(sonde_ ? *sonde_ : *local_);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#263b84"), 2, Qt::DashLine));
    painter.drawLine(a, b);
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(QColor("#1766c1"));
    painter.drawEllipse(a, 7, 7);
    if (local_) {
        painter.setBrush(QColor("#148a36"));
        painter.drawEllipse(visiblePoint(*local_), 11, 11);
    }
    if (sonde_) {
        painter.setBrush(QColor("#d92727"));
        painter.drawEllipse(visiblePoint(*sonde_), 7, 7);
    }
    QFont font = painter.font();
    font.setPixelSize(11);
    painter.setFont(font);
    painter.fillRect(QRect(0, 0, width(), 20), QColor(255, 255, 255, 220));
    painter.fillRect(QRect(0, height() - 20, width(), 20), QColor(255, 255, 255, 220));
    painter.setPen(Qt::black);
    painter.drawText(QRect(4, 0, width() - 8, 20), Qt::AlignVCenter,
                     error_.isEmpty() ? "Ty: niebieski  SondeHub: czerwony  Odbiór: zielony" : error_);
    painter.drawText(QRect(4, height() - 20, width() - 8, 20), Qt::AlignVCenter,
                     "© OpenStreetMap contributors");
}

void SondeMap::resizeEvent(QResizeEvent *) {
    controls_->move(width() - controls_->width() - 6, 24);
    loadVisibleTiles();
}
void SondeMap::showEvent(QShowEvent *) { loadVisibleTiles(); }

void SondeMap::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !origin_ || (!sonde_ && !local_)) return;
    center_ = viewCenter();
    zoom_ = zoomLevel();
    lastDrag_ = event->pos();
    dragging_ = true;
    event->accept();
}

void SondeMap::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_) return;
    const QPoint delta = event->pos() - lastDrag_;
    center_ -= QPointF(delta) / (tileSize * double(1 << zoom_));
    center_.setX(center_.x() - std::floor(center_.x()));
    center_.setY(std::clamp(center_.y(), 0.0, 1.0));
    lastDrag_ = event->pos();
    error_.clear();
    tileTimer_->start();
    update();
    event->accept();
}

void SondeMap::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        event->accept();
    }
}
