QT += core dbus network testlib

CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = tryx-runtimebootstrap-tests

INCLUDEPATH += $$PWD/../../src $$PWD/../../src/quick

DESTDIR = $$PWD/../../build/runtimebootstrap-tests/quick
OBJECTS_DIR = $$PWD/../../build/runtimebootstrap-tests/obj
MOC_DIR = $$PWD/../../build/runtimebootstrap-tests/moc
DEFINES += TRYX_BUILD_QUICK_DIRECTORY=\\\"$$clean_path($$DESTDIR)\\\"

HEADERS += \
    ../../src/runtimecontract.h \
    ../../src/quick/runtimebootstrap.h

SOURCES += \
    ../../src/runtimebadgetext.cpp \
    ../../src/runtimecontract.cpp \
    ../../src/quick/runtimebootstrap.cpp \
    runtimebootstrap_tests.cpp
