QT += core dbus gui testlib

CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = runtimeclient-handshake-tests

INCLUDEPATH += $$PWD/../../src $$PWD/../../src/quick

DESTDIR = $$PWD/../../build/runtimeclient-handshake-tests
OBJECTS_DIR = $$PWD/../../build/runtimeclient-handshake-tests/obj
MOC_DIR = $$PWD/../../build/runtimeclient-handshake-tests/moc

HEADERS += \
    ../../src/applicationpaths.h \
    ../../src/mediatransform.h \
    ../../src/runtimecontract.h \
    ../../src/supportsnapshot.h \
    ../../src/quick/supportbundle.h \
    ../../src/quick/supportbundlecontroller.h \
    ../../src/quick/mediacatalogmodel.h \
    ../../src/quick/operationlistmodel.h \
    ../../src/quick/runtimeclient.h \
    ../../src/quick/savedlayoutlistmodel.h

SOURCES += \
    ../../src/mediatransform.cpp \
    ../../src/runtimebadgetext.cpp \
    ../../src/runtimecontract.cpp \
    ../../src/supportsnapshot.cpp \
    ../../src/quick/supportbundle.cpp \
    ../../src/quick/supportbundlecontroller.cpp \
    ../../src/quick/mediacatalogmodel.cpp \
    ../../src/quick/operationlistmodel.cpp \
    ../../src/quick/runtimeclient.cpp \
    ../../src/quick/savedlayoutlistmodel.cpp \
    runtimeclient_handshake_tests.cpp
