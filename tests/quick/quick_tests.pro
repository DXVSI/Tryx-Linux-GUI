QT += concurrent core dbus gui testlib

CONFIG += c++17 console testcase
TEMPLATE = app
TARGET = tryx-quick-tests

INCLUDEPATH += $$PWD/../../src $$PWD/../../src/quick
INCLUDEPATH += $$PWD/../../include

DESTDIR = $$PWD/../../build/quick-tests
OBJECTS_DIR = $$PWD/../../build/quick-tests/obj
MOC_DIR = $$PWD/../../build/quick-tests/moc

HEADERS += \
    ../../src/systemmonitor.h \
    ../../src/runtimecontract.h \
    ../../src/mediatransform.h \
    ../../src/quick/appsettingscontroller.h \
    ../../src/quick/devicemediaworkflowcontroller.h \
    ../../src/quick/firmwarecontroller.h \
    ../../src/quick/mediacatalogmodel.h \
    ../../src/quick/mediaeditorcontroller.h \
    ../../src/quick/mediapreviewcontroller.h \
    ../../src/quick/operationlistmodel.h \
    ../../src/quick/runtimeclient.h \
    ../../src/quick/systemmetricsmodel.h \
    ../../src/quick/windowchromecontroller.h

SOURCES += \
    ../../src/systemmonitor.cpp \
    ../../src/runtimecontract.cpp \
    ../../src/mediatransform.cpp \
    ../../src/core/config.cpp \
    ../../src/quick/appsettingscontroller.cpp \
    ../../src/quick/devicemediaworkflowcontroller.cpp \
    ../../src/quick/firmwarecontroller.cpp \
    ../../src/quick/mediacatalogmodel.cpp \
    ../../src/quick/mediaeditorcontroller.cpp \
    ../../src/quick/mediapreviewcontroller.cpp \
    ../../src/quick/operationlistmodel.cpp \
    ../../src/quick/runtimeclient.cpp \
    ../../src/quick/systemmetricsmodel.cpp \
    ../../src/quick/windowchromecontroller.cpp \
    tst_quickmodels.cpp
