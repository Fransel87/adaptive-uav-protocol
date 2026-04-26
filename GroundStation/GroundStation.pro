QT += core network widgets
CONFIG += c++11
TARGET = GroundStation
TEMPLATE = app
CONFIG += static

SOURCES += main.cpp groundstation.cpp
HEADERS += groundstation.h ../common/protocol.h
