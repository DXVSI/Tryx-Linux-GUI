QT += core dbus network testlib
QT -= gui
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = releaseupdatecontroller-tests

INCLUDEPATH += $$PWD/../../src/quick
DESTDIR = $$PWD/../../build/releaseupdate-tests
OBJECTS_DIR = $$PWD/../../build/releaseupdate-tests/obj
MOC_DIR = $$PWD/../../build/releaseupdate-tests/moc

HEADERS += ../../src/quick/releaseinfo.h ../../src/quick/releaseupdatecontroller.h \
    ../../src/quick/linuxtraycontroller.h
SOURCES += ../../src/quick/releaseinfo.cpp \
    ../../src/quick/releaseupdatecontroller.cpp \
    ../../src/quick/linuxtraycontroller.cpp releaseupdatecontroller_tests.cpp
