#ifndef GROUNDSTATION_H
#define GROUNDSTATION_H

#include <QMainWindow>
#include <QUdpSocket>
#include <QTimer>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QMap>
#include <QSet>
#include "../common/protocol.h"

class GroundStation : public QMainWindow
{
    Q_OBJECT

public:
    GroundStation(QWidget *parent = nullptr);
    ~GroundStation();

private slots:
    void onStartClicked();
    void processDatagrams();
    void sendFeedback();
    void updateLoss();

private:
    void log(const QString &msg);
    double calculateLossPercent();
    void processFECPacket(const FECPacket& fecPkt);

    QUdpSocket *socket;
    QTimer *feedbackTimer;
    QTimer *lossTimer;

    QLineEdit *portInput;
    QPushButton *startButton;
    QTextEdit *logDisplay;

    bool lossloss;
    bool running;
    bool autoModeEnabled;
    quint16 listenPort;

    QHostAddress droneAddress;
    quint16 dronePort;

    int lossPercent = 0;
    int minDelayMs = 50;
    int maxDelayMs = 200;

    // Метрики
    int receivedTelemetry;
    int receivedPhoto;
    int lostTelemetry;
    int lastTelemetrySeq;
    int telemetryRecoveredCount;     // счётчик восстановленных через FEC пакетов телеметрии

    // FEC хранение
    struct FECGroup {
        quint32 packetId;
        QMap<quint32, QByteArray> fragments;
        QByteArray parity;
        quint64 firstTimestamp;
        bool recovered;
    };
    QMap<quint32, FECGroup> fecGroups;
    quint32 lastReceivedPacketId = 0;
};

#endif // GROUNDSTATION_H
