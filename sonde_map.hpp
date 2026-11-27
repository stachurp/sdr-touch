#pragma once

#include "geography.hpp"
#include <QHash>
#include <QImage>
#include <QSet>
#include <QWidget>
#include <optional>

class QNetworkAccessManager;
class QMouseEvent;
class QTimer;

class SondeMap : public QWidget {
public:
    explicit SondeMap(bool networkEnabled, QWidget *parent = nullptr);
    void setPositions(std::optional<Location> origin, std::optional<Location> sonde, bool fit = false);
    void setLocalPosition(std::optional<Location> local);
    std::optional<Location> localPosition() const { return local_; }
    void zoomBy(int steps);
    void fitPositions();
    int zoomLevel() const;
    QPointF viewCenter() const;
    bool loaded() const;
    QString error() const { return error_; }

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void loadVisibleTiles();
    QPointF project(Location location, int zoom) const;
    void geometry(int &zoom, QPointF &topLeft) const;

    QNetworkAccessManager *network_;
    std::optional<Location> origin_, sonde_, local_;
    QHash<QString, QImage> tiles_;
    QSet<QString> pending_;
    QString error_;
    bool networkEnabled_;
    int zoom_ = -1;
    QPointF center_;
    QPoint lastDrag_;
    bool dragging_ = false;
    QTimer *tileTimer_;
    QWidget *controls_;
};
