#include <QApplication>
#include "groundstation.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    GroundStation station;
    station.setWindowTitle("📡 Наземная станция - Эмулятор канала");
    station.resize(600, 400);
    station.show();

    return a.exec();
}
