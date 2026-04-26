#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QIODevice>
#include <QMetaType>
#include <QByteArray>
#include <QDataStream>

// Типы пакетов
enum PacketType {
    PT_TELEMETRY = 1,
    PT_PHOTO = 2,
    PT_FEEDBACK = 3,
    PT_TELEMETRY_FEC = 4,
    PT_PHOTO_PRIORITY = 5   // приоритетное фото (тепловизионное)
};


// Структура пакета
struct Packet {
    PacketType type;
    quint32 seqNum;
    quint64 timestamp;
    quint8 priority;
    QByteArray data;

    QByteArray toBytes() const {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream << (quint8)type;
        stream << seqNum;
        stream << timestamp;
        stream << priority;
        stream << data;
        return bytes;
    }

    static Packet fromBytes(const QByteArray &bytes) {
        Packet p;
        QDataStream stream(bytes);
        quint8 t;
        stream >> t;
        p.type = (PacketType)t;
        stream >> p.seqNum;
        stream >> p.timestamp;
        stream >> p.priority;
        stream >> p.data;
        return p;
    }
};

// FEC пакет для телеметрии
struct FECPacket {
    quint32 packetId;
    quint32 fragId;
    QByteArray data;
    bool isParity;

    QByteArray toBytes() const {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream << packetId;
        stream << fragId;
        stream << data;
        stream << (quint8)(isParity ? 1 : 0);
        return bytes;
    }

    static FECPacket fromBytes(const QByteArray &bytes) {
        FECPacket p;
        QDataStream stream(bytes);
        quint8 parityFlag;
        stream >> p.packetId;
        stream >> p.fragId;
        stream >> p.data;
        stream >> parityFlag;
        p.isParity = (parityFlag == 1);
        return p;
    }
};

// Параметры адаптации
struct AdaptationParams {
    static constexpr double LOSS_BAD = 20.0;
    static constexpr double LOSS_MID_LOW = 10.0;
    static constexpr double LOSS_MID_HIGH = 5.0;

    static constexpr double TELE_RATE_ALWAYS = 1.0;
    static constexpr double PHOTO_RATE_OFF = 0.0;
    static constexpr double PHOTO_RATE_LOW = 0.5;

    static constexpr int TELEMETRY_SIZE = 64;
    static constexpr int PHOTO_SIZE = 512;
    static constexpr int PHOTO_PRIORITY_SIZE = 256;  // приоритетное фото меньше/важнее
};

// Состояние дрона
struct DroneState {
    float latitude = 55.7558;
    float longitude = 37.6176;
    float altitude = 100.0;
    float speed = 10.0;
    float battery = 95.0;

    QByteArray toBytes() const {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream << latitude;
        stream << longitude;
        stream << altitude;
        stream << speed;
        stream << battery;
        return bytes;
    }

    static DroneState fromBytes(const QByteArray &bytes) {
        DroneState s;
        QDataStream stream(bytes);
        stream >> s.latitude;
        stream >> s.longitude;
        stream >> s.altitude;
        stream >> s.speed;
        stream >> s.battery;
        return s;
    }
};

struct AdaptationStats {
    double lossLow = 5.0;
    double lossHigh = 20.0;

    int numSwitches = 0;
    int photoDelivered = 0;
    int telemetryRecovered = 0;   // количество восстановленных пакетов телеметрии
    int telemetryReceived = 0;
    int telemetrySent = 0;
    int telemetryLost = 0;

    double getTelemetryLossPercent() const {
        if (telemetrySent == 0) return 0;
        return (double)telemetryLost / telemetrySent * 100;
    }

    void reset() {
        numSwitches = 0;
        photoDelivered = 0;
        telemetryRecovered = 0;
        telemetrySent = 0;
        telemetryLost = 0;
    }
};

#endif


