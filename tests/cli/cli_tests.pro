QT += core dbus gui testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tryx-cli-tests

INCLUDEPATH += \
    $$PWD/../../src \
    $$PWD/../../src/cli \
    $$PWD/../../src/quick

VERSION = $$cat($$PWD/../../VERSION, lines)
isEmpty(VERSION): error("VERSION is empty or missing")
DEFINES += TRYX_APP_VERSION=\\\"$$VERSION\\\"
DEFINES += TRYX_CLI_TESTING TRYX_SUPPORT_BUNDLE_TESTING
DEFINES += TRYX_EXPECTED_VERSION=\\\"$$VERSION\\\"
DEFINES += TRYX_CLI_BINARY=\\\"$$clean_path($$PWD/../../build/cli/tryx)\\\"

DESTDIR = $$PWD/../../build/cli-tests
OBJECTS_DIR = $$PWD/../../build/cli-tests/obj
MOC_DIR = $$PWD/../../build/cli-tests/moc

SOURCES += \
    ../../src/runtimebadgetext.cpp \
    ../../src/runtimecontract.cpp \
    ../../src/runtimedowngradestore.cpp \
    ../../src/supportsnapshot.cpp \
    ../../src/quick/supportbundle.cpp \
    ../../src/cli/sessionbusguard.cpp \
    ../../src/cli/tryxclirunner.cpp \
    cli_tests.cpp

HEADERS += \
    ../../src/runtimecontract.h \
    ../../src/runtimedowngradestore.h \
    ../../src/supportsnapshot.h \
    ../../src/quick/supportbundle.h \
    ../../src/cli/sessionbusguard.h \
    ../../src/cli/tryxclirunner.h
