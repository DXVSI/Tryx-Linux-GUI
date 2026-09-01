QT += core testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = runtime-downgradestore-tests
DEFINES += TRYX_RUNTIME_DOWNGRADE_STORE_TESTING

RUNTIME_BINARY = $$clean_path($$PWD/../build/runtime/tryx-panorama-runtime)
!exists($$RUNTIME_BINARY): error("Build tryx-panorama-runtime before runtime downgrade store tests")
PRE_TARGETDEPS += $$RUNTIME_BINARY
DEFINES += TRYX_RUNTIME_BINARY=\\\"$$RUNTIME_BINARY\\\"

INCLUDEPATH += $$PWD/../src

DESTDIR = $$PWD/../build/runtime-downgradestore-tests
OBJECTS_DIR = $$PWD/../build/runtime-downgradestore-tests/obj
MOC_DIR = $$PWD/../build/runtime-downgradestore-tests/moc

HEADERS += \
    $$PWD/../src/runtimedowngradestore.h

SOURCES += \
    runtime_downgradestore_tests.cpp \
    $$PWD/../src/runtimedowngradestore.cpp
