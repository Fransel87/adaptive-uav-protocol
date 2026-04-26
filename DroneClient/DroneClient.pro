QT += core network widgets
CONFIG += c++11
TARGET = DroneClient
TEMPLATE = app
CONFIG += static

SOURCES += main.cpp drone.cpp
HEADERS += drone.h ../common/protocol.h
