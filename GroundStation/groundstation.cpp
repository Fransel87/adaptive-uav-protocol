#include "groundstation.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QRandomGenerator>
#include <QFile>
#include <QTextStream>
#include <QSlider>

GroundStation::GroundStation(QWidget *parent) : QMainWindow(parent) {
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout *mainLayout = new QVBoxLayout(central);

    QHBoxLayout *controlLayout = new QHBoxLayout();
    controlLayout->addWidget(new QLabel("Порт:"));
    portInput = new QLineEdit("8888");
    portInput->setFixedWidth(80);
    controlLayout->addWidget(portInput);
    startButton = new QPushButton("Старт");
    controlLayout->addWidget(startButton);
    controlLayout->addStretch();
    mainLayout->addLayout(controlLayout);

    logDisplay = new QTextEdit();
    logDisplay->setReadOnly(true);
    mainLayout->addWidget(logDisplay);

    QHBoxLayout *sliderLayout = new QHBoxLayout();
    sliderLayout->addWidget(new QLabel("Потери пакетов:"));
    QSlider *lossSlider = new QSlider(Qt::Horizontal);
    lossSlider->setRange(0, 100);
    lossSlider->setValue(lossPercent);
    lossSlider->setFixedWidth(200);
    sliderLayout->addWidget(lossSlider);

    QLabel *lossValueLabel = new QLabel("0%");
    sliderLayout->addWidget(lossValueLabel);

    QPushButton *autoModeButton = new QPushButton("Авторежим");
    autoModeButton->setCheckable(true);
    sliderLayout->addWidget(autoModeButton);

    controlLayout->addLayout(sliderLayout);

    socket = new QUdpSocket(this);

    feedbackTimer = new QTimer(this);
    lossTimer = new QTimer(this);

    connect(startButton, &QPushButton::clicked, this, &GroundStation::onStartClicked);
    connect(socket, &QUdpSocket::readyRead, this, &GroundStation::processDatagrams);
    connect(feedbackTimer, &QTimer::timeout, this, &GroundStation::sendFeedback);
    connect(lossTimer, &QTimer::timeout, this, &GroundStation::updateLoss);

    connect(lossSlider, &QSlider::valueChanged, this, [this, lossValueLabel, autoModeButton](int value) {
        if (!autoModeButton->isChecked()) {
            lossPercent = value;
            lossValueLabel->setText(QString("%1%").arg(value));
            log(QString("🔧 Ручное изменение: loss = %1%").arg(lossPercent));
        }
    });

    connect(autoModeButton, &QPushButton::toggled, this, [this, lossSlider, lossValueLabel](bool checked) {
        autoModeEnabled = checked;
        if (checked) {
            log("🤖 Включен АВТОМАТИЧЕСКИЙ режим изменения потерь (для демонстрации FEC)");
            lossSlider->setEnabled(false);
            lossTimer->start(1000);
        } else {
            log("👆 Переключен в РУЧНОЙ режим управления потерями");
            lossSlider->setEnabled(true);
            lossTimer->stop();
            lossPercent = lossSlider->value();
            lossValueLabel->setText(QString("%1%").arg(lossPercent));
            log(QString("🔧 Потери установлены в %1% (ручной режим)").arg(lossPercent));
        }
    });

    running = false;
    autoModeEnabled = false;
    receivedTelemetry = 0;
    receivedPhoto = 0;
    lostTelemetry = 0;
    lastTelemetrySeq = -1;
    telemetryRecoveredCount = 0;
    lastReceivedPacketId = 0;

    log("Наземная станция готова");
    log("Управление: ползунок для ручной настройки потерь, кнопка 'Авторежим' для демонстрации адаптации");
}

GroundStation::~GroundStation() {}

void GroundStation::onStartClicked() {
    if (running) {
        socket->close();
        feedbackTimer->stop();
        if (autoModeEnabled) {
            lossTimer->stop();
        }
        startButton->setText("Старт");
        running = false;
        log("⏹️ Сервер остановлен");
        return;
    }

    listenPort = portInput->text().toUShort();
    if (listenPort == 0) {
        log("❌ Ошибка: введите корректный порт");
        return;
    }

    if (!socket->bind(QHostAddress::Any, listenPort)) {
        log("❌ Ошибка: не могу открыть порт " + QString::number(listenPort));
        return;
    }

    feedbackTimer->start(2000);

    startButton->setText("Стоп");
    running = true;

    log(QString("✅ Сервер запущен на порту %1").arg(listenPort));
    log(QString("⚠️ Текущие потери: %1%, задержка %2-%3 мс")
        .arg(lossPercent).arg(minDelayMs).arg(maxDelayMs));
}

void GroundStation::processFECPacket(const FECPacket& fecPkt) {
    // Статический набор для отслеживания уже обработанных packetId
    static QSet<quint32> processedPackets;

    // Если этот packetId уже обработан - игнорируем (и оригинал, и дубликат)
    if (processedPackets.contains(fecPkt.packetId)) {
        log(QString("⚠️ [FEC] Пакет #%1 уже обработан").arg(fecPkt.packetId));
        return;
    }

    // Помечаем как обработанный
    processedPackets.insert(fecPkt.packetId);

    // Проверяем потери в последовательности
    static quint32 lastSeq = 0;
    if (lastSeq > 0 && fecPkt.packetId > lastSeq + 1) {
        quint32 lost = fecPkt.packetId - lastSeq - 1;
        lostTelemetry += lost;
        log(QString("⚠️ Потеряно %1 пакетов между #%2 и #%3").arg(lost).arg(lastSeq).arg(fecPkt.packetId));
    }

    // Обрабатываем телеметрию
    DroneState state = DroneState::fromBytes(fecPkt.data);
    receivedTelemetry++;  // Увеличивается только один раз для каждого packetId

    // Логируем, откуда пришли данные
    if (fecPkt.fragId == 0) {
        log(QString("✅ [FEC] Телеметрия #%1 (оригинал) | Поз:%2,%3 | Батарея:%4%")
            .arg(fecPkt.packetId)
            .arg(state.latitude, 0, 'f', 4)
            .arg(state.longitude, 0, 'f', 4)
            .arg(state.battery, 0, 'f', 1));
    } else {
        telemetryRecoveredCount++;  // Считаем восстановленные только для дубликатов
        log(QString("🔄 [FEC] Телеметрия #%1 (ВОССТАНОВЛЕНА из дубликата) | Поз:%2,%3 | Батарея:%4%")
            .arg(fecPkt.packetId)
            .arg(state.latitude, 0, 'f', 4)
            .arg(state.longitude, 0, 'f', 4)
            .arg(state.battery, 0, 'f', 1));
    }

    lastSeq = fecPkt.packetId;

    // Очищаем набор, чтобы он не рос бесконечно (храним последние 1000)
    if (processedPackets.size() > 1000) {
        processedPackets.clear();
    }
}


void GroundStation::processDatagrams() {
    while (socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(socket->pendingDatagramSize());
        QHostAddress sender;
        quint16 senderPort;

        socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        droneAddress = sender;
        dronePort = senderPort;

        // Моделирование потерь
        if (QRandomGenerator::global()->bounded(100) < lossPercent) {
            log("❌ Пакет потерян (потери " + QString::number(lossPercent) + "%)");

            continue;
        }
        // Моделирование потерь
        if (QRandomGenerator::global()->bounded(100) < lossPercent) {
            log("⚡📉 Потери: пакет отброшен");
            lossloss = true;
        }
        else {lossloss=false;}

        // Моделирование задержки
        int delay = minDelayMs + QRandomGenerator::global()->bounded(maxDelayMs - minDelayMs);

        QTimer::singleShot(delay, this, [this, datagram]() {
            Packet p = Packet::fromBytes(datagram);
            quint64 now = QDateTime::currentMSecsSinceEpoch();


            if (p.type == PT_TELEMETRY) {
                DroneState state = DroneState::fromBytes(p.data);
                receivedTelemetry++;


                if (lastTelemetrySeq != -1) {
                    int lost = p.seqNum - lastTelemetrySeq - 1;
                    if (lost > 0) {
                        lostTelemetry += lost;
                    }
                }
                lastTelemetrySeq = p.seqNum;


                log(QString("📥 Телеметрия #%1 (обычный режим) | Поз:%2,%3 | Батарея:%4%")
                    .arg(p.seqNum)
                    .arg(state.latitude, 0, 'f', 4)
                    .arg(state.longitude, 0, 'f', 4)
                    .arg(state.battery, 0, 'f', 1));

            } else if (p.type == PT_TELEMETRY_FEC) {
                FECPacket fecPkt = FECPacket::fromBytes(p.data);
                processFECPacket(fecPkt);

            } else if (p.type == PT_PHOTO) {
                receivedPhoto++;
                log(QString("📥 Фото #%1 | Размер:%2 байт")
                    .arg(p.seqNum).arg(p.data.size()));
            }
            else if (p.type == PT_PHOTO_PRIORITY) {
                 receivedPhoto++;
                 int packetDelay = now - p.timestamp;
                 log(QString("🔥 ПРИОРИТЕТНОЕ ФОТО #%1 | Размер:%2 байт | Задержка:%3 мс")
                     .arg(p.seqNum).arg(p.data.size()).arg(packetDelay));
             }

        });
    }
}

void GroundStation::sendFeedback() {
    if (!running || droneAddress.isNull()) return;

    Packet p;
    p.type = PT_FEEDBACK;
    p.timestamp = QDateTime::currentMSecsSinceEpoch();

    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);

    if (lossloss==true) {
        stream << (double)-1;  // отправляем -1 как специальное значение
    } else {
        stream << (double)lossPercent;  // нормальное значение
    }
    stream << (quint32)telemetryRecoveredCount;
    stream << (quint32)lostTelemetry;
    stream << (quint32)receivedTelemetry;
    p.data = data;

    QByteArray bytes = p.toBytes();
    socket->writeDatagram(bytes, droneAddress, dronePort);

    if(lossloss==false){
    log(QString("📤 Обратная связь: Loss=%1% | Восстановлений телеметрии (FEC): %2")
        .arg((double)lossPercent, 0, 'f', 1)
        .arg(telemetryRecoveredCount));}
    else {log(QString("📤 Обратная связь: Loss неизвестен | Восстановлений телеметрии (FEC): %2")
        .arg(telemetryRecoveredCount));}
}

void GroundStation::updateLoss() {
    if (!running || !autoModeEnabled) return;

    static double time = 0;

    int newLoss = 0;
    QString mode;

    if (time < 62) {
        newLoss = 0;
        mode = "ОТЛИЧНЫЙ КАНАЛ";
    } else if (time < 122) {
        newLoss = 5;
        mode = "ХОРОШИЙ КАНАЛ (FEC активируется)";
    } else if (time < 182) {
        newLoss = 10;
        mode = "ПЛОХОЙ КАНАЛ (FEC восстанавливает телеметрию)";
    } else if (time < 242) {
        newLoss = 20;
        mode = "КРИТИЧЕСКИЙ КАНАЛ (фото отключено)";
    } else if (time < 302) {
        newLoss = 5;
        mode = "ХОРОШИЙ КАНАЛ";
    }else if (time < 362) {
        newLoss = 0;
        mode = "ОТЛИЧНЫЙ КАНАЛ";
    }else {
        time = 0;
        newLoss = 0;
        mode = "НАЧАЛО НОВОГО ЦИКЛА";
        log(QString("📊 " + mode));
    }

    if (newLoss != lossPercent) {
        lossPercent = newLoss;
        log(QString("📊 %1: потери установлены в %2%").arg(mode).arg(lossPercent));
    }
    time += 1;
}

double GroundStation::calculateLossPercent() {
    int total = receivedTelemetry + lostTelemetry;
    if (total == 0) return 0;
    return (double)lostTelemetry / total * 100;
}

void GroundStation::log(const QString &msg) {
    logDisplay->append(QDateTime::currentDateTime().toString("hh:mm:ss.zzz") + " " + msg);
}
