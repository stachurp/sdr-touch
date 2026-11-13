#include "touch_keyboard.hpp"
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QValidator>
#include <QVBoxLayout>
#include <cmath>

TouchKeyboard::TouchKeyboard(QObject *parent) : QObject(parent) {}

bool TouchKeyboard::eventFilter(QObject *object, QEvent *event) {
    auto *line = qobject_cast<QLineEdit *>(object);
    if (!line) return false;
    if (event->type() == QEvent::Polish) line->setAttribute(Qt::WA_InputMethodEnabled, false);
    if (event->type() == QEvent::FocusIn && !line->isReadOnly() &&
        !line->property("keyboardInput").toBool() && !open_) {
        const auto reason = static_cast<QFocusEvent *>(event)->reason();
        const bool newDialog = qobject_cast<QInputDialog *>(line->window()) &&
                               !line->property("autoKeyboardOpened").toBool();
        if (newDialog || reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason) {
            QPointer<QLineEdit> guarded(line);
            QTimer::singleShot(0, this, [this, guarded, newDialog] {
                if (guarded && guarded->hasFocus() && !open_) {
                    if (newDialog) guarded->setProperty("autoKeyboardOpened", true);
                    edit(guarded);
                }
            });
        }
    }
    if (event->type() == QEvent::MouseButtonRelease && line->isEnabled() && !line->isReadOnly() &&
        !line->property("keyboardInput").toBool() && !open_) {
        edit(line);
    }
    return false;
}

void TouchKeyboard::edit(QLineEdit *target) {
    if (!target || open_) return;
    open_ = true;
    QPointer<QLineEdit> guardedTarget(target);
    auto *spin = qobject_cast<QDoubleSpinBox *>(target->parentWidget());
    QPointer<QDoubleSpinBox> guardedSpin(spin);
    QDialog dialog(target->window());
    dialog.setWindowTitle("Klawiatura ekranowa");
    dialog.setObjectName("touchKeyboard");
    dialog.resize(940, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *input = new QLineEdit;
    input->setObjectName("keyboardInput");
    input->setProperty("keyboardInput", true);
    input->setMinimumHeight(40);
    if (spin) {
        input->setText(spin->locale().toString(spin->value(), 'f', spin->decimals()));
    } else {
        input->setMaxLength(target->maxLength());
        input->setText(target->text());
    }
    input->setCursorPosition(input->text().size());
    layout->addWidget(input);
    auto *error = new QLabel;
    layout->addWidget(error);
    QList<QPushButton *> characterKeys;
    const QStringList rows = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    for (const auto &row : rows) {
        auto *keys = new QHBoxLayout;
        for (const QChar ch : row) {
            auto *key = new QPushButton(QString(ch));
            key->setMinimumHeight(40);
            key->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            keys->addWidget(key);
            characterKeys.append(key);
            QObject::connect(key, &QPushButton::clicked, input, [input, key] { input->insert(key->text()); });
        }
        layout->addLayout(keys);
    }
    auto *extras = new QHBoxLayout;
    const QStringList characters = {"Ą", "Ć", "Ę", "Ł", "Ń", "Ó", "Ś", "Ź", "Ż", "-", ".", "/"};
    for (const QString &character : characters) {
        auto *key = new QPushButton(character);
        key->setMinimumHeight(40);
        key->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        extras->addWidget(key);
        characterKeys.append(key);
        QObject::connect(key, &QPushButton::clicked, input, [input, key, guardedSpin] {
            const QString character = key->text();
            input->insert(character == "." && guardedSpin ? QString(guardedSpin->locale().decimalPoint()) : character);
        });
    }
    layout->addLayout(extras);
    auto *controls = new QHBoxLayout;
    auto *space = new QPushButton("Spacja");
    auto *shift = new QPushButton("Małe litery");
    shift->setCheckable(true);
    auto *clear = new QPushButton("Wyczyść");
    auto *back = new QPushButton("⌫");
    auto *cancel = new QPushButton("Anuluj");
    auto *accept = new QPushButton("Gotowe");
    accept->setObjectName("keyboardAccept");
    for (auto *button : {shift, space, clear, back, cancel, accept}) {
        button->setMinimumHeight(40);
        controls->addWidget(button);
    }
    layout->addLayout(controls);
    QObject::connect(shift, &QPushButton::toggled, &dialog, [shift, characterKeys](bool lower) {
        shift->setText(lower ? "WIELKIE" : "Małe litery");
        for (auto *key : characterKeys) key->setText(lower ? key->text().toLower() : key->text().toUpper());
    });
    QObject::connect(space, &QPushButton::clicked, input, [input] { input->insert(" "); });
    QObject::connect(clear, &QPushButton::clicked, input, &QLineEdit::clear);
    QObject::connect(back, &QPushButton::clicked, input, &QLineEdit::backspace);
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto commit = [&] {
        if (!guardedTarget) { dialog.reject(); return; }
        if (guardedSpin) {
            bool ok = false;
            const double value = guardedSpin->locale().toDouble(input->text().trimmed(), &ok);
            if (!ok || !std::isfinite(value) || value < guardedSpin->minimum() || value > guardedSpin->maximum()) {
                error->setText("Podaj liczbę w dozwolonym zakresie.");
                return;
            }
            guardedSpin->setValue(value);
        } else {
            if (guardedTarget->validator()) {
                QString value = input->text();
                int position = value.size();
                if (guardedTarget->validator()->validate(value, position) != QValidator::Acceptable) {
                    error->setText("Nieprawidłowa wartość.");
                    return;
                }
            }
            guardedTarget->setText(input->text());
        }
        dialog.accept();
    };
    QObject::connect(accept, &QPushButton::clicked, &dialog, commit);
    QObject::connect(input, &QLineEdit::returnPressed, &dialog, commit);
    input->setFocus();
    dialog.exec();
    open_ = false;
}
