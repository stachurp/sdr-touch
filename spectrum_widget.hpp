#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

class SpectrumWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumWidget(QWidget *parent = nullptr);
    void setFrame(const QVector<float> &levels, qint64 center, qint64 tunedFrequency);
    void clear(const QString &message);

signals:
    void frequencySelected(qint64 frequency);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QVector<float> levels_;
    QImage waterfall_;
    qint64 center_ = 0;
    qint64 tunedFrequency_ = 0;
    QString message_;
};
