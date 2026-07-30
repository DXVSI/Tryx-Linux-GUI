QT += core dbus testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = linuxtraycontroller-tests

INCLUDEPATH += $$PWD/../../src/quick

DESTDIR = $$PWD/../../build/linuxtray-tests
OBJECTS_DIR = $$PWD/../../build/linuxtray-tests/obj
MOC_DIR = $$PWD/../../build/linuxtray-tests/moc

HEADERS += \
    ../../src/quick/linuxtraycontroller.h

SOURCES += \
    ../../src/quick/linuxtraycontroller.cpp \
    linuxtraycontroller_tests.cpp
