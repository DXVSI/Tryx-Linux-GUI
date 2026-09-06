QT += core dbus testlib
QT -= gui
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = badgetext-tests

INCLUDEPATH += $$PWD/../src
DESTDIR = $$PWD/../build/badgetext-tests
OBJECTS_DIR = $$PWD/../build/badgetext-tests/obj
MOC_DIR = $$PWD/../build/badgetext-tests/moc

HEADERS += $$PWD/../src/runtimebadgetext.h
SOURCES += $$PWD/../src/runtimebadgetext.cpp $$PWD/badgetext_tests.cpp \
    $$PWD/../src/runtimecontract.cpp $$PWD/../src/runtimeapplyrequestcodec.cpp
