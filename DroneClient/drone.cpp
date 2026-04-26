#include <QCoreApplication>
#include "drone.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QRandomGenerator>
#include <QHostInfo>
#include <cmath>
#include <QFile>
#include <QTextStream>

Drone::Drone(QWidget *parent) : QMainWindow(parent) {
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *mainLayout = new QVBoxLayout(central);

    QHBoxLayout *connectLayout = new QHBoxLayout();
    connectLayout->addWidget(new QLabel("Сервер:"));
    ipInput = new QLineEdit("127.0.0.1");
    connectLayout->addWidget(ipInput);
    connectLayout->addWidget(new QLabel("Порт:"));
    portInput = new QLineEdit("8888");
    portInput->setFixedWidth(80);
    connectLayout->addWidget(portInput);
    connectButton = new QPushButton("Подключиться");
    connectLayout->addWidget(connectButton);
    mainLayout->addLayout(connectLayout);

    QHBoxLayout *snrLayout = new QHBoxLayout();
    snrLayout->addWidget(new QLabel("SNR (дБ):"));
    snrSlider = new QSlider(Qt::Horizontal);
    snrSlider->setRange(0, 40);  // 0-40 дБ
    snrSlider->setValue(20);      // начальное значение 20 дБ
    snrSlider->setFixedWidth(200);
    snrLayout->addWidget(snrSlider);

    snrLabel = new QLabel("20 dB");
    snrLayout->addWidget(snrLabel);

    mainLayout->insertLayout(1, snrLayout);  // вставить после connectLayout

    // Подключаем обработчик слайдера
    connect(snrSlider, &QSlider::valueChanged, this, [this](int value) {
        currentSNR = value;
        snrLabel->setText(QString("%1 dB").arg(value));
        log(QString("🎛️ SNR изменен вручную: %1 дБ").arg(value));
    });

    logDisplay = new QTextEdit();
    logDisplay->setReadOnly(true);
    mainLayout->addWidget(logDisplay);

    socket = new QUdpSocket(this);

    telemetryTimer = new QTimer(this);
    photoTimer = new QTimer(this);
    motionTimer = new QTimer(this);

    connect(connectButton, &QPushButton::clicked, this, &Drone::onConnectClicked);
    connect(telemetryTimer, &QTimer::timeout, this, &Drone::sendTelemetry);
    connect(photoTimer, &QTimer::timeout, this, &Drone::sendPhoto);
    connect(motionTimer, &QTimer::timeout, this, &Drone::updatePosition);
    connect(socket, &QUdpSocket::readyRead, this, &Drone::processFeedback);

    seqNumTelemetry = 0;
    seqNumPhoto = 0;
    fecPacketId = 0;
    photoRate = AdaptationParams::PHOTO_RATE_LOW;
    fecEnabled = false;

    latitude = 55.7558;
    longitude = 37.6176;
    altitude = 100.0;
    speed = 10.0;
    battery = 100.0;
    lastPhotoState = "OFF";
    stats.lossLow = AdaptationParams::LOSS_MID_LOW;
    stats.lossHigh = AdaptationParams::LOSS_BAD;

    priorityPhotoTimer = new QTimer(this);
    seqNumPriorityPhoto = 0;
    priorityPhotoRate = 0.2;  // 0.2 Гц (1 раз в 5 секунд) - реже обычных фото
    priorityPhotoEnabled = true;

    connect(priorityPhotoTimer, &QTimer::timeout, this, &Drone::sendPriorityPhoto);

    connected = false;

    csvFile.setFileName("drone_adaptation_log.csv");
    if (csvFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&csvFile);
        stream << "timestamp;loss_percent;telemetry_rate_hz;photo_rate_hz;priority_photo_rate_hz;"
               << "loss_low;loss_high;photo_delivered;"
               << "telemetry_sent;telemetry_delivered;"
               << "current_photo_state;priority_photo_state;fec_enabled\n";
        stream.flush();
        log("✅ CSV файл создан");
    } else {
        log("❌ ОШИБКА: Не удалось создать CSV файл!");
    }
    log("Дрон инициализирован. Введите IP сервера и нажмите Подключиться");
}


void Drone::onConnectClicked() {
    serverIp = ipInput->text();
    serverPort = portInput->text().toUShort();

    if (serverIp.isEmpty() || serverPort == 0) {
        log("❌ Ошибка: введите корректный IP и порт");
        return;
    }

    connected = true;
    log("✅ Подключен к серверу " + serverIp + ":" + QString::number(serverPort));

    updateTimers();
    motionTimer->start(100);
    log("🚁 Дрон начал передачу данных");
}

void Drone::sendTelemetryWithFEC() {
    DroneState state;
    state.latitude = latitude;
    state.longitude = longitude;
    state.altitude = altitude;
    state.speed = speed;
    state.battery = battery;

    QByteArray telemetryData = state.toBytes();

    // Отправляем оригинал (фрагмент 0)
    FECPacket dataPkt;
    dataPkt.packetId = fecPacketId;
    dataPkt.fragId = 0;
    dataPkt.data = telemetryData;
    dataPkt.isParity = false;

    Packet p1;
    p1.type = PT_TELEMETRY_FEC;
    p1.seqNum = seqNumTelemetry++;
    p1.timestamp = QDateTime::currentMSecsSinceEpoch();
    p1.priority = 255;
    p1.data = dataPkt.toBytes();
    socket->writeDatagram(p1.toBytes(), QHostAddress(serverIp), serverPort);

    // Отправляем дубликат как "чётность" (фрагмент 1)
    FECPacket parityPkt;
    parityPkt.packetId = fecPacketId;
    parityPkt.fragId = 1;
    parityPkt.data = telemetryData;  // дубликат
    parityPkt.isParity = true;

    Packet p2;
    p2.type = PT_TELEMETRY_FEC;
    p2.seqNum = seqNumTelemetry++;
    p2.timestamp = QDateTime::currentMSecsSinceEpoch();
    p2.priority = 255;
    p2.data = parityPkt.toBytes();
    socket->writeDatagram(p2.toBytes(), QHostAddress(serverIp), serverPort);

    log(QString("📤 [FEC] Телеметрия группа %1: отправлены дубликаты (защита от потерь)")
        .arg(fecPacketId));

    fecPacketId++;
}


void Drone::sendPriorityPhoto() {
    if (!connected || !priorityPhotoEnabled) return;

    // Приоритетное фото меньше по размеру, но важнее
    QByteArray photoData;
    photoData.resize(AdaptationParams::PHOTO_PRIORITY_SIZE);
    for (int i = 0; i < photoData.size(); ++i) {
        photoData[i] = QRandomGenerator::global()->bounded(256);
    }

    Packet p;
    p.type = PT_PHOTO_PRIORITY;
    p.seqNum = seqNumPriorityPhoto++;
    p.timestamp = QDateTime::currentMSecsSinceEpoch();
    p.priority = 200;  // высокий приоритет (выше обычного фото)
    p.data = photoData;

    socket->writeDatagram(p.toBytes(), QHostAddress(serverIp), serverPort);

    log(QString("🔥 ПРИОРИТЕТНОЕ ФОТО #%1 | Размер: %2 байт")
        .arg(p.seqNum).arg(photoData.size()));
}

void Drone::sendTelemetry() {
    if (!connected) return;

    stats.telemetrySent++;
    if (fecEnabled) {
        sendTelemetryWithFEC();
    } else {
        // Обычная отправка без FEC
        DroneState state;
        state.latitude = latitude;
        state.longitude = longitude;
        state.altitude = altitude;
        state.speed = speed;
        state.battery = battery;

        Packet p;
        p.type = PT_TELEMETRY;
        p.seqNum = seqNumTelemetry++;
        p.timestamp = QDateTime::currentMSecsSinceEpoch();
        p.priority = 255;
        p.data = state.toBytes();

        QByteArray data = p.toBytes();
        socket->writeDatagram(data, QHostAddress(serverIp), serverPort);

        log(QString("📤 Телеметрия #%1 (без FEC)").arg(p.seqNum));
    }
}
void Drone::sendPhoto() {
    if (!connected || photoRate == 0) return;

    // Обычное фото без FEC
    QByteArray photoData;
    photoData.resize(AdaptationParams::PHOTO_SIZE);
    for (int i = 0; i < photoData.size(); ++i) {
        photoData[i] = QRandomGenerator::global()->bounded(256);
    }

    Packet p;
    p.type = PT_PHOTO;
    p.seqNum = seqNumPhoto++;
    p.timestamp = QDateTime::currentMSecsSinceEpoch();
    p.priority = 100;
    p.data = photoData;

    socket->writeDatagram(p.toBytes(), QHostAddress(serverIp), serverPort);
    stats.photoDelivered++;

    log(QString("📸 Фото #%1 | Размер: %2 байт | Частота: %3 Гц")
        .arg(p.seqNum).arg(photoData.size()).arg(photoRate));
}

void Drone::processFeedback() {
    while (socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;
        socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        Packet p = Packet::fromBytes(datagram);

        if (p.type == PT_FEEDBACK) {
                   QDataStream stream(p.data);
                   double packetLoss;
                   quint32 recoveredCount;
                             quint32 realLost;
                             quint32 realReceived;

                             stream >> packetLoss;
                             stream >> recoveredCount;
                             stream >> realLost;
                             stream >> realReceived;

                             // Сохраняем в статистику
                             stats.telemetryRecovered = recoveredCount;
                             stats.telemetryLost = realLost;
                             stats.telemetryReceived = realReceived;

                   log(QString("📡 Обратная связь: Loss=%1% | Потеряно реально: %2 | Получено: %3 | Восстановлено: %4")
                                  .arg(packetLoss, 0, 'f', 1)
                                  .arg(realLost)
                                  .arg(realReceived)
                                  .arg(recoveredCount));

                              if (packetLoss == -1) {
                                  packetLoss = lossPercent;
                              } else {lossPercent = packetLoss;}

                              adaptProtocol(packetLoss);

                          }

                      }
  }
void Drone::updatePosition() {
    static double angle = 0;
    angle += 0.05;
    if (angle > 2 * M_PI) angle -= 2 * M_PI;

    double radius = 0.01;
    latitude = 55.7558 + radius * cos(angle);
    longitude = 37.6176 + radius * sin(angle);
    altitude = 100 + 50 * sin(angle * 2);
    speed = 10 + 5 * sin(angle);
    battery -= 0.01;
    if (battery < 0) battery = 100;
}

Drone::~Drone() {
    if (csvFile.isOpen()) {
        csvFile.close();
        qDebug() << "CSV файл закрыт";
    }
}

void Drone::adaptProtocol(double lossPercent) {
    double newPhotoRate = photoRate;
    bool newFecEnabled = fecEnabled;
    bool newPriorityPhotoEnabled = priorityPhotoEnabled;
    bool changed = false;

    // Пороги гистерезиса для Loss
    const double LOSS_OFF = 10.0;   // выключаем фото при ≥10%
    const double LOSS_ON = 5.0;     // включаем фото при ≤5%

    // Пороги для SNR
    const double SNR_BAD = 10.0;
    const double SNR_RECOVERY = 12.0;
    bool snrIsBad_abs = (currentSNR < SNR_BAD);
    bool snrIsGood = (currentSNR >= SNR_RECOVERY);

    // SNR с гистерезисом
    bool snrIsBad;
    if (previousSnrWasBad) {
        snrIsBad = !snrIsGood;
        if (snrIsGood) {
            previousSnrWasBad = false;
            log(QString("🔄 SNR восстановился до %1 дБ ≥ %2 дБ")
                .arg(currentSNR, 0, 'f', 1)
                .arg(SNR_RECOVERY, 0, 'f', 1));
        }
    } else {
        snrIsBad = snrIsBad_abs;
        if (snrIsBad_abs) {
            previousSnrWasBad = true;
            log(QString("⚠️ SNR упал до %1 дБ < %2 дБ")
                .arg(currentSNR, 0, 'f', 1)
                .arg(SNR_BAD, 0, 'f', 1));
        }
    }

    // ПРИОРИТЕТНЫЕ ФОТО (зависит только от Loss)
    if (lossPercent >= 30.0) {
        newPriorityPhotoEnabled = false;
        priorityPhotoRate = 0.0;
        log("⚠️ КРИТИЧЕСКИЙ КАНАЛ: приоритетные фото ВЫКЛ");
    } else if (lossPercent >= 20.0) {
        newPriorityPhotoEnabled = true;
        priorityPhotoRate = 0.1;
        log("📡 Плохой канал: приоритетные фото 0.1 Гц");
    } else if (lossPercent >= 10.0) {
        newPriorityPhotoEnabled = true;
        priorityPhotoRate = 0.2;
        log("📡 Средний канал: приоритетные фото 0.2 Гц");
    } else {
        newPriorityPhotoEnabled = true;
        priorityPhotoRate = 0.5;
        log("✅ Хороший канал: приоритетные фото 0.5 Гц");
    }

    if (newPriorityPhotoEnabled != priorityPhotoEnabled) {
        priorityPhotoEnabled = newPriorityPhotoEnabled;
        changed = true;
    }

    // ОСНОВНАЯ АДАПТАЦИЯ С ГИСТЕРЕЗИСОМ

    // FEC включается при loss >= 5% ИЛИ при плохом SNR
    bool needFec = (lossPercent >= 5.0) || snrIsBad;

    // ФОТО С ГИСТЕРЕЗИСОМ (запоминаем состояние)
    static bool lastPhotoWasOn = true;  // предыдущее состояние фото

    bool shouldPhotoBeOn;

    if (lastPhotoWasOn) {
        // Фото было включено - выключаем при loss >= 10% ИЛИ плохом SNR
        if (lossPercent >= LOSS_OFF || snrIsBad) {
            shouldPhotoBeOn = false;
            lastPhotoWasOn = false;
            log(QString("📸 Фото ВЫКЛ: loss=%1% ≥ %2% или SNR=%3 дБ (плохой)")
                .arg(lossPercent, 0, 'f', 1)
                .arg(LOSS_OFF, 0, 'f', 1)
                .arg(currentSNR, 0, 'f', 1));
        } else {
            shouldPhotoBeOn = true;
        }
    } else {
        // Фото было выключено - включаем только при loss <= 5% И хорошем SNR
        if (lossPercent <= LOSS_ON && !snrIsBad) {
            shouldPhotoBeOn = true;
            lastPhotoWasOn = true;
            log(QString("✅ Фото ВКЛ: loss=%1% ≤ %2% и SNR=%3 дБ (хороший)")
                .arg(lossPercent, 0, 'f', 1)
                .arg(LOSS_ON, 0, 'f', 1)
                .arg(currentSNR, 0, 'f', 1));
        } else {
            shouldPhotoBeOn = false;
        }
    }

    // Применяем решения
    newPhotoRate = shouldPhotoBeOn ? AdaptationParams::PHOTO_RATE_LOW : AdaptationParams::PHOTO_RATE_OFF;
    newFecEnabled = needFec;

    if (photoRate != newPhotoRate || fecEnabled != newFecEnabled) {
        changed = true;
        if (photoRate != newPhotoRate) {
            stats.numSwitches++;
        }
    }

    if (changed) {
        photoRate = newPhotoRate;
        fecEnabled = newFecEnabled;
        updateTimers();

        log(QString("📊 Состояние: фото=%1, FEC=%2, Loss=%3%, SNR=%4 дБ")
            .arg(photoRate > 0 ? "ВКЛ" : "ВЫКЛ")
            .arg(fecEnabled ? "ВКЛ" : "ВЫКЛ")
            .arg(lossPercent, 0, 'f', 1)
            .arg(currentSNR, 0, 'f', 1));
    }

    logToCSV(lossPercent);

    static int callCount = 0;
    if (++callCount % 10 == 0) {
        log(QString("📊 СТАТИСТИКА: Loss=%1% | Переключений=%2 | Фото=%3 | Восстановлений=%4")
            .arg(stats.getTelemetryLossPercent(), 0, 'f', 1)
            .arg(stats.numSwitches)
            .arg(stats.photoDelivered)
            .arg(stats.telemetryRecovered));
    }
}

void Drone::logToCSV(double lossPercent) {
    if (csvFile.isOpen()) {
        QTextStream stream(&csvFile);

        stream << QDateTime::currentMSecsSinceEpoch() << ";"
               << QString::number(lossPercent, 'f', 2).replace('.', ',') << ";"
               << "1,0" << ";"
               << QString::number(photoRate, 'f', 2).replace('.', ',') << ";"
               << QString::number(priorityPhotoRate, 'f', 2).replace('.', ',') << ";"
               << QString::number(stats.lossLow, 'f', 1).replace('.', ',') << ";"
               << QString::number(stats.lossHigh, 'f', 1).replace('.', ',') << ";"
               << stats.photoDelivered << ";"
               << stats.telemetrySent << ";"
               << stats.telemetryReceived << ";"
               << (photoRate > 0 ? "ON" : "OFF") << ";"
               << (priorityPhotoEnabled ? "ON" : "OFF") << ";"
               << (fecEnabled ? "ON" : "OFF") << "\n";
        stream.flush();
    }
}

void Drone::updateTimers() {
    telemetryTimer->setInterval(1000);
    if (!telemetryTimer->isActive()) telemetryTimer->start();

    // Обычное фото
    if (photoRate > 0) {
        photoTimer->setInterval(1000 / photoRate);
        if (!photoTimer->isActive()) photoTimer->start();
    } else {
        photoTimer->stop();
    }


    if (priorityPhotoEnabled && priorityPhotoRate > 0) {
        priorityPhotoTimer->setInterval(1000 / priorityPhotoRate);
        if (!priorityPhotoTimer->isActive()) {
            priorityPhotoTimer->start();
        }
    } else {
        priorityPhotoTimer->stop();
    }
}
void Drone::log(const QString &msg) {
    logDisplay->append(QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + " " + msg);
}

