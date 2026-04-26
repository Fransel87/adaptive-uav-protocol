#include <QApplication>
#include "drone.h"

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    Drone drone;
    drone.setWindowTitle("🚁 Дрон - Адаптивный протокол");
    drone.resize(600, 400);
    drone.show();

    return a.exec();
}
