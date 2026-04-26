#ifndef DRONE_H
#define DRONE_H

#include <QMainWindow>
#include <QUdpSocket>
#include <QTimer>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include "../common/protocol.h"
#include <QFile>
#include <QSlider>
#include <QLabel>


class Drone : public QMainWindow
{
    Q_OBJECT

public:
    Drone(QWidget *parent = nullptr);
    ~Drone();

private slots:
    void onConnectClicked();
    void sendTelemetry();
    void sendPhoto();
    void processFeedback();
    void updatePosition();
    void adaptProtocol(double lossPercent);
    void sendPriorityPhoto();  // новый слот для приоритетных фото

private:
    void updateTimers();
    void log(const QString &msg);
    void sendTelemetryWithFEC();  // отправка телеметрии с FEC

    AdaptationStats stats;
    QString lastPhotoState;

    QFile csvFile;

    QUdpSocket *socket;
    QTimer *telemetryTimer;
    QTimer *photoTimer;
    QTimer *motionTimer;
    QTimer *priorityPhotoTimer;  // новый таймер для приоритетных фото

    QLineEdit *ipInput;
    QLineEdit *portInput;
    QPushButton *connectButton;
    QTextEdit *logDisplay;

    QString serverIp;
    quint16 serverPort;
    bool connected;
    int lossPercent = 0;

    quint32 seqNumTelemetry;
    quint32 seqNumPhoto;
    quint32 fecPacketId;
    double photoRate;
    bool fecEnabled;      // FEC для телеметрии

    double latitude, longitude, altitude, speed, battery;

    void logToCSV(double lossPercent);

    quint32 seqNumPriorityPhoto;
    double priorityPhotoRate;    // частота приоритетных фото
    bool priorityPhotoEnabled;

    double currentSNR = 20;  // текущее значение SNR в dB

    QSlider *snrSlider;
    QLabel *snrLabel;
    bool previousSnrWasBad = false;  // для гистерезиса SNR
    bool previousLossWasBad = false;  // для гистерезиса Loss

};

#endif // DRONE_H

