QT += core dbus testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = savedlayoutstore-tests
DEFINES += TRYX_SAVED_LAYOUT_STORE_TESTING

INCLUDEPATH += $$PWD/../src

DESTDIR = $$PWD/../build/savedlayoutstore-tests
OBJECTS_DIR = $$PWD/../build/savedlayoutstore-tests/obj
MOC_DIR = $$PWD/../build/savedlayoutstore-tests/moc

HEADERS += \
    $$PWD/../src/applicationpaths.h \
    $$PWD/../src/runtimeapplyrequestcodec.h \
    $$PWD/../src/runtimecontract.h \
    $$PWD/../src/savedlayoutstore.h

SOURCES += \
    savedlayoutstore_tests.cpp \
    $$PWD/../src/runtimeapplyrequestcodec.cpp \
    $$PWD/../src/runtimecontract.cpp \
    $$PWD/../src/savedlayoutstore.cpp
