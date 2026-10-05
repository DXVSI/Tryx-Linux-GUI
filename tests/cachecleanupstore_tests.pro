QT += core dbus gui testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = cachecleanupstore-tests
DEFINES += TRYX_PROTOCOL_TESTING

INCLUDEPATH += $$PWD/../src $$PWD/../include

DESTDIR = $$PWD/../build/cachecleanupstore-tests
OBJECTS_DIR = $$PWD/../build/cachecleanupstore-tests/obj
MOC_DIR = $$PWD/../build/cachecleanupstore-tests/moc

HEADERS += \
    $$PWD/../src/applicationpaths.h \
    $$PWD/../src/devicemediaartifactstore.h \
    $$PWD/../src/mediacatalogstore.h \
    $$PWD/../src/printermediafileintegrity.h \
    $$PWD/../src/printermediaidentity.h \
    $$PWD/../src/privateruntimepaths.h \
    $$PWD/../src/runtimecontract.h

SOURCES += \
    cachecleanupstore_tests.cpp \
    $$PWD/../src/devicemediaartifactstore.cpp \
    $$PWD/../src/mediacatalogstore.cpp \
    $$PWD/../src/printermediafileintegrity.cpp \
    $$PWD/../src/privateruntimepaths.cpp \
    $$PWD/../src/runtimebadgetext.cpp \
    $$PWD/../src/runtimecontract.cpp
