#pragma once

#include <QObject>

class QLineEdit;

class TouchKeyboard : public QObject {
public:
    explicit TouchKeyboard(QObject *parent = nullptr);
    bool eventFilter(QObject *object, QEvent *event) override;
    void edit(QLineEdit *target);
private:
    bool open_ = false;
};
