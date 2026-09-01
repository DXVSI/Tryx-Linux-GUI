QT += core dbus gui

CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = tryx

!versionAtLeast(QT_VERSION, 6.4.0) {
    error("tryx requires Qt 6.4 or newer")
}

VERSION = $$cat($$PWD/VERSION, lines)
isEmpty(VERSION): error("VERSION is empty or missing")
DEFINES += TRYX_APP_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD/src $$PWD/src/cli $$PWD/src/quick

DESTDIR = $$PWD/build/cli
OBJECTS_DIR = $$PWD/build/cli/obj
MOC_DIR = $$PWD/build/cli/moc
RCC_DIR = $$PWD/build/cli/rcc

HEADERS += \
    src/runtimecontract.h \
    src/runtimedowngradestore.h \
    src/supportsnapshot.h \
    src/quick/supportbundle.h \
    src/cli/sessionbusguard.h \
    src/cli/tryxclirunner.h

SOURCES += \
    src/runtimecontract.cpp \
    src/runtimedowngradestore.cpp \
    src/supportsnapshot.cpp \
    src/quick/supportbundle.cpp \
    src/cli/sessionbusguard.cpp \
    src/cli/tryxclirunner.cpp \
    src/cli/main.cpp

cli_tests.target = cli-check
cli_tests.depends = \
    $$relative_path($$DESTDIR/$$TARGET, $$OUT_PWD)
cli_tests.commands = \
    cd $$shell_path($$PWD/tests/cli) && \
        $$QMAKE_QMAKE cli_tests.pro && $(MAKE) && \
        dbus-run-session -- \
            $$shell_path($$PWD/build/cli-tests/tryx-cli-tests) -txt
QMAKE_EXTRA_TARGETS += cli_tests

DISTFILES += \
    packaging/tryx.1 \
    tests/cli/cli_tests.cpp \
    tests/cli/cli_tests.pro

unix {
    target.path = /usr/bin
    tryx_cli_manpage.path = /usr/share/man/man1
    tryx_cli_manpage.files = packaging/tryx.1
    INSTALLS += target tryx_cli_manpage
}
