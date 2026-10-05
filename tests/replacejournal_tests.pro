QT += core testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = replacejournal-tests

INCLUDEPATH += $$PWD/../src

DESTDIR = $$PWD/../build/replacejournal-tests
OBJECTS_DIR = $$PWD/../build/replacejournal-tests/obj
MOC_DIR = $$PWD/../build/replacejournal-tests/moc

HEADERS += \
    $$PWD/../src/replacejournal.h

SOURCES += \
    replacejournal_tests.cpp \
    $$PWD/../src/replacejournal.cpp
