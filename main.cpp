#include "receiver.hpp"
#include "digital.hpp"
#include "geography.hpp"
#include "station_lists.hpp"
#include "touch_keyboard.hpp"
#include "adsb.hpp"
#include "voice.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDoubleValidator>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSettings>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
constexpr int voiceDmr = 100;
constexpr int voiceYsf = 101;
QString dataFile() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("sygnaly.csv");
}

QDoubleSpinBox *frequencyBox(double initial) {
    auto *box = new QDoubleSpinBox;
    box->setRange(0.010000, 2000.000000);
    box->setDecimals(6);
    box->setSingleStep(0.001);
    box->setSuffix(" MHz");
    box->setValue(initial);
    box->setKeyboardTracking(false);
    return box;
}

QComboBox *modeBox() {
    auto *box = new QComboBox;
    box->addItem("LSB", int(Mode::LSB));
    box->addItem("USB", int(Mode::USB));
    box->addItem("AM", int(Mode::AM));
    box->addItem("NFM", int(Mode::NFM));
    box->addItem("WFM mono", int(Mode::WFM));
    return box;
}

QString storedMode(Mode mode) {
    switch (mode) {
    case Mode::LSB: return "LSB";
    case Mode::USB: return "USB";
    case Mode::AM: return "AM";
    case Mode::NFM: return "NFM";
    case Mode::WFM: return "WFM";
    case Mode::DigitalUSB: return "USB";
    case Mode::DigitalVoice: return "NFM";
    }
    throw std::invalid_argument("Nieznana modulacja");
}

class NumericItem : public QTableWidgetItem {
public:
    explicit NumericItem(const QString &text) : QTableWidgetItem(text) {}
    bool operator<(const QTableWidgetItem &other) const override {
        const auto left = data(Qt::UserRole), right = other.data(Qt::UserRole);
        const bool descending = tableWidget() &&
            tableWidget()->horizontalHeader()->sortIndicatorOrder() == Qt::DescendingOrder;
        const double missing = descending ? -std::numeric_limits<double>::infinity() :
                                            std::numeric_limits<double>::infinity();
        const double a = left.isValid() ? left.toDouble() : missing;
        const double b = right.isValid() ? right.toDouble() : missing;
        return a < b;
    }
};

class Window : public QWidget {
public:
    explicit Window(bool test = false, bool mute = false, QString adsbExecutable = {}, QString voiceExecutable = {})
        : adsbExecutable_(std::move(adsbExecutable)) {
        setWindowTitle("SDR Touch – RSP1");
        resize(1024, 600);
        setStyleSheet("QPushButton { min-height: 36px; } "
                      "QDoubleSpinBox, QComboBox { min-height: 34px; } "
                      "QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 30px; } "
                      "QComboBox::drop-down { width: 30px; } "
                      "QSlider::handle:horizontal { width: 22px; margin: -7px 0; }");
        auto *root = new QVBoxLayout(this);
        pages_ = new QTabWidget;
        auto *home = new QWidget;
        auto *homeLayout = new QVBoxLayout(home);
        auto *scanPage = new QWidget;
        auto *scanLayout = new QVBoxLayout(scanPage);
        auto *adsbPage = new QWidget;
        auto *adsbLayout = new QVBoxLayout(adsbPage);
        pages_->addTab(home, "Odbiornik");
        pages_->addTab(scanPage, "Skanowanie i listy");
        pages_->addTab(adsbPage, "Samoloty ADS-B");
        root->addWidget(pages_, 1);
        auto *grid = new QGridLayout;
        frequency_ = frequencyBox(7.1);
        mode_ = modeBox();
        volume_ = new QSlider(Qt::Horizontal);
        volume_->setRange(0, 100);
        volume_->setValue(mute ? 0 : 35);
        mute_ = new QPushButton("Wycisz");
        mute_->setCheckable(true);
        signal_ = new QLabel("Sygnał: -- dBFS");
        meter_ = new QProgressBar;
        meter_->setRange(-140, 0);
        meter_->setValue(-140);
        meter_->setTextVisible(false);
        meter_->setToolTip("Szczyt FFT w paśmie, przybliżony poziom dBFS, nie pomiar mocy stacji");
        grid->addWidget(new QLabel("Częstotliwość"), 0, 0);
        grid->addWidget(frequency_, 0, 1);
        grid->addWidget(new QLabel("Tryb"), 0, 2);
        grid->addWidget(mode_, 0, 3);
        grid->addWidget(new QLabel("Głośność"), 0, 4);
        grid->addWidget(volume_, 0, 5);
        grid->addWidget(mute_, 0, 6);
        grid->addWidget(signal_, 1, 0, 1, 2);
        grid->addWidget(meter_, 1, 2, 1, 5);
        homeLayout->addLayout(grid);
        auto *stationControls = new QHBoxLayout;
        mainList_ = new QComboBox;
        mainList_->setPlaceholderText("Utwórz listę w skanerze");
        stationChoice_ = new QComboBox;
        stationChoice_->setMinimumWidth(220);
        previousStation_ = new QPushButton("◀");
        nextStation_ = new QPushButton("▶");
        stationControls->addWidget(new QLabel("Lista"));
        stationControls->addWidget(mainList_, 1);
        stationControls->addWidget(previousStation_);
        stationControls->addWidget(stationChoice_, 2);
        stationControls->addWidget(nextStation_);
        homeLayout->addLayout(stationControls);
        auto *scanGrid = new QGridLayout;
        start_ = frequencyBox(7.0);
        end_ = frequencyBox(7.2);
        step_ = new QDoubleSpinBox;
        step_->setRange(0.001, 2000000);
        step_->setDecimals(3);
        step_->setValue(1);
        step_->setSuffix(" kHz");
        step_->setKeyboardTracking(false);
        threshold_ = new QDoubleSpinBox;
        threshold_->setRange(-140, 0);
        threshold_->setDecimals(1);
        threshold_->setValue(-60);
        threshold_->setSuffix(" dBFS");
        threshold_->setKeyboardTracking(false);
        scanMode_ = modeBox();
        scanGrid->addWidget(new QLabel("Od"), 0, 0);
        scanGrid->addWidget(start_, 0, 1);
        scanGrid->addWidget(new QLabel("Do"), 0, 2);
        scanGrid->addWidget(end_, 0, 3);
        scanGrid->addWidget(new QLabel("Krok"), 0, 4);
        scanGrid->addWidget(step_, 0, 5);
        scanGrid->addWidget(new QLabel("Próg"), 1, 0);
        scanGrid->addWidget(threshold_, 1, 1);
        scanGrid->addWidget(new QLabel("Modulacja"), 1, 2);
        scanGrid->addWidget(scanMode_, 1, 3);
        scanButton_ = new QPushButton("Rozpocznij skanowanie");
        scanGrid->addWidget(scanButton_, 1, 4, 1, 2);
        scanLayout->addLayout(scanGrid);
        auto *digitalControls = new QHBoxLayout;
        digitalMode_ = new QComboBox;
        digitalMode_->addItem("Wyłączony", int(DigitalMode::Off));
        digitalMode_->addItem("FT8", int(DigitalMode::FT8));
        digitalMode_->addItem("FT4", int(DigitalMode::FT4));
        digitalMode_->addItem("WSPR", int(DigitalMode::WSPR));
        digitalMode_->addItem("DMR", voiceDmr);
        digitalMode_->addItem("C4FM (YSF)", voiceYsf);
        qth_ = new QLineEdit;
        qth_->setPlaceholderText("np. KO02");
        qth_->setMaxLength(8);
        qth_->setMaximumWidth(110);
        if (!test) {
            savedQth_ = QSettings().value("qth").toString().trimmed().toUpper();
            if (!savedQth_.isEmpty() && !maidenheadLocation(savedQth_)) {
                invalidQth_ = true;
                savedQth_.clear();
            }
            qth_->setText(savedQth_);
        }
        auto *saveQth = new QPushButton("Zapisz QTH");
        digitalControls->addWidget(new QLabel("Dekoder"));
        digitalControls->addWidget(digitalMode_);
        digitalControls->addWidget(new QLabel("Mój lokator"));
        digitalControls->addWidget(qth_);
        digitalControls->addWidget(saveQth);
        clockStatus_ = new QLabel("NTP: sprawdzanie");
        digitalControls->addWidget(clockStatus_);
        digitalStatus_ = new QLabel("Dekodowanie wyłączone");
        digitalStatus_->setWordWrap(true);
        if (invalidQth_) digitalStatus_->setText("Zapisany QTH jest nieprawidłowy");
        digitalControls->addWidget(digitalStatus_, 1);
        homeLayout->addLayout(digitalControls);
        voiceMetadata_ = new QLabel("Brak danych");
        voiceMetadata_->setVisible(false);
        homeLayout->addWidget(voiceMetadata_);
        digitalTable_ = new QTableWidget(0, 9);
        digitalTable_->setHorizontalHeaderLabels({"UTC", "Tryb", "SNR", "MHz", "Znak", "Kraj / obszar",
                                                  "Lokator", "km", "Wiadomość"});
        digitalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        digitalTable_->horizontalHeader()->setStretchLastSection(true);
        digitalTable_->setColumnWidth(0, 90);
        digitalTable_->setColumnWidth(1, 60);
        digitalTable_->setColumnWidth(2, 50);
        digitalTable_->setColumnWidth(3, 100);
        digitalTable_->setColumnWidth(4, 100);
        digitalTable_->setColumnWidth(5, 170);
        digitalTable_->setColumnWidth(6, 75);
        digitalTable_->setColumnWidth(7, 65);
        digitalTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        digitalTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        digitalTable_->verticalHeader()->hide();
        digitalTable_->horizontalHeader()->setSortIndicatorShown(false);
        connect(digitalTable_->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
            if (sortColumn_ == column)
                sortOrder_ = sortOrder_ == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
            else {
                sortColumn_ = column;
                sortOrder_ = Qt::AscendingOrder;
            }
            sortMessages();
        });
        auto *messageControls = new QHBoxLayout;
        newest_ = new QPushButton("Najnowsze");
        farthest_ = new QPushButton("Najdalsze");
        clearMessages_ = new QPushButton("Wyczyść wyniki");
        messageControls->addWidget(new QLabel("Wiadomości cyfrowe"), 1);
        messageControls->addWidget(newest_);
        messageControls->addWidget(farthest_);
        messageControls->addWidget(clearMessages_);
        homeLayout->addLayout(messageControls);
        connect(newest_, &QPushButton::clicked, this, [this] {
            sortColumn_ = 0;
            sortOrder_ = Qt::DescendingOrder;
            sortMessages();
            digitalTable_->scrollToTop();
        });
        connect(farthest_, &QPushButton::clicked, this, [this] {
            sortColumn_ = 7;
            sortOrder_ = Qt::DescendingOrder;
            sortMessages();
            digitalTable_->scrollToTop();
        });
        connect(clearMessages_, &QPushButton::clicked, this, [this] {
            if (digitalTable_->rowCount() == 0) return;
            if (QMessageBox::question(this, "Wyczyść wyniki",
                    "Usunąć wszystkie wiadomości cyfrowe z bieżącej sesji?",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
                digitalTable_->setRowCount(0);
        });
        homeLayout->addWidget(digitalTable_, 1);
        auto *listControls = new QHBoxLayout;
        editList_ = new QComboBox;
        editList_->setPlaceholderText("Utwórz listę");
        newList_ = new QPushButton("Nowa lista");
        deleteList_ = new QPushButton("Usuń listę");
        listControls->addWidget(new QLabel("Lista"));
        listControls->addWidget(editList_, 1);
        listControls->addWidget(newList_);
        listControls->addWidget(deleteList_);
        scanLayout->addLayout(listControls);
        auto *manualControls = new QHBoxLayout;
        stationName_ = new QLineEdit;
        stationName_->setPlaceholderText("Nazwa (opcjonalna)");
        manualFrequency_ = frequencyBox(7.1);
        manualMode_ = modeBox();
        addManual_ = new QPushButton("Dodaj");
        removeStation_ = new QPushButton("Usuń z listy");
        manualControls->addWidget(stationName_, 1);
        manualControls->addWidget(manualFrequency_);
        manualControls->addWidget(manualMode_);
        manualControls->addWidget(addManual_);
        manualControls->addWidget(removeStation_);
        scanLayout->addLayout(manualControls);
        auto *tables = new QHBoxLayout;
        auto *scanResults = new QVBoxLayout;
        scanResults->addWidget(new QLabel("Wyniki skanowania — zaznacz do zapisania"));
        table_ = new QTableWidget(0, 4);
        table_->setHorizontalHeaderLabels({"✓", "MHz", "Tryb", "dBFS"});
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
        table_->setColumnWidth(0, 45);
        table_->verticalHeader()->setDefaultSectionSize(36);
        table_->setStyleSheet("QTableWidget::indicator { width: 24px; height: 24px; }");
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->verticalHeader()->hide();
        scanResults->addWidget(table_, 1);
        auto *scanActions = new QHBoxLayout;
        selectAll_ = new QPushButton("Zaznacz wszystkie");
        deselectAll_ = new QPushButton("Odznacz wszystkie");
        saveSelected_ = new QPushButton("Zapisz zaznaczone");
        scanActions->addWidget(selectAll_);
        scanActions->addWidget(deselectAll_);
        scanResults->addLayout(scanActions);
        auto *listResults = new QVBoxLayout;
        listResults->addWidget(new QLabel("Częstotliwości w wybranej liście"));
        listTable_ = new QTableWidget(0, 3);
        listTable_->setHorizontalHeaderLabels({"Nazwa", "MHz", "Tryb"});
        listTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        listTable_->verticalHeader()->setDefaultSectionSize(36);
        listTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        listTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        listTable_->verticalHeader()->hide();
        listResults->addWidget(listTable_, 1);
        listResults->addWidget(saveSelected_);
        tables->addLayout(scanResults, 1);
        tables->addLayout(listResults, 1);
        scanLayout->addLayout(tables, 1);
        auto *note = new QLabel("Skanowanie przerywa odsłuch; wykryty sygnał nie musi być stacją.");
        note->setWordWrap(true);
        scanLayout->addWidget(note);
        auto *positionControls = new QHBoxLayout;
        adsbLatitude_ = new QLineEdit;
        adsbLongitude_ = new QLineEdit;
        adsbLatitude_->setPlaceholderText("np. 52.123456");
        adsbLongitude_->setPlaceholderText("np. 21.123456");
        adsbLatitude_->setMaximumWidth(150);
        adsbLongitude_->setMaximumWidth(150);
        auto *latValidator = new QDoubleValidator(-90, 90, 6, adsbLatitude_);
        auto *lonValidator = new QDoubleValidator(-180, 180, 6, adsbLongitude_);
        latValidator->setNotation(QDoubleValidator::StandardNotation);
        lonValidator->setNotation(QDoubleValidator::StandardNotation);
        latValidator->setLocale(QLocale::c());
        lonValidator->setLocale(QLocale::c());
        adsbLatitude_->setValidator(latValidator);
        adsbLongitude_->setValidator(lonValidator);
        saveAdsbPosition_ = new QPushButton("Zapisz pozycję");
        adsbStart_ = new QPushButton("Uruchom ADS-B");
        positionControls->addWidget(new QLabel("Szerokość"));
        positionControls->addWidget(adsbLatitude_);
        positionControls->addWidget(new QLabel("Długość"));
        positionControls->addWidget(adsbLongitude_);
        positionControls->addWidget(saveAdsbPosition_);
        positionControls->addWidget(adsbStart_);
        adsbLayout->addLayout(positionControls);
        auto *adsbNote = new QLabel("ADS-B: 1090 MHz. Odbiór i skanowanie są wstrzymane podczas pracy dekodera.");
        adsbNote->setWordWrap(true);
        adsbLayout->addWidget(adsbNote);
        adsbStatus_ = new QLabel("ADS-B wyłączony. Wpisz współrzędne, aby obliczać odległości.");
        adsbStatus_->setWordWrap(true);
        adsbStatus_->setMaximumHeight(50);
        adsbLayout->addWidget(adsbStatus_);
        aircraftTable_ = new QTableWidget(0, 8);
        aircraftTable_->setHorizontalHeaderLabels({"ICAO", "Lot / znak", "Wys. m", "km/h", "Kurs °",
                                                   "km", "Pozycja", "Wiek s"});
        aircraftTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        const int aircraftWidths[] = {95, 130, 95, 95, 85, 85, 200, 75};
        for (int i = 0; i < 8; ++i) aircraftTable_->setColumnWidth(i, aircraftWidths[i]);
        aircraftTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
        aircraftTable_->verticalHeader()->setDefaultSectionSize(36);
        aircraftTable_->verticalHeader()->hide();
        aircraftTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        adsbLayout->addWidget(aircraftTable_, 1);
        auto *aircraftNote = new QLabel("Odległość po powierzchni Ziemi; pozycje sprzed ponad 60 s nie są wyświetlane.");
        aircraftNote->setWordWrap(true);
        adsbLayout->addWidget(aircraftNote);
        auto *closeButton = new QPushButton("Zamknij");
        root->addWidget(closeButton);
        status_ = new QLabel(test ? "Test interfejsu" : "Łączenie z odbiornikiem...");
        audioStatus_ = new QLabel;
        root->addWidget(status_);
        root->addWidget(audioStatus_);
        connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
        connect(mute_, &QPushButton::toggled, this, [this](bool muted) {
            mute_->setText(muted ? "Włącz dźwięk" : "Wycisz");
            if (audio_) audio_->setVolume(muted || adsb_ ? 0 : volume_->value() / 100.0);
        });
        connect(volume_, &QSlider::valueChanged, this, [this](int value) {
            if (audio_ && !mute_->isChecked() && !adsb_) audio_->setVolume(value / 100.0);
        });
        setupAdsbPosition(test);
        setupLists(test);
        if (test) {
            countries_ = std::make_unique<Countries>("/usr/share/wsjtx/cty.dat",
                                                      "/usr/share/iso-codes/json/iso_3166-1.json");
            savedQth_ = "KO02";
            digitalStatus_->setText("Oczekiwanie na początek pełnego przedziału UTC");
            addDigitalMessage({"120000", "CQ SP9ABC JO90", "SP9ABC", "JO90", -10, 14075500, DigitalMode::FT8});
            return;
        }

        try {
            countries_ = std::make_unique<Countries>("/usr/share/wsjtx/cty.dat",
                                                      "/usr/share/iso-codes/json/iso_3166-1.json");
        } catch (const std::exception &error) {
            digitalStatus_->setText("Brak danych krajów: " + QString::fromUtf8(error.what()));
            QMessageBox::warning(this, "Dane krajów", "Nie można wczytać bazy krajów: " +
                                 QString::fromUtf8(error.what()));
        }
        try {
            digital_ = new DigitalDecoder(this);
            connect(digital_, &DigitalDecoder::statusChanged, this, [this](const QString &status) {
                digitalStatus_->setText(status);
                if (status.startsWith("Dekodowanie zakończone")) digitalComplete_ = true;
            });
            connect(digital_, &DigitalDecoder::failed, this, [this](const QString &message) {
                digitalMode_->setCurrentIndex(0);
                digitalStatus_->setText("Błąd dekodera: " + message);
            });
            connect(digital_, &DigitalDecoder::decoded, this, [this](const DigitalMessage &message) {
                addDigitalMessage(message);
            });
        } catch (const std::exception &error) {
            digitalMode_->setEnabled(false);
            digitalStatus_->setText("Błąd dekodera: " + QString::fromUtf8(error.what()));
        }
        voice_ = new VoiceDecoder(this, std::move(voiceExecutable));
        voiceAudioTimer_ = new QTimer(this);
        voiceAudioTimer_->setInterval(10);
        connect(voiceAudioTimer_, &QTimer::timeout, this, [this] { flushVoiceAudio(); });
        connect(voice_, &VoiceDecoder::metadataChanged, voiceMetadata_, &QLabel::setText);
        connect(voice_, &VoiceDecoder::statusChanged, digitalStatus_, &QLabel::setText);
        connect(voice_, &VoiceDecoder::failed, this, [this](const QString &message) {
            digitalMode_->setCurrentIndex(0);
            digitalStatus_->setText("Błąd dekodera: " + message);
        });
        connect(voice_, &VoiceDecoder::audioReady, this, [this](const QByteArray &pcm) {
            if (voiceMode() == VoiceMode::Off || adsb_ || !audioDevice_) return;
            if (voiceAudio_.size() + pcm.size() > kAudioRate * 4) {
                digitalMode_->setCurrentIndex(0);
                digitalStatus_->setText("Błąd dekodera: przepełnienie bufora audio.");
                return;
            }
            voiceAudio_.append(pcm);
            flushVoiceAudio();
            if (!voiceAudio_.isEmpty()) voiceAudioTimer_->start();
        });
        clock_ = new QProcess(this);
        connect(clock_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            const auto response = clock_->readAllStandardOutput().trimmed();
            const bool synced = code == 0 && status == QProcess::NormalExit && response == "yes";
            clockStatus_->setText(synced ? "NTP: OK" : "NTP: NIE");
            clockStatus_->setToolTip(synced ? "Czas systemowy jest zsynchronizowany" :
                                               "Czas systemowy nie jest zsynchronizowany");
        });
        connect(clock_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            clockStatus_->setText("NTP: ?");
            clockStatus_->setToolTip("Nie można sprawdzić synchronizacji czasu");
        });
        auto checkClock = [this] {
            if (clock_->state() == QProcess::NotRunning)
                clock_->start("timedatectl", {"show", "--property=NTPSynchronized", "--value"});
        };
        clockTimer_ = new QTimer(this);
        connect(clockTimer_, &QTimer::timeout, this, checkClock);
        clockTimer_->start(60000);
        checkClock();

        QAudioFormat format;
        format.setSampleRate(kAudioRate);
        format.setChannelCount(2);
        format.setSampleSize(16);
        format.setCodec("audio/pcm");
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);
        auto device = QAudioDeviceInfo::defaultOutputDevice();
        if (device.isNull() || !device.isFormatSupported(format)) {
            audioStatus_->setText("Brak zgodnego wyjścia audio 48 kHz stereo. Skaner nadal jest dostępny.");
        } else {
            audio_ = new QAudioOutput(device, format, this);
            connect(audio_, &QAudioOutput::stateChanged, this, [this](QAudio::State state) {
                if (state == QAudio::StoppedState && audio_->error() != QAudio::NoError) {
                    audioStatus_->setText(QString("Błąd wyjścia audio (kod %1).").arg(int(audio_->error())));
                    audioDevice_ = nullptr;
                }
            });
            audio_->setBufferSize(kAudioRate * 4 / 4);
            audio_->setVolume(volume_->value() / 100.0);
            audioDevice_ = audio_->start();
            if (!audioDevice_ || audio_->error() == QAudio::OpenError ||
                audio_->error() == QAudio::IOError || audio_->error() == QAudio::FatalError) {
                audioDevice_ = nullptr;
                audioStatus_->setText("Nie udało się uruchomić wyjścia audio. Skaner nadal jest dostępny.");
            } else
                audioStatus_->setText("Wyjście audio: " + device.deviceName());
        }
        receiver_ = new Receiver(dataFile(), this);
        connect(receiver_, &Receiver::statusChanged, status_, &QLabel::setText);
        connect(receiver_, &Receiver::blockReceived, this, [this] { if (!adsb_) receivedIq_ = true; });
        connect(receiver_, &Receiver::audioGap, this, [this] {
            if (digital_ && !adsb_) digital_->resetCapture();
            if (voice_ && !adsb_) voice_->gap();
        });
        connect(receiver_, &Receiver::signalLevel, this, [this](double level) {
            if (adsb_) return;
            signal_->setText(QString("Sygnał: %1 dBFS").arg(level, 0, 'f', 1));
            if (std::isfinite(level)) {
                meter_->setValue(int(std::lround(level)));
                signalSeen_ = true;
            }
        });
        connect(receiver_, &Receiver::failed, this, [this](const QString &message) {
            scanning_ = false;
            setScanUi(false);
            scanButton_->setEnabled(false);
            frequency_->setEnabled(false);
            mode_->setEnabled(false);
            digitalMode_->setEnabled(false);
            adsbStart_->setEnabled(false);
            status_->setText("Błąd odbiornika: " + message);
            QMessageBox::critical(this, "Błąd odbiornika", message);
        });
        connect(receiver_, &Receiver::audioReady, this, [this](const QByteArray &pcm, qint64 timestamp, qint64 frequency, int mode) {
            if (adsb_) return;
            const auto desired = settings();
            if (frequency != desired.frequency || mode != int(desired.mode)) return;
            if (voiceMode() != VoiceMode::Off) {
                if (voice_ && !scanning_) {
                    voice_->feed(pcm);
                    ++voiceInputBlocks_;
                }
                return;
            }
            if (digital_ && !scanning_) digital_->feed(pcm, timestamp);
            if (!scanning_) playAudio(pcm);
        });
        connect(receiver_, &Receiver::detected, this, [this](quint64 scanId, qint64 freq, double level, const QString &mode) {
            receiveScanDetection(scanId, freq, level, mode);
        });
        connect(receiver_, &Receiver::scanFinished, this, [this](quint64 scanId) {
            finishScan(scanId);
        });
        connect(frequency_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { applySettings(); });
        connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { applySettings(); });
        connect(scanButton_, &QPushButton::clicked, this, [this] { toggleScan(); });
        connect(adsbStart_, &QPushButton::clicked, this, [this] { toggleAdsb(); });
        connect(saveQth, &QPushButton::clicked, this, [this] {
            const QString locator = qth_->text().trimmed().toUpper();
            if (!locator.isEmpty() && !maidenheadLocation(locator)) {
                QMessageBox::warning(this, "QTH", "Podaj poprawny lokator Maidenhead (4, 6 lub 8 znaków).");
                return;
            }
            QSettings settings;
            settings.setValue("qth", locator);
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                QMessageBox::warning(this, "QTH", "Nie udało się zapisać lokatora.");
                return;
            }
            qth_->setText(locator);
            savedQth_ = locator;
            for (int row = 0; row < digitalTable_->rowCount(); ++row) updateDistance(row);
            sortMessages();
            digitalStatus_->setText(locator.isEmpty() ? "Usunięto lokator QTH" : "Zapisano lokator QTH");
        });
        connect(digitalMode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
            if (!digital_) return;
            const bool enabled = digitalMode_->currentData().toInt() != int(DigitalMode::Off);
            if (enabled && !digitalEnabled_) {
                previousMode_ = mode_->currentIndex();
            } else if (!enabled && digitalEnabled_) {
                mode_->setCurrentIndex(previousMode_);
            }
            if (enabled) mode_->setCurrentIndex(mode_->findData(int(voiceMode() == VoiceMode::Off ? Mode::USB : Mode::NFM)));
            digitalEnabled_ = enabled;
            voiceMetadata_->setVisible(voiceMode() != VoiceMode::Off);
            mode_->setEnabled(!enabled && !scanning_);
            scanButton_->setEnabled(!enabled && receiver_->isRunning());
            applySettings();
            if (enabled) pages_->setCurrentIndex(0);
        });
        applySettings();
        receiver_->start();
    }

    ~Window() override {
        audioDevice_ = nullptr;
        if (clockTimer_) clockTimer_->stop();
        if (clock_) {
            disconnect(clock_, nullptr, this, nullptr);
            if (clock_->state() != QProcess::NotRunning) {
                clock_->kill();
                clock_->waitForFinished(1000);
            }
        }
        if (receiver_) {
            receiver_->stop();
            QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
            disconnect(receiver_, nullptr, this, nullptr);
        }
        if (adsb_) {
            disconnect(adsb_, nullptr, this, nullptr);
            delete adsb_;
            adsb_ = nullptr;
        }
        if (digital_) {
            auto *decoder = digital_;
            digital_ = nullptr;
            delete decoder;
        }
        delete voice_;
        voice_ = nullptr;
        if (audio_) {
            disconnect(audio_, nullptr, this, nullptr);
            audio_->stop();
            delete audio_;
            audio_ = nullptr;
        }
    }

    bool smokeSucceeded() const { return receivedIq_ && audioDevice_ && audioBlocks_ > 10; }
    void startVoiceSmoke(VoiceMode mode) {
        voiceInputBlocks_ = 0;
        mute_->setChecked(true);
        frequency_->setValue(mode == VoiceMode::DMR ? 145.5 : 145.525);
        digitalMode_->setCurrentIndex(digitalMode_->findData(mode == VoiceMode::DMR ? voiceDmr : voiceYsf));
    }
    bool voiceSmokeSucceeded(VoiceMode mode) const {
        return voiceMode() == mode && voice_ && voice_->isRunning() && voiceInputBlocks_ > 100 &&
            mute_->isChecked() && signalSeen_;
    }
    bool finishVoiceSmoke() {
        digitalMode_->setCurrentIndex(0);
        return voiceMode() == VoiceMode::Off && mode_->isEnabled() && scanButton_->isEnabled();
    }
    void startVoiceAdsbSmoke() { adsbUpdates_ = 0; toggleAdsb(); }
    bool stopVoiceAdsbSmoke() {
        if (!adsbSmokeRunning()) return false;
        toggleAdsb();
        voiceInputBlocks_ = 0;
        return voiceMode() == VoiceMode::DMR;
    }
    void voiceUiTest() {
        digitalMode_->setCurrentIndex(digitalMode_->findData(voiceDmr));
        voiceMetadata_->setText("Color code: 04 | Slot 2 | Cel: 91 | Źródło: 123");
        voiceMetadata_->setVisible(true);
    }
    void startDigitalSmoke() {
        mute_->setChecked(true);
        frequency_->setValue(14.080);
        mode_->setCurrentIndex(mode_->findData(int(Mode::USB)));
        digitalMode_->setCurrentIndex(digitalMode_->findData(int(DigitalMode::FT4)));
    }
    bool digitalSmokeSucceeded() const { return receivedIq_ && signalSeen_ && digitalComplete_ && mute_->isChecked(); }
    void startAdsbSmoke(bool digital) {
        if (digital) startDigitalSmoke();
        adsbUpdates_ = 0;
        toggleAdsb();
        receivedIq_ = false;
        signalSeen_ = false;
        digitalComplete_ = false;
    }
    bool adsbSmokeRunning() const { return adsb_ && adsbUpdates_ > 0 && !receiver_->isRunning(); }
    bool adsbFailureRestored() const {
        return !adsb_ && receiver_->isRunning() && adsbStatus_->text().startsWith("Błąd ADS-B") &&
            digitalMode_->currentData().toInt() == int(DigitalMode::FT4);
    }
    bool stopAdsbSmoke(bool digital) {
        if (!adsbSmokeRunning()) return false;
        toggleAdsb();
        receivedIq_ = false;
        signalSeen_ = false;
        digitalComplete_ = false;
        return !adsb_ && digitalMode_->currentData().toInt() ==
            int(digital ? DigitalMode::FT4 : DigitalMode::Off) &&
            (digital || scanButton_->isEnabled());
    }
    bool listUiTest() {
        if (!lists_ || !testListsDirectory_) return false;
        addDetection(7000000, -50, "LSB");
        scanMode_->setCurrentIndex(scanMode_->findData(int(Mode::AM)));
        toggleScan();
        if (!scanning_ || settings().mode != Mode::AM || mainList_->isEnabled() ||
            table_->rowCount() != 0) return false;
        const quint64 firstScan = scanId_;
        receiveScanDetection(firstScan, 7100000, -50, "AM");
        toggleScan();
        if (scanning_ || table_->rowCount() != 1) return false;
        toggleScan();
        receiveScanDetection(firstScan, 7100000, -50, "AM");
        finishScan(firstScan);
        if (!scanning_ || table_->rowCount() != 0) return false;
        receiveScanDetection(scanId_, 7200000, -40, "AM");
        if (table_->rowCount() != 1) return false;
        toggleScan();
        toggleScan();
        if (table_->rowCount() != 0) return false;
        toggleScan();
        lists_->save({{"Test", {}}, {"Druga", {{"FT8", 14074000, "USB"}}}});
        refreshLists(0);
        stationName_->setText("Radio");
        manualFrequency_->setValue(100.1);
        manualMode_->setCurrentIndex(manualMode_->findData(int(Mode::WFM)));
        addManual_->click();
        addDetection(7101000, -50, "LSB");
        addDetection(7200000, -40, "AM");
        table_->item(0, 0)->setCheckState(Qt::Checked);
        saveSelected_->click();
        if (lists_->lists()[0].stations.size() != 2) return false;
        selectAll_->click();
        deselectAll_->click();
        if (table_->item(0, 0)->checkState() != Qt::Unchecked ||
            table_->item(1, 0)->checkState() != Qt::Unchecked) return false;
        selectAll_->click();
        saveSelected_->click();
        saveSelected_->click();
        if (lists_->lists()[0].stations.size() != 3) return false;
        nextStation_->click();
        if (std::llround(frequency_->value() * 1000000) != 100100000 ||
            Mode(mode_->currentData().toInt()) != Mode::WFM) return false;
        nextStation_->click();
        if (std::llround(frequency_->value() * 1000000) != 7101000 ||
            Mode(mode_->currentData().toInt()) != Mode::LSB) return false;
        editList_->setCurrentIndex(1);
        if (mainList_->currentIndex() != 1) return false;
        nextStation_->click();
        if (std::llround(frequency_->value() * 1000000) != 14074000 ||
            Mode(mode_->currentData().toInt()) != Mode::USB) return false;
        frequency_->setValue(7.1);
        nextStation_->click();
        if (std::llround(frequency_->value() * 1000000) != 14074000) return false;
        const int volume = volume_->value();
        mute_->click();
        if (!mute_->isChecked() || volume_->value() != volume) return false;
        mute_->click();
        if (mute_->isChecked() || volume_->value() != volume) return false;
        StationLists reloaded(testListsDirectory_->filePath("lists.json"));
        if (reloaded.lists().size() != 2 || reloaded.lists()[0].stations.size() != 3) return false;
        pages_->setCurrentIndex(1);
        return true;
    }
    bool digitalSortTest() {
        addDigitalMessage({"235959", "CQ K1ABC FN42", "K1ABC", "FN42", -5, 14075500, DigitalMode::FT8});
        addDigitalMessage({"000000", "CQ SP1ABC KO02", "SP1ABC", "KO02", -20, 7075000, DigitalMode::FT8});
        addDigitalMessage({"000001", "SP1ABC K1ABC RR73", "K1ABC", "", -10, 14076000, DigitalMode::FT8});
        if (digitalTable_->item(0, 0)->text() != "000001" ||
            digitalTable_->item(1, 0)->text() != "000000" ||
            digitalTable_->item(2, 0)->text() != "235959") {
            std::cerr << "Newest order incorrect\n"; return false;
        }
        farthest_->click();
        if (digitalTable_->item(0, 6)->text() != "FN42" ||
            digitalTable_->item(3, 7)->text() != "—") {
            std::cerr << "Farthest order incorrect\n"; return false;
        }
        addDigitalMessage({"000002", "CQ SP2ABC KO02", "SP2ABC", "KO02", -7, 14077000, DigitalMode::FT8});
        if (digitalTable_->item(0, 6)->text() != "FN42" ||
            digitalTable_->item(4, 7)->text() != "—") {
            std::cerr << "Farthest order after insertion incorrect\n"; return false;
        }
        newest_->click();
        if (digitalTable_->item(0, 0)->text() != "000002") { std::cerr << "Newest button failed\n"; return false; }
        addDigitalMessage({"000003", "CQ SP3ABC JO90", "SP3ABC", "JO90", -7, 14078000, DigitalMode::FT8});
        if (digitalTable_->item(0, 0)->text() != "000003") { std::cerr << "Newest insertion failed\n"; return false; }
        sortColumn_ = 7;
        sortOrder_ = Qt::AscendingOrder;
        sortMessages();
        if (digitalTable_->item(0, 7)->text() != "0" ||
            digitalTable_->item(1, 7)->text() != "0" ||
            digitalTable_->item(4, 4)->text() != "K1ABC" ||
            digitalTable_->item(5, 7)->text() != "—") { std::cerr << "Ascending distance failed\n"; return false; }
        addDigitalMessage({"120004", "CQ SP4ABC KO02", "SP4ABC", "KO02", -7, 14077000, DigitalMode::FT8});
        if (digitalTable_->item(0, 7)->text() != "0" || digitalTable_->item(1, 7)->text() != "0" ||
            digitalTable_->item(6, 7)->text() != "—") { std::cerr << "Ascending insertion failed\n"; return false; }
        sortColumn_ = 3;
        sortMessages();
        if (digitalTable_->item(0, 4)->text() != "SP1ABC") { std::cerr << "Frequency sort failed\n"; return false; }
        savedQth_ = "FN42";
        for (int row = 0; row < digitalTable_->rowCount(); ++row) updateDistance(row);
        sortColumn_ = 7;
        sortMessages();
        if (digitalTable_->item(0, 6)->text() != "FN42" || digitalTable_->item(0, 7)->text() != "0")
            { std::cerr << "QTH distance update failed\n"; return false; }
        sortColumn_ = -1;
        while (digitalTable_->rowCount() < 2000)
            addDigitalMessage({"120005", "CQ K1ABC FN42", "K1ABC", "FN42", -5, 14075500, DigitalMode::FT8});
        sortColumn_ = 7;
        sortMessages();
        addDigitalMessage({"120006", "CQ K1ABC FN42", "K1ABC", "FN42", -5, 14075500, DigitalMode::FT8});
        if (digitalTable_->rowCount() != 2000) { std::cerr << "Table limit failed\n"; return false; }
        for (int row = 0; row < digitalTable_->rowCount(); ++row)
            if (digitalTable_->item(row, 4)->text() == "SP9ABC") { std::cerr << "Oldest removal failed\n"; return false; }
        lists_->save({{"Zachowana", {{"Stacja", 7100000, "LSB"}}}});
        addDetection(7101000, -50, "LSB");
        auto answer = [](QMessageBox::StandardButton button) {
            QTimer::singleShot(0, [button] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                    box->button(button)->click();
            });
        };
        answer(QMessageBox::No);
        clearMessages_->click();
        if (digitalTable_->rowCount() != 2000) { std::cerr << "Clear cancellation failed\n"; return false; }
        answer(QMessageBox::Yes);
        clearMessages_->click();
        if (digitalTable_->rowCount() != 0 || table_->rowCount() != 1 || lists_->lists().size() != 1 ||
            lists_->lists()[0].stations.size() != 1) {
            std::cerr << "Clear isolation failed: digital=" << digitalTable_->rowCount()
                      << " scan=" << table_->rowCount() << " lists=" << lists_->lists().size() << '\n';
            return false;
        }
        addDigitalMessage({"120007", "CQ K1ABC FN42", "K1ABC", "FN42", -5, 14075500, DigitalMode::FT8});
        return digitalTable_->rowCount() == 1;
    }
    bool adsbUiTest() {
        adsbLatitude_->setText("52.260000");
        adsbLongitude_->setText("3.920000");
        saveAdsbPosition_->click();
        if (!adsbQth_ || std::abs(adsbQth_->latitude - 52.26) > 0.00001 ||
            std::abs(QSettings().value("adsbLongitude").toDouble() - 3.92) > 0.00001) return false;
        const auto planes = parseAircraftJson(R"({"aircraft":[
          {"hex":"40621d","flight":"TEST123","lat":52.265780,"lon":3.938913,
           "seen_pos":1,"seen":2,"alt_baro":38000,"gs":200,"track":90},
          {"hex":"123abc","seen":4}
        ]})");
        showAircraft(planes);
        if (aircraftTable_->rowCount() != 2 || aircraftTable_->item(0, 0)->text() != "40621D" ||
            aircraftTable_->item(0, 5)->text() != "1" || aircraftTable_->item(1, 5)->text() != "—" ||
            aircraftTable_->item(0, 2)->text() != "11582") return false;
        aircraftSortOrder_ = Qt::DescendingOrder;
        sortAircraft();
        if (aircraftTable_->item(1, 5)->text() != "—") return false;
        pages_->setCurrentIndex(2);
        return true;
    }

private:
    void setupAdsbPosition(bool test) {
        if (!test) {
            QSettings settings;
            const bool hasLat = settings.contains("adsbLatitude"), hasLon = settings.contains("adsbLongitude");
            if (hasLat || hasLon) {
                bool latOk = false, lonOk = false;
                const double lat = settings.value("adsbLatitude").toDouble(&latOk);
                const double lon = settings.value("adsbLongitude").toDouble(&lonOk);
                if (!hasLat || !hasLon || !latOk || !lonOk || !std::isfinite(lat) || !std::isfinite(lon) ||
                    lat < -90 || lat > 90 || lon < -180 || lon > 180) {
                    adsbStatus_->setText("Zapisana pozycja ADS-B jest nieprawidłowa.");
                } else {
                    adsbQth_ = Location{lat, lon};
                    adsbLatitude_->setText(QString::number(lat, 'f', 6));
                    adsbLongitude_->setText(QString::number(lon, 'f', 6));
                }
            }
        }
        connect(saveAdsbPosition_, &QPushButton::clicked, this, [this] {
            const QString latitude = adsbLatitude_->text().trimmed();
            const QString longitude = adsbLongitude_->text().trimmed();
            std::optional<Location> location;
            if (!latitude.isEmpty() || !longitude.isEmpty()) {
                bool latOk = false, lonOk = false;
                const double lat = QLocale::c().toDouble(latitude, &latOk);
                const double lon = QLocale::c().toDouble(longitude, &lonOk);
                if (!latOk || !lonOk || !std::isfinite(lat) || !std::isfinite(lon) ||
                    lat < -90 || lat > 90 || lon < -180 || lon > 180 ||
                    !adsbLatitude_->hasAcceptableInput() || !adsbLongitude_->hasAcceptableInput()) {
                    QMessageBox::warning(this, "Pozycja ADS-B",
                                         "Podaj obie współrzędne w stopniach: szerokość −90 do 90, długość −180 do 180.");
                    return;
                }
                location = Location{lat, lon};
            }
            QSettings settings;
            if (location) {
                settings.setValue("adsbLatitude", location->latitude);
                settings.setValue("adsbLongitude", location->longitude);
            } else {
                settings.remove("adsbLatitude");
                settings.remove("adsbLongitude");
            }
            settings.sync();
            if (settings.status() != QSettings::NoError) {
                QMessageBox::warning(this, "Pozycja ADS-B", "Nie udało się zapisać pozycji.");
                return;
            }
            adsbQth_ = location;
            adsbStatus_->setText(location ? "Zapisano pozycję ADS-B lokalnie." : "Usunięto pozycję ADS-B.");
        });
        aircraftTable_->horizontalHeader()->setSortIndicatorShown(true);
        aircraftTable_->horizontalHeader()->setSortIndicator(aircraftSortColumn_, aircraftSortOrder_);
        connect(aircraftTable_->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
            if (column == aircraftSortColumn_)
                aircraftSortOrder_ = aircraftSortOrder_ == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
            else {
                aircraftSortColumn_ = column;
                aircraftSortOrder_ = Qt::AscendingOrder;
            }
            sortAircraft();
        });
    }

    void sortAircraft() {
        aircraftTable_->horizontalHeader()->setSortIndicator(aircraftSortColumn_, aircraftSortOrder_);
        aircraftTable_->sortItems(aircraftSortColumn_, aircraftSortOrder_);
    }

    void showAircraft(const QVector<Aircraft> &aircraft) {
        aircraftTable_->setRowCount(0);
        for (const auto &plane : aircraft) {
            const int row = aircraftTable_->rowCount();
            aircraftTable_->insertRow(row);
            aircraftTable_->setItem(row, 0, new QTableWidgetItem(plane.icao));
            aircraftTable_->setItem(row, 1, new QTableWidgetItem(plane.flight.isEmpty() ? "—" : plane.flight));
            auto numeric = [this, row](int column, std::optional<double> value, const QString &fallback = "—") {
                auto *item = new NumericItem(value ? QString::number(*value, 'f', 0) : fallback);
                if (value) item->setData(Qt::UserRole, *value);
                aircraftTable_->setItem(row, column, item);
            };
            numeric(2, plane.altitudeMetres, plane.onGround ? "Na ziemi" : "—");
            numeric(3, plane.speedKmh);
            numeric(4, plane.trackDegrees);
            const std::optional<double> distance = adsbQth_ && plane.position ?
                std::optional<double>(distanceKm(*adsbQth_, *plane.position)) : std::nullopt;
            numeric(5, distance);
            aircraftTable_->setItem(row, 6, new QTableWidgetItem(plane.position ?
                QString::number(plane.position->latitude, 'f', 4) + ", " +
                QString::number(plane.position->longitude, 'f', 4) : "—"));
            numeric(7, plane.ageSeconds);
        }
        sortAircraft();
    }

    void toggleAdsb() {
        if (adsb_) {
            if (stopAdsb()) adsbStatus_->setText("ADS-B wyłączony. Uruchamianie odbiornika.");
            return;
        }
        if (scanning_) {
            adsbStatus_->setText("Najpierw zatrzymaj skanowanie.");
            return;
        }
        if (!receiver_ || !receiver_->isRunning()) {
            adsbStatus_->setText("Odbiornik nie jest gotowy.");
            return;
        }
        const QString executable = adsbExecutable_.isEmpty() ?
            QDir::home().filePath("sdr-touch-readsb/readsb") : adsbExecutable_;
        if (!QFileInfo(executable).isExecutable()) {
            adsbStatus_->setText("Brak dekodera ADS-B: " + executable);
            return;
        }
        try {
            adsb_ = new AdsbDecoder(this);
        } catch (const std::exception &error) {
            adsbStatus_->setText("Błąd ADS-B: " + QString::fromUtf8(error.what()));
            return;
        }
        auto *decoder = adsb_;
        connect(decoder, &AdsbDecoder::updated, this, [this, decoder] {
            if (adsb_ != decoder) return;
            ++adsbUpdates_;
            showAircraft(decoder->aircraft());
            adsbStatus_->setText(QString("ADS-B: %1 samolotów. %2")
                .arg(decoder->aircraft().size()).arg(adsbQth_ ? "QTH zapisane." : "Brak QTH: odległości niedostępne."));
        });
        connect(decoder, &AdsbDecoder::failed, this, [this, decoder](const QString &message) {
            if (adsb_ != decoder) return;
            if (stopAdsb()) {
                adsbStatus_->setText("Błąd ADS-B. Uruchamianie odbiornika.");
                QMessageBox::warning(this, "Błąd ADS-B", message);
            }
        }, Qt::QueuedConnection);
        savedDigitalMode_ = digitalMode_->currentIndex();
        if (savedDigitalMode_ != 0) digitalMode_->setCurrentIndex(0);
        receiver_->stop();
        if (audio_) audio_->setVolume(0);
        frequency_->setEnabled(false);
        mode_->setEnabled(false);
        digitalMode_->setEnabled(false);
        scanButton_->setEnabled(false);
        mainList_->setEnabled(false);
        stationChoice_->setEnabled(false);
        previousStation_->setEnabled(false);
        nextStation_->setEnabled(false);
        adsbLatitude_->setEnabled(false);
        adsbLongitude_->setEnabled(false);
        saveAdsbPosition_->setEnabled(false);
        adsbStart_->setText("Zatrzymaj ADS-B");
        aircraftTable_->setRowCount(0);
        adsbStatus_->setText("Uruchamianie ADS-B na 1090 MHz...");
        decoder->start(executable, adsbQth_);
    }

    bool stopAdsb() {
        if (!adsb_) return true;
        if (!adsb_->stop()) {
            adsbStatus_->setText("Nie można zatrzymać dekodera ADS-B; odbiornik pozostaje wstrzymany.");
            return false;
        }
        auto *decoder = adsb_;
        adsb_ = nullptr;
        disconnect(decoder, nullptr, this, nullptr);
        delete decoder;
        aircraftTable_->setRowCount(0);
        adsbLatitude_->setEnabled(true);
        adsbLongitude_->setEnabled(true);
        saveAdsbPosition_->setEnabled(true);
        adsbStart_->setText("Uruchom ADS-B");
        receiver_->restart();
        setScanUi(false);
        if (savedDigitalMode_ != 0) digitalMode_->setCurrentIndex(savedDigitalMode_);
        else scanButton_->setEnabled(true);
        if (audio_) audio_->setVolume(mute_->isChecked() ? 0 : volume_->value() / 100.0);
        return true;
    }

    void setupLists(bool test) {
        try {
            QString path;
            if (test) {
                testListsDirectory_ = std::make_unique<QTemporaryDir>();
                if (!testListsDirectory_->isValid()) throw std::runtime_error("Brak katalogu testowego");
                path = testListsDirectory_->filePath("lists.json");
            } else {
                path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                           .filePath("listy-czestotliwosci.json");
            }
            lists_ = std::make_unique<StationLists>(path);
        } catch (const std::exception &error) {
            QMessageBox::warning(this, "Listy", "Nie można wczytać list: " + QString::fromUtf8(error.what()));
        }
        connect(mainList_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            const QSignalBlocker blocker(editList_);
            editList_->setCurrentIndex(index);
            refreshStations();
        });
        connect(editList_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            const QSignalBlocker blocker(mainList_);
            mainList_->setCurrentIndex(index);
            refreshStations();
        });
        connect(stationChoice_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            tuneStation(index);
        });
        connect(previousStation_, &QPushButton::clicked, this, [this] { stepStation(-1); });
        connect(nextStation_, &QPushButton::clicked, this, [this] { stepStation(1); });
        connect(newList_, &QPushButton::clicked, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, "Nowa lista", "Nazwa listy:",
                                                       QLineEdit::Normal, {}, &ok).trimmed();
            if (!ok) return;
            if (name.isEmpty()) {
                QMessageBox::warning(this, "Lista", "Podaj nazwę listy.");
                return;
            }
            auto updated = lists_->lists();
            updated.append({name, {}});
            saveLists(updated, updated.size() - 1);
        });
        connect(deleteList_, &QPushButton::clicked, this, [this] {
            const int index = editList_->currentIndex();
            if (!lists_ || index < 0) return;
            if (QMessageBox::question(this, "Usuń listę",
                    "Usunąć listę „" + lists_->lists()[index].name + "” wraz z jej częstotliwościami?") != QMessageBox::Yes)
                return;
            auto updated = lists_->lists();
            updated.removeAt(index);
            saveLists(updated, std::min(index, updated.size() - 1));
        });
        connect(addManual_, &QPushButton::clicked, this, [this] {
            const int index = editList_->currentIndex();
            if (!lists_ || index < 0) return;
            auto updated = lists_->lists();
            Station station{stationName_->text().trimmed(), std::llround(manualFrequency_->value() * 1000000),
                            storedMode(Mode(manualMode_->currentData().toInt()))};
            for (const auto &existing : updated[index].stations) {
                if (existing.frequencyHz == station.frequencyHz && existing.mode == station.mode) {
                    QMessageBox::information(this, "Lista", "Ta częstotliwość i modulacja już są na liście.");
                    return;
                }
            }
            updated[index].stations.append(station);
            if (saveLists(updated, index)) stationName_->clear();
        });
        connect(removeStation_, &QPushButton::clicked, this, [this] {
            const int index = editList_->currentIndex(), row = listTable_->currentRow();
            if (!lists_ || index < 0 || row < 0) return;
            auto updated = lists_->lists();
            updated[index].stations.removeAt(row);
            saveLists(updated, index);
        });
        connect(selectAll_, &QPushButton::clicked, this, [this] {
            for (int row = 0; row < table_->rowCount(); ++row)
                table_->item(row, 0)->setCheckState(Qt::Checked);
        });
        connect(deselectAll_, &QPushButton::clicked, this, [this] {
            for (int row = 0; row < table_->rowCount(); ++row)
                table_->item(row, 0)->setCheckState(Qt::Unchecked);
        });
        connect(saveSelected_, &QPushButton::clicked, this, [this] {
            const int index = editList_->currentIndex();
            if (!lists_ || index < 0) return;
            auto updated = lists_->lists();
            int added = 0, duplicates = 0;
            for (int row = 0; row < table_->rowCount(); ++row) {
                if (table_->item(row, 0)->checkState() != Qt::Checked) continue;
                const qint64 frequency = table_->item(row, 1)->data(Qt::UserRole).toLongLong();
                const QString mode = table_->item(row, 2)->text() == "WFM mono" ? "WFM" : table_->item(row, 2)->text();
                bool exists = false;
                for (const auto &station : updated[index].stations)
                    if (station.frequencyHz == frequency && station.mode == mode) { exists = true; break; }
                if (exists) ++duplicates;
                else { updated[index].stations.append({"", frequency, mode}); ++added; }
            }
            if (!added && !duplicates) {
                QMessageBox::information(this, "Wyniki", "Zaznacz wyniki do zapisania.");
                return;
            }
            if ((!added || saveLists(updated, index)))
                status_->setText(QString("Dodano: %1, pominięto duplikaty: %2").arg(added).arg(duplicates));
        });
        refreshLists(-1);
    }

    bool saveLists(const QVector<StationList> &updated, int index) {
        try {
            lists_->save(updated);
            refreshLists(index);
            return true;
        } catch (const std::exception &error) {
            QMessageBox::warning(this, "Listy", "Nie zapisano zmian: " + QString::fromUtf8(error.what()));
            return false;
        }
    }

    void refreshLists(int index) {
        const QSignalBlocker mainBlock(mainList_), editBlock(editList_);
        mainList_->clear();
        editList_->clear();
        if (lists_) {
            for (const auto &list : lists_->lists()) {
                mainList_->addItem(list.name);
                editList_->addItem(list.name);
            }
            if (index < 0 && !lists_->lists().isEmpty()) index = 0;
        }
        mainList_->setCurrentIndex(index);
        editList_->setCurrentIndex(index);
        newList_->setEnabled(bool(lists_));
        refreshStations();
    }

    void refreshStations() {
        const QSignalBlocker blocker(stationChoice_);
        stationChoice_->clear();
        listTable_->setRowCount(0);
        const int index = editList_->currentIndex();
        const bool hasList = lists_ && index >= 0;
        mainList_->setEnabled(!scanning_ && hasList);
        editList_->setEnabled(hasList);
        deleteList_->setEnabled(hasList);
        addManual_->setEnabled(hasList);
        removeStation_->setEnabled(hasList);
        saveSelected_->setEnabled(hasList);
        if (hasList) {
            for (const auto &station : lists_->lists()[index].stations) {
                const QString frequency = QString::number(station.frequencyHz / 1000000.0, 'f', 6);
                stationChoice_->addItem((station.name.isEmpty() ? "" : station.name + " — ") +
                                        frequency + " MHz " + station.mode);
                const int row = listTable_->rowCount();
                listTable_->insertRow(row);
                listTable_->setItem(row, 0, new QTableWidgetItem(station.name));
                listTable_->setItem(row, 1, new QTableWidgetItem(frequency));
                listTable_->setItem(row, 2, new QTableWidgetItem(station.mode));
            }
        }
        stationChoice_->setCurrentIndex(-1);
        stationChoice_->setPlaceholderText("Wybierz częstotliwość");
        const bool canTune = !scanning_ && stationChoice_->count() > 0;
        stationChoice_->setEnabled(canTune);
        previousStation_->setEnabled(canTune);
        nextStation_->setEnabled(canTune);
    }

    void stepStation(int step) {
        const int count = stationChoice_->count();
        if (!count) return;
        const int current = stationChoice_->currentIndex();
        const int next = current < 0 ? (step > 0 ? 0 : count - 1) : (current + step + count) % count;
        if (next == current) tuneStation(next);
        else stationChoice_->setCurrentIndex(next);
    }

    void tuneStation(int index) {
        if (!lists_ || scanning_ || mainList_->currentIndex() < 0 || index < 0) return;
        const auto &station = lists_->lists()[mainList_->currentIndex()].stations[index];
        const int modeIndex = mode_->findText(station.mode == "WFM" ? "WFM mono" : station.mode);
        if (digitalEnabled_ && station.mode != (voiceMode() == VoiceMode::Off ? "USB" : "NFM"))
            digitalMode_->setCurrentIndex(0);
        if (!digitalEnabled_) mode_->setCurrentIndex(modeIndex);
        frequency_->setValue(station.frequencyHz / 1000000.0);
    }

    ReceiverSettings settings() const {
        ReceiverSettings s;
        s.frequency = std::llround(frequency_->value() * 1000000);
        s.mode = static_cast<Mode>(mode_->currentData().toInt());
        if (scanning_)
            s.mode = static_cast<Mode>(scanMode_->currentData().toInt());
        else if (digitalMode_->currentData().toInt() != int(DigitalMode::Off))
            s.mode = voiceMode() == VoiceMode::Off ? Mode::DigitalUSB : Mode::DigitalVoice;
        s.scan = scanning_;
        s.scanStart = std::llround(start_->value() * 1000000);
        s.scanEnd = std::llround(end_->value() * 1000000);
        s.scanStep = std::llround(step_->value() * 1000);
        s.threshold = threshold_->value();
        s.scanId = scanId_;
        return s;
    }

    void applySettings() {
        const auto s = settings();
        const VoiceMode voice = scanning_ ? VoiceMode::Off : voiceMode();
        if (digital_) digital_->configure(!scanning_ && voice == VoiceMode::Off ?
                                           DigitalMode(digitalMode_->currentData().toInt()) : DigitalMode::Off,
                                           s.frequency);
        if (voice_ && (voice != appliedVoice_ || (voice != VoiceMode::Off && s.frequency != voiceFrequency_))) {
            voiceAudioTimer_->stop();
            voiceAudio_.clear();
            if (audio_ && audioDevice_) {
                audio_->reset();
                audioDevice_ = audio_->start();
                if (!audioDevice_) audioStatus_->setText("Nie udało się ponownie uruchomić wyjścia audio.");
            }
            appliedVoice_ = voice;
            voiceFrequency_ = s.frequency;
            voice_->configure(voice, s.frequency);
        }
        if (receiver_) receiver_->configure(s);
    }

    VoiceMode voiceMode() const {
        const int mode = digitalMode_->currentData().toInt();
        return mode == voiceDmr ? VoiceMode::DMR : mode == voiceYsf ? VoiceMode::YSF : VoiceMode::Off;
    }

    void playAudio(const QByteArray &pcm) {
        if (!audioDevice_) return;
        if (audio_->state() == QAudio::StoppedState) {
            audioStatus_->setText(QString("Wyjście audio zatrzymane (kod %1).").arg(int(audio_->error())));
            audioDevice_ = nullptr;
            return;
        }

        if (audio_->bytesFree() < pcm.size()) {
            if (++audioDrops_ == 20 || voiceMode() != VoiceMode::Off)
                audioStatus_->setText("Ostrzeżenie: przepełnienie bufora audio, część dźwięku pominięto.");
            return;
        }
        if (audioDevice_->write(pcm) != pcm.size()) {
            audioStatus_->setText("Błąd zapisu do wyjścia audio; uruchom program ponownie.");
            audioDevice_ = nullptr;
        } else ++audioBlocks_;
    }

    void flushVoiceAudio() {
        if (!audioDevice_ || voiceAudio_.isEmpty()) {
            voiceAudioTimer_->stop();
            return;
        }
        if (audio_->state() == QAudio::StoppedState) {
            audioStatus_->setText(QString("Wyjście audio zatrzymane (kod %1).").arg(int(audio_->error())));
            audioDevice_ = nullptr;
            voiceAudio_.clear();
            voiceAudioTimer_->stop();
            return;
        }
        const int count = std::min(int(audio_->bytesFree()) / 4 * 4, voiceAudio_.size());
        if (count > 0) {
            const qint64 written = audioDevice_->write(voiceAudio_.constData(), count);
            if (written != count) {
                audioStatus_->setText("Błąd zapisu do wyjścia audio; uruchom program ponownie.");
                audioDevice_ = nullptr;
                voiceAudio_.clear();
                voiceAudioTimer_->stop();
                return;
            }
            voiceAudio_.remove(0, count);
        }
        if (voiceAudio_.isEmpty()) voiceAudioTimer_->stop();
    }

    void addDigitalMessage(const DigitalMessage &message) {
        if (digitalTable_->rowCount() >= 2000) {
            int oldest = 0;
            for (int i = 1; i < digitalTable_->rowCount(); ++i)
                if (digitalTable_->item(i, 0)->data(Qt::UserRole).toLongLong() <
                    digitalTable_->item(oldest, 0)->data(Qt::UserRole).toLongLong()) oldest = i;
            digitalTable_->removeRow(oldest);
        }
        const int row = digitalTable_->rowCount();
        digitalTable_->insertRow(row);
        auto set = [this, row](int column, const QString &value) {
            digitalTable_->setItem(row, column, new QTableWidgetItem(value));
        };
        auto *time = new NumericItem(message.time);
        time->setData(Qt::UserRole, ++messageSequence_);
        digitalTable_->setItem(row, 0, time);
        set(1, message.mode == DigitalMode::FT8 ? "FT8" : message.mode == DigitalMode::FT4 ? "FT4" : "WSPR");
        auto *snr = new NumericItem(QString::number(message.snr));
        snr->setData(Qt::UserRole, message.snr);
        digitalTable_->setItem(row, 2, snr);
        auto *frequency = new NumericItem(QString::number(message.frequencyHz / 1000000.0, 'f', 6));
        frequency->setData(Qt::UserRole, message.frequencyHz);
        digitalTable_->setItem(row, 3, frequency);
        set(4, message.station.isEmpty() ? "—" : message.station);
        std::optional<Country> country;
        if (countries_ && !message.station.isEmpty()) country = countries_->lookup(message.station);
        set(5, country ? country->flag + "  " + country->polishName : "—");
        if (country) {
            digitalTable_->item(row, 5)->setFont(QFont("Noto Color Emoji", 11));
            digitalTable_->item(row, 5)->setToolTip("Kraj lub obszar na podstawie prefiksu znaku");
        }
        set(6, message.grid.isEmpty() ? "—" : message.grid);
        digitalTable_->setItem(row, 7, new NumericItem("—"));
        set(8, message.text);
        updateDistance(row);
        if (sortColumn_ >= 0) {
            sortMessages();
            if (sortOrder_ == Qt::DescendingOrder && (sortColumn_ == 0 || sortColumn_ == 7))
                digitalTable_->scrollToTop();
        }
        else digitalTable_->scrollToBottom();
    }

    void updateDistance(int row) {
        const auto *grid = digitalTable_->item(row, 6);
        auto *distance = digitalTable_->item(row, 7);
        if (!grid || !distance) return;
        auto mine = maidenheadLocation(savedQth_);
        auto theirs = maidenheadLocation(grid->text());
        const QVariant value = mine && theirs ? QVariant(std::llround(distanceKm(*mine, *theirs))) : QVariant();
        distance->setData(Qt::UserRole, value);
        distance->setText(value.isValid() ? value.toString() : "—");
        distance->setToolTip(mine && theirs ? "Odległość między środkami lokatorów " + savedQth_ +
                                              " i " + grid->text() : "Brak jednego z lokatorów");
    }

    void sortMessages() {
        if (sortColumn_ < 0) return;
        digitalTable_->horizontalHeader()->setSortIndicatorShown(true);
        digitalTable_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
        digitalTable_->sortItems(sortColumn_, sortOrder_);
    }

    void addDetection(qint64 freq, double level, const QString &mode) {
        if (table_->rowCount() >= 2000) table_->removeRow(0);
        int row = table_->rowCount();
        table_->insertRow(row);
        auto *check = new QTableWidgetItem;
        check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        check->setCheckState(Qt::Unchecked);
        table_->setItem(row, 0, check);
        auto *item = new QTableWidgetItem(QString::number(freq / 1000000.0, 'f', 6));
        item->setData(Qt::UserRole, freq);
        table_->setItem(row, 1, item);
        table_->setItem(row, 2, new QTableWidgetItem(mode));
        table_->setItem(row, 3, new QTableWidgetItem(QString::number(level, 'f', 1)));
        table_->scrollToBottom();
    }

    void receiveScanDetection(quint64 scanId, qint64 freq, double level, const QString &mode) {
        if (!scanId || scanId != scanId_) return;
        addDetection(freq, level, mode == "WFM" ? "WFM mono" : mode);
    }

    void finishScan(quint64 scanId) {
        if (adsb_ || !scanning_ || scanId != scanId_) return;
        scanning_ = false;
        setScanUi(false);
        applySettings();
    }

    void setScanUi(bool scan) {
        scanButton_->setText(scan ? "Zatrzymaj skanowanie" : "Rozpocznij skanowanie");
        for (QWidget *widget : std::initializer_list<QWidget *>{frequency_, mode_, start_, end_, step_, threshold_})
            widget->setEnabled(!scan);
        scanMode_->setEnabled(!scan);
        adsbStart_->setEnabled(!scan);
        mainList_->setEnabled(!scan && lists_);
        stationChoice_->setEnabled(!scan && stationChoice_->count() > 0);
        previousStation_->setEnabled(!scan && stationChoice_->count() > 0);
        nextStation_->setEnabled(!scan && stationChoice_->count() > 0);
        digitalMode_->setEnabled(!scan && digital_);
    }

    void toggleScan() {
        if (scanning_) {
            scanning_ = false;
            setScanUi(false);
            applySettings();
            return;
        }
        auto s = settings();
        if (s.scanStart > s.scanEnd) {
            QMessageBox::warning(this, "Skanowanie", "Początek zakresu musi być nie większy od końca.");
            return;
        }
        auto steps = (s.scanEnd - s.scanStart) / s.scanStep + 1;
        if (steps > 10000 &&
            QMessageBox::question(this, "Długie skanowanie",
                QString("To %1 kroków; samo oczekiwanie potrwa co najmniej %2 minut. Kontynuować?")
                    .arg(steps).arg(steps / 240.0, 0, 'f', 1)) != QMessageBox::Yes) return;
        ++scanId_;
        table_->setRowCount(0);
        scanning_ = true;
        setScanUi(true);
        applySettings();
    }

    Receiver *receiver_ = nullptr;
    QAudioOutput *audio_ = nullptr;
    QIODevice *audioDevice_ = nullptr;
    QDoubleSpinBox *frequency_, *start_, *end_, *step_, *threshold_, *manualFrequency_;
    QComboBox *mode_, *scanMode_, *manualMode_, *mainList_, *editList_, *stationChoice_;
    QSlider *volume_;
    QProgressBar *meter_;
    QLabel *signal_, *status_, *audioStatus_;
    QPushButton *scanButton_, *mute_, *previousStation_, *nextStation_, *newList_, *deleteList_;
    QPushButton *addManual_, *removeStation_, *selectAll_, *deselectAll_, *saveSelected_;
    QPushButton *newest_, *farthest_, *clearMessages_;
    QPushButton *adsbStart_, *saveAdsbPosition_;
    QLineEdit *stationName_;
    QLineEdit *adsbLatitude_, *adsbLongitude_;
    QTableWidget *table_, *listTable_;
    QTableWidget *aircraftTable_;
    QTableWidget *digitalTable_;
    QTabWidget *pages_;
    QComboBox *digitalMode_;
    QLineEdit *qth_;
    QLabel *digitalStatus_;
    QLabel *voiceMetadata_;
    QLabel *clockStatus_;
    QLabel *adsbStatus_;
    DigitalDecoder *digital_ = nullptr;
    VoiceDecoder *voice_ = nullptr;
    QTimer *voiceAudioTimer_ = nullptr;
    QByteArray voiceAudio_;
    VoiceMode appliedVoice_ = VoiceMode::Off;
    qint64 voiceFrequency_ = 0;
    AdsbDecoder *adsb_ = nullptr;
    QString adsbExecutable_;
    int adsbUpdates_ = 0;
    QProcess *clock_ = nullptr;
    QTimer *clockTimer_ = nullptr;
    std::unique_ptr<Countries> countries_;
    std::unique_ptr<QTemporaryDir> testListsDirectory_;
    std::unique_ptr<StationLists> lists_;
    bool digitalEnabled_ = false;
    bool digitalComplete_ = false;
    bool invalidQth_ = false;
    QString savedQth_;
    std::optional<Location> adsbQth_;
    int previousMode_ = 0;
    int savedDigitalMode_ = 0;
    int aircraftSortColumn_ = 5;
    Qt::SortOrder aircraftSortOrder_ = Qt::AscendingOrder;
    int sortColumn_ = 0;
    Qt::SortOrder sortOrder_ = Qt::DescendingOrder;
    qint64 messageSequence_ = 0;
    bool scanning_ = false;
    quint64 scanId_ = 0;
    bool receivedIq_ = false;
    bool signalSeen_ = false;
    int audioBlocks_ = 0;
    int audioDrops_ = 0;
    int voiceInputBlocks_ = 0;
};

int hardwareTest(QApplication &app) {
    Receiver receiver(dataFile());
    ReceiverSettings settings;
    receiver.configure(settings);
    int blocks = 0;
    int phase = 0;
    QObject::connect(&receiver, &Receiver::failed, &app, [&app](const QString &message) {
        std::cerr << "Hardware error: " << message.toStdString() << '\n';
        app.exit(1);
    });
    QObject::connect(&receiver, &Receiver::blockReceived, &app, [&] {
        if (++blocks == 10 && phase == 0) {
            settings.frequency = 100100000;
            settings.mode = Mode::WFM;
            receiver.configure(settings);
            phase = 1;
            blocks = 0;
            std::cout << "Received IQ at 7.1 MHz; tuning to 100.1 MHz\n";
        } else if (blocks == 10 && phase == 1) {
            std::cout << "Received IQ after retuning to 100.1 MHz\n";
            app.exit(0);
        }
    });
    QTimer::singleShot(15000, &app, [&app] {
        std::cerr << "Hardware test timed out\n";
        app.exit(1);
    });
    receiver.start();
    int result = app.exec();
    receiver.stop();
    return result;
}

int scanTest(QApplication &app, Mode mode = Mode::LSB, bool fmBand = false, qint64 singleFrequency = 0) {
    QTemporaryDir temp;
    if (!temp.isValid()) {
        std::cerr << "Cannot create temporary scan directory\n";
        return 1;
    }
    const QString path = temp.filePath("results.csv");
    Receiver receiver(path);
    ReceiverSettings settings;
    settings.scan = true;
    settings.mode = mode;
    settings.scanStart = singleFrequency ? singleFrequency : fmBand ? 88000000 : 7100000;
    settings.scanEnd = singleFrequency ? singleFrequency : fmBand ? 108000000 : 7102000;
    settings.scanStep = fmBand ? 100000 : 1000;
    settings.threshold = fmBand ? -60 : -200;
    settings.scanId = 123;
    receiver.configure(settings);
    int detections = 0;
    int resumedAudio = 0;
    bool scanDone = false;
    QObject::connect(&receiver, &Receiver::audioReady, &app, [&](const QByteArray &, qint64) {
        if (scanDone) ++resumedAudio;
    });
    QObject::connect(&receiver, &Receiver::detected, &app, [&](quint64 scanId, qint64, double, const QString &detectedMode) {
        if (scanId != settings.scanId || detectedMode != storedMode(mode)) {
            std::cerr << "Incorrect scan identity or modulation\n";
            app.exit(1);
        }
        ++detections;
    });
    QObject::connect(&receiver, &Receiver::failed, &app, [&app](const QString &message) {
        std::cerr << "Scan error: " << message.toStdString() << '\n';
        app.exit(1);
    });
    QObject::connect(&receiver, &Receiver::scanFinished, &app, [&](quint64 scanId) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::cerr << "Scan results were not saved\n";
            app.exit(1);
            return;
        }
        const auto lines = file.readAll().trimmed().split('\n');
        const int steps = int((settings.scanEnd - settings.scanStart) / settings.scanStep + 1);
        if (scanId != settings.scanId || detections > steps || lines.size() != detections + 1 ||
            lines[0] != "utc,frequency_hz,mode,peak_dbfs") {
            std::cerr << "Unexpected scan result shape\n";
            app.exit(1);
            return;
        }
        qint64 previous = 0;
        for (int i = 1; i < lines.size(); ++i) {
            const auto columns = lines[i].split(',');
            bool validFreq = false, validLevel = false;
            const qint64 frequency = columns.size() == 4 ? columns[1].toLongLong(&validFreq) : 0;
            if (columns.size() != 4 || !validFreq || frequency < settings.scanStart ||
                frequency > settings.scanEnd || (frequency - settings.scanStart) % settings.scanStep != 0 ||
                frequency <= previous || columns[2] != storedMode(mode).toUtf8() || !std::isfinite(columns[3].toDouble(&validLevel)) ||
                !validLevel || QDateTime::fromString(QString::fromUtf8(columns[0]), Qt::ISODateWithMs).isNull()) {
                std::cerr << "Invalid scan result row " << i << '\n';
                app.exit(1);
                return;
            }
            if (fmBand) std::cout << "FM candidate MHz: " << frequency / 1000000.0 << '\n';
            previous = frequency;
        }
        std::cout << steps << " scan steps completed; " << detections << " detections persisted with expected CSV shape\n";
        scanDone = true;
        settings.scan = false;
        receiver.configure(settings);
        QTimer::singleShot(2000, &app, [&] {
            if (resumedAudio < 10) {
                std::cerr << "Receiver audio did not resume after scan\n";
                app.exit(1);
            } else {
                std::cout << "Receiver audio resumed after scan\n";
                app.exit(0);
            }
        });
    });
    QTimer::singleShot(fmBand ? 150000 : 15000, &app, [&app] {
        std::cerr << "Scan test timed out\n";
        app.exit(1);
    });
    receiver.start();
    int result = app.exec();
    receiver.stop();
    return result;
}

int audioTest(QApplication &app) {
    QAudioFormat format;
    format.setSampleRate(kAudioRate);
    format.setChannelCount(2);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);
    const auto device = QAudioDeviceInfo::defaultOutputDevice();
    if (device.isNull() || !device.isFormatSupported(format)) {
        std::cerr << "No compatible default audio output\n";
        return 1;
    }
    QAudioOutput output(device, format);
    auto *stream = output.start();
    if (!stream) {
        std::cerr << "Could not start audio output\n";
        return 1;
    }
    const QByteArray silence(kAudioRate * 4 / 4, '\0');
    if (stream->write(silence) != silence.size()) {
        std::cerr << "Could not write audio buffer\n";
        return 1;
    }
    QTimer::singleShot(100, &app, [&] {
        if (output.error() != QAudio::NoError) {
            std::cerr << "Audio output error: " << int(output.error()) << '\n';
            app.exit(1);
        } else {
            std::cout << "Sent silence to audio device: " << device.deviceName().toStdString() << '\n';
            app.exit(0);
        }
    });
    return app.exec();
}

int keyboardTest() {
    QLineEdit text;
    text.setText("STARA");
    text.show();
    bool textDialog = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "touchKeyboard") return;
        auto *input = dialog->findChild<QLineEdit *>("keyboardInput");
        auto *accept = dialog->findChild<QPushButton *>("keyboardAccept");
        if (!input || !accept) { dialog->reject(); return; }
        input->clear();
        for (const QString &character : {QString("N"), QString("O")}) {
            QPushButton *found = nullptr;
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->text() == character) { found = button; break; }
            if (!found) { dialog->reject(); return; }
            found->click();
        }
        if (input->text() != "NO") { dialog->reject(); return; }
        input->setText("NOWA LISTA");
        const QString path = qEnvironmentVariable("SDR_TOUCH_SCREENSHOT");
        if (!path.isEmpty() && !dialog->grab().save(path)) { dialog->reject(); return; }
        textDialog = dialog->width() <= 1024 && dialog->height() <= 600;
        accept->click();
    });
    QMouseEvent click(QEvent::MouseButtonRelease, QPointF(2, 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&text, &click);
    if (!textDialog || text.text() != "NOWA LISTA") return 1;
    QDoubleSpinBox number;
    number.setRange(1, 200);
    number.setDecimals(3);
    number.setValue(7.1);
    number.show();
    auto *line = number.findChild<QLineEdit *>();
    if (!line) return 1;
    bool numericDialog = false;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *input = dialog->findChild<QLineEdit *>("keyboardInput");
        auto *accept = dialog->findChild<QPushButton *>("keyboardAccept");
        if (!input || !accept) { dialog->reject(); return; }
        input->setText("999");
        accept->click();
        if (!dialog->isVisible()) return;
        input->setText(number.locale().toString(14.074, 'f', 3));
        numericDialog = true;
        accept->click();
    });
    QMouseEvent numberClick(QEvent::MouseButtonRelease, QPointF(2, 2), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(line, &numberClick);
    if (!numericDialog || std::abs(number.value() - 14.074) > 0.0001) return 1;
    QInputDialog nameDialog;
    nameDialog.setInputMode(QInputDialog::TextInput);
    nameDialog.setLabelText("Nazwa");
    nameDialog.show();
    nameDialog.activateWindow();
    auto *nameInput = nameDialog.findChild<QLineEdit *>();
    if (!nameInput) return 1;
    nameInput->setFocus(Qt::OtherFocusReason);
    QEventLoop loop;
    bool automatic = false;
    QTimer::singleShot(100, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog && dialog->objectName() == "touchKeyboard") {
            auto *input = dialog->findChild<QLineEdit *>("keyboardInput");
            auto *accept = dialog->findChild<QPushButton *>("keyboardAccept");
            if (input && accept) {
                input->setText("AUTOMATYCZNIE");
                automatic = true;
                accept->click();
            } else dialog->reject();
        }
        loop.quit();
    });
    loop.exec();
    if (!automatic || nameDialog.textValue() != "AUTOMATYCZNIE") {
        std::cerr << "Input-dialog keyboard test failed: automatic=" << automatic
                  << ", focused=" << nameInput->hasFocus()
                  << ", marked=" << nameInput->property("autoKeyboardOpened").toBool() << '\n';
        return 1;
    }
    std::cout << "Touch keyboard text and numeric input tests passed\n";
    return 0;
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    TouchKeyboard keyboard(&app);
    app.installEventFilter(&keyboard);
    QCoreApplication::setOrganizationName("SDRTouch");
    QCoreApplication::setApplicationName("SDRTouch");
    const QStringList args = app.arguments();
    if (args.contains("--install-digital-lists")) {
        try {
            const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                     .filePath("listy-czestotliwosci.json");
            StationLists lists(path);
            auto updated = lists.lists();
            const int count = addDigitalLists(updated);
            if (count) lists.save(updated);
            std::cout << "Added " << count << " digital lists; existing lists were preserved\n";
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "Cannot install digital lists: " << error.what() << '\n';
            return 1;
        }
    }
    if (args.contains("--keyboard-test")) return keyboardTest();
    if (args.contains("--hardware-test")) return hardwareTest(app);
    if (args.contains("--scan-test")) return scanTest(app);
    if (args.contains("--scan-am-test")) return scanTest(app, Mode::AM);
    if (args.contains("--scan-fm-test")) return scanTest(app, Mode::WFM, true);
    if (args.contains("--scan-fm-frequency-test")) {
        const int index = args.indexOf("--scan-fm-frequency-test");
        bool ok = false;
        const double mhz = index + 1 < args.size() ? args.at(index + 1).toDouble(&ok) : 0;
        if (!ok || !std::isfinite(mhz) || mhz < 0.01 || mhz > 2000) {
            std::cerr << "Provide a valid frequency in MHz\n";
            return 1;
        }
        return scanTest(app, Mode::WFM, true, std::llround(mhz * 1000000));
    }
    if (args.contains("--audio-test")) return audioTest(app);
    if (args.contains("--adsb-failure-test")) {
        QTemporaryDir testSettings;
        if (!testSettings.isValid()) return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettings.path());
        Window window(false, true, "/bin/false");
        window.show();
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, &app, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) box->accept();
        });
        dismiss.start(100);
        QTimer::singleShot(2000, &app, [&] { window.startAdsbSmoke(true); });
        QTimer::singleShot(32000, &app, [&] {
            if (!window.adsbFailureRestored() || !window.digitalSmokeSucceeded()) {
                std::cerr << "Receiver did not recover from ADS-B process failure\n";
                app.exit(1);
            } else {
                std::cout << "Receiver and FT4 recovered after ADS-B process failure\n";
                app.exit(0);
            }
        });
        return app.exec();
    }
    if (args.contains("--adsb-hardware-test")) {
        const int index = args.indexOf("--adsb-hardware-test");
        if (index + 1 >= args.size()) {
            std::cerr << "Provide the readsb executable path\n";
            return 1;
        }
        QTemporaryDir testSettings;
        if (!testSettings.isValid()) return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettings.path());
        Window window(false, true, args.at(index + 1));
        window.show();
        QTimer::singleShot(2000, &app, [&] { window.startAdsbSmoke(false); });
        QTimer::singleShot(9000, &app, [&] {
            if (!window.stopAdsbSmoke(false)) {
                std::cerr << "ADS-B normal receiver transition failed\n";
                app.exit(1);
            }
        });
        QTimer::singleShot(12000, &app, [&] { window.startAdsbSmoke(true); });
        QTimer::singleShot(19000, &app, [&] {
            if (!window.stopAdsbSmoke(true)) {
                std::cerr << "ADS-B FT4 transition failed\n";
                app.exit(1);
            }
        });
        QTimer::singleShot(49000, &app, [&] {
            if (!window.digitalSmokeSucceeded()) {
                std::cerr << "FT4 did not resume after ADS-B\n";
                app.exit(1);
            } else {
                std::cout << "ADS-B and normal/FT4 receiver transitions passed; live aircraft positions not asserted\n";
                app.exit(0);
            }
        });
        return app.exec();
    }
    if (args.contains("--voice-smoke-test")) {
        const int index = args.indexOf("--voice-smoke-test");
        if (index + 1 >= args.size()) return 1;
        QTemporaryDir testSettings;
        if (!testSettings.isValid()) return 1;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettings.path());
        Window window(false, true, {}, args.at(index + 1));
        window.show();
        QTimer::singleShot(2000, &app, [&] { window.startVoiceSmoke(VoiceMode::DMR); });
        QTimer::singleShot(8000, &app, [&] {
            if (!window.voiceSmokeSucceeded(VoiceMode::DMR)) {
                std::cerr << "DMR receiver stream failed\n";
                app.exit(1);
            } else window.startVoiceAdsbSmoke();
        });
        QTimer::singleShot(14000, &app, [&] {
            if (!window.stopVoiceAdsbSmoke()) {
                std::cerr << "DMR to ADS-B transition failed\n";
                app.exit(1);
            }
        });
        QTimer::singleShot(18000, &app, [&] {
            if (!window.voiceSmokeSucceeded(VoiceMode::DMR)) {
                std::cerr << "DMR did not resume after ADS-B\n";
                app.exit(1);
            } else window.startVoiceSmoke(VoiceMode::YSF);
        });
        QTimer::singleShot(24000, &app, [&] {
            if (!window.voiceSmokeSucceeded(VoiceMode::YSF) || !window.finishVoiceSmoke()) {
                std::cerr << "YSF receiver stream or transition failed\n";
                app.exit(1);
            } else {
                std::cout << "DMR, ADS-B and YSF receiver transitions passed; no live voice asserted\n";
                app.exit(0);
            }
        });
        return app.exec();
    }
    if (args.contains("--digital-smoke-test")) {
        Window window(false, true);
        window.show();
        window.startDigitalSmoke();
        QTimer::singleShot(26000, &app, [&] {
            if (!window.digitalSmokeSucceeded()) {
                std::cerr << "Live IQ-to-digital decoder test failed\n";
                app.exit(1);
            } else {
                std::cout << "Live IQ reached the FT4 decoder; station reception not asserted\n";
                app.exit(0);
            }
        });
        return app.exec();
    }
    if (args.contains("--smoke-test")) {
        Window window(false, true);
        window.showMaximized();
        QTimer::singleShot(4000, &app, [&] {
            if (!window.smokeSucceeded()) {
                std::cerr << "Integrated IQ, GUI and audio test failed\n";
                app.exit(1);
            } else {
                std::cout << "Integrated IQ, GUI and audio test passed\n";
                app.exit(0);
            }
        });
        return app.exec();
    }
    if (args.contains("--ui-test") || args.contains("--voice-ui-test") || args.contains("--ui-scan-test") || args.contains("--sort-test") ||
        args.contains("--adsb-ui-test")) {
        std::unique_ptr<QTemporaryDir> testSettings;
        if (args.contains("--adsb-ui-test")) {
            testSettings = std::make_unique<QTemporaryDir>();
            if (!testSettings->isValid()) return 1;
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, testSettings->path());
        }
        Window window(true);
        if (args.contains("--voice-ui-test")) window.voiceUiTest();
        if (args.contains("--adsb-ui-test") && !window.adsbUiTest()) {
            std::cerr << "ADS-B UI test failed\n";
            return 1;
        }
        if (args.contains("--sort-test") && !window.digitalSortTest()) {
            std::cerr << "Digital message sorting test failed\n";
            return 1;
        }
        if (args.contains("--ui-scan-test") && !window.listUiTest()) {
            std::cerr << "Station list UI test failed\n";
            return 1;
        }
        window.show();
        QTimer::singleShot(500, &app, [&] {
            const auto screenshot = qEnvironmentVariable("SDR_TOUCH_SCREENSHOT");
            if (!screenshot.isEmpty() && !window.grab().save(screenshot)) {
                std::cerr << "Could not save UI screenshot\n";
                app.exit(1);
            } else if (window.width() > 1024 || window.height() > 600) {
                std::cerr << "UI exceeds 1024x600: " << window.width() << 'x' << window.height() << '\n';
                app.exit(1);
            } else {
                std::cout << "UI fits within 1024x600\n";
                app.exit(0);
            }
        });
        return app.exec();
    }
    Window window;
    window.showMaximized();
    return app.exec();
}
