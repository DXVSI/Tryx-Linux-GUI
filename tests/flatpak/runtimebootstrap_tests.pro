QT += core dbus network testlib
CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = flatpak-runtimebootstrap-tests
DEFINES += TRYX_FLATPAK
INCLUDEPATH += $$PWD/../../src $$PWD/../../src/quick
DESTDIR = $$PWD/../../build/flatpak-runtimebootstrap-tests
OBJECTS_DIR = $$DESTDIR/obj
MOC_DIR = $$DESTDIR/moc
HEADERS += ../../src/quick/runtimebootstrap.h ../../src/flatpakruntimeownership.h
SOURCES += runtimebootstrap_tests.cpp \
    ../../src/quick/runtimebootstrap.cpp ../../src/flatpakruntimeownership.cpp \
    ../../src/runtimecontract.cpp ../../src/runtimebadgetext.cpp
SOURCES += ../../src/quick/guiautostart.cpp
