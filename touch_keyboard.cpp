#include "touch_keyboard.hpp"
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QEvent>
#include <QFocusEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
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
    auto *intSpin = qobject_cast<QSpinBox *>(target->parentWidget());
    QPointer<QSpinBox> guardedIntSpin(intSpin);
    const auto *decimalValidator = qobject_cast<const QDoubleValidator *>(target->validator());
    const auto *intValidator = qobject_cast<const QIntValidator *>(target->validator());
    const bool numeric = spin || intSpin || decimalValidator || intValidator;
    const bool decimal = spin ? spin->decimals() > 0 : decimalValidator && decimalValidator->decimals() != 0;
    const bool scientific = !spin && decimalValidator && decimalValidator->notation() == QDoubleValidator::ScientificNotation;
    const bool negative = spin ? spin->minimum() < 0 : intSpin ? intSpin->minimum() < 0 :
        decimalValidator ? decimalValidator->bottom() < 0 || scientific : intValidator && intValidator->bottom() < 0;
    const QLocale numericLocale = spin ? spin->locale() : intSpin ? intSpin->locale() :
        decimalValidator ? decimalValidator->locale() : intValidator ? intValidator->locale() : target->locale();
    QDialog dialog(target->window());
    dialog.setWindowTitle("Klawiatura ekranowa");
    dialog.setObjectName("touchKeyboard");
    dialog.resize(numeric ? 500 : 940, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *input = new QLineEdit;
    input->setObjectName("keyboardInput");
    input->setProperty("keyboardInput", true);
    input->setMinimumHeight(40);
    if (spin) {
        input->setText(spin->locale().toString(spin->value(), 'f', spin->decimals()));
    } else if (intSpin) {
        input->setText(intSpin->locale().toString(intSpin->value()));
    } else {
        input->setMaxLength(target->maxLength());
        input->setText(target->text());
    }
    input->setCursorPosition(input->text().size());
    layout->addWidget(input);
    auto *error = new QLabel;
    error->setObjectName("keyboardError");
    QObject::connect(input, &QLineEdit::textChanged, error, &QLabel::clear);
    layout->addWidget(error);
    QList<QPushButton *> characterKeys;
    if (numeric) {
        auto *keys = new QGridLayout;
        const QString digits = "7894561230";
        for (int i = 0; i < digits.size(); ++i) {
            auto *key = new QPushButton(QString(digits.at(i)));
            key->setMinimumHeight(52);
            keys->addWidget(key, i / 3, i % 3);
            QObject::connect(key, &QPushButton::clicked, input, [input, key] { input->insert(key->text()); });
        }
        int column = 1;
        if (decimal) {
            auto *key = new QPushButton(QString(numericLocale.decimalPoint()));
            key->setMinimumHeight(52);
            keys->addWidget(key, 3, column++);
            QObject::connect(key, &QPushButton::clicked, input, [input, key] { input->insert(key->text()); });
        }
        if (negative) {
            auto *key = new QPushButton("-");
            key->setMinimumHeight(52);
            keys->addWidget(key, 3, column++);
            QObject::connect(key, &QPushButton::clicked, input, [input, key] { input->insert(key->text()); });
        }
        if (scientific) {
            auto *key = new QPushButton("E");
            key->setMinimumHeight(52);
            keys->addWidget(key, 3, column);
            QObject::connect(key, &QPushButton::clicked, input, [input, key] { input->insert(key->text()); });
        }
        layout->addLayout(keys);
    } else {
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
            QObject::connect(key, &QPushButton::clicked, input, [input, key] { input->insert(key->text()); });
        }
        layout->addLayout(extras);
    }
    auto *controls = new QHBoxLayout;
    auto *space = numeric ? nullptr : new QPushButton("Spacja");
    auto *shift = numeric ? nullptr : new QPushButton("Małe litery");
    if (shift) shift->setCheckable(true);
    auto *clear = new QPushButton("Wyczyść");
    auto *back = new QPushButton(numeric ? "Usuń znak" : "⌫");
    auto *cancel = new QPushButton("Anuluj");
    auto *accept = new QPushButton("Gotowe");
    accept->setObjectName("keyboardAccept");
    const QList<QPushButton *> buttons = numeric ? QList<QPushButton *>{clear, back, cancel, accept} :
        QList<QPushButton *>{shift, space, clear, back, cancel, accept};
    for (auto *button : buttons) {
        button->setMinimumHeight(40);
        controls->addWidget(button);
    }
    layout->addLayout(controls);
    if (!numeric) {
        QObject::connect(shift, &QPushButton::toggled, &dialog, [shift, characterKeys](bool lower) {
            shift->setText(lower ? "WIELKIE" : "Małe litery");
            for (auto *key : characterKeys) key->setText(lower ? key->text().toLower() : key->text().toUpper());
        });
        QObject::connect(space, &QPushButton::clicked, input, [input] { input->insert(" "); });
    }
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
        } else if (guardedIntSpin) {
            bool ok = false;
            const int value = guardedIntSpin->locale().toInt(input->text().trimmed(), &ok);
            if (!ok || value < guardedIntSpin->minimum() || value > guardedIntSpin->maximum()) {
                error->setText("Podaj liczbę w dozwolonym zakresie.");
                return;
            }
            guardedIntSpin->setValue(value);
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
