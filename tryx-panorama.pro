QT += core dbus gui widgets network

CONFIG += c++17 lrelease embed_translations link_pkgconfig
TARGET = tryx-panorama-manager
TEMPLATE = app

VERSION = $$cat($$PWD/VERSION, lines)
isEmpty(VERSION): error("VERSION is empty or missing")
DEFINES += TRYX_APP_VERSION=\\\"$$VERSION\\\"

PKGCONFIG += protobuf libudev libusb-1.0

PROTOC_VERSION = $$system(protoc --version)
PROTOC_VERSION = $$last(PROTOC_VERSION)
PROTOBUF_RUNTIME_VERSION = $$system(pkg-config --modversion protobuf)
isEmpty(PROTOC_VERSION): error("protoc was not found")
isEmpty(PROTOBUF_RUNTIME_VERSION): error("protobuf pkg-config metadata was not found")
# Some distributions omit only a trailing zero patch component from
# `protoc --version` while pkg-config reports it, for example 35.1 versus
# 35.1.0. Normalize that formatting difference without accepting real patch
# skew such as 35.1.1 versus 35.1.0.
PROTOC_NORMALIZED_VERSION = $$PROTOC_VERSION
PROTOC_PATCH_VERSION = $$section(PROTOC_VERSION, ., 2, 2)
isEmpty(PROTOC_PATCH_VERSION): PROTOC_NORMALIZED_VERSION = $${PROTOC_VERSION}.0
PROTOBUF_RUNTIME_NORMALIZED_VERSION = $$PROTOBUF_RUNTIME_VERSION
PROTOBUF_RUNTIME_PATCH_VERSION = $$section(PROTOBUF_RUNTIME_VERSION, ., 2, 2)
isEmpty(PROTOBUF_RUNTIME_PATCH_VERSION): PROTOBUF_RUNTIME_NORMALIZED_VERSION = $${PROTOBUF_RUNTIME_VERSION}.0
!equals(PROTOC_NORMALIZED_VERSION, $$PROTOBUF_RUNTIME_NORMALIZED_VERSION): error("protoc $$PROTOC_VERSION does not match libprotobuf $$PROTOBUF_RUNTIME_VERSION")

TRANSLATIONS += translations/tryx-panorama_ru.ts
LRELEASE_DIR = build/i18n

INCLUDEPATH += $$PWD/include

# Minimal, independently named schema for the confirmed PASE wire contract.
PROTO_DIR = $$PWD/protocol/wire-v1
PROTO_GEN_DIR = $$PWD/build/generated/proto
PROTO_FILES = \
    $$PROTO_DIR/configuration.proto \
    $$PROTO_DIR/overlay.proto \
    $$PROTO_DIR/transport.proto

INCLUDEPATH += $$PROTO_GEN_DIR
DEPENDPATH += $$PROTO_GEN_DIR

protobuf_header.name = protoc header ${QMAKE_FILE_IN}
protobuf_header.input = PROTO_FILES
protobuf_header.output = $$PROTO_GEN_DIR/${QMAKE_FILE_BASE}.pb.h
protobuf_header.commands = $$QMAKE_MKDIR $$shell_path($$PROTO_GEN_DIR) && cd $$shell_path($$PROTO_DIR) && protoc --proto_path=. --cpp_out=$$shell_path($$PROTO_GEN_DIR) ${QMAKE_FILE_BASE}.proto
protobuf_header.depends = $$PROTO_FILES
protobuf_header.variable_out = GENERATED_FILES
protobuf_header.CONFIG += no_link target_predeps

protobuf_source.name = protoc source ${QMAKE_FILE_IN}
protobuf_source.input = PROTO_FILES
protobuf_source.output = $$PROTO_GEN_DIR/${QMAKE_FILE_BASE}.pb.cc
protobuf_source.commands = if test ! -f ${QMAKE_FILE_OUT}; then $$QMAKE_MKDIR $$shell_path($$PROTO_GEN_DIR) && cd $$shell_path($$PROTO_DIR) && protoc --proto_path=. --cpp_out=$$shell_path($$PROTO_GEN_DIR) ${QMAKE_FILE_BASE}.proto; fi
protobuf_source.depends = $$PROTO_GEN_DIR/${QMAKE_FILE_BASE}.pb.h
protobuf_source.variable_out = GENERATED_SOURCES
protobuf_source.dependency_type = TYPE_C

QMAKE_EXTRA_COMPILERS += protobuf_header protobuf_source

protocol_tests.target = check
protocol_tests.commands = sh $$shell_path($$PWD/tests/check_no_bundled_video.sh) && cd $$shell_path($$PWD/tests) && $$QMAKE_QMAKE printerprotocol_tests.pro && $(MAKE) && $$shell_path($$PWD/build/tests/printerprotocol-tests) && $$QMAKE_QMAKE replacejournal_tests.pro && $(MAKE) && $$shell_path($$PWD/build/replacejournal-tests/replacejournal-tests)
QMAKE_EXTRA_TARGETS += protocol_tests

# Build output
DESTDIR = $$PWD/build
OBJECTS_DIR = $$PWD/build/obj
MOC_DIR = $$PWD/build/moc
RCC_DIR = $$PWD/build/rcc

# Core library
SOURCES += \
    src/core/protocol.cpp \
    src/core/device.cpp \
    src/core/adb.cpp \
    src/core/media.cpp \
    src/core/config.cpp

# GUI
HEADERS += \
    src/devicemanager.h \
    src/systemmonitor.h \
    src/homepage.h \
    src/panoramapage.h \
    src/displaypage.h \
    src/firmwareupdater.h \
    src/mediatransform.h \
    src/printerprotocol.h \
    src/replacejournal.h \
    src/runtimecontract.h \
    src/runtimebridge.h \
    src/settingspage.h \
    src/traymanager.h \
    src/mainwindow.h \
    src/splitconfig.h

SOURCES += \
    src/main.cpp \
    src/devicemanager.cpp \
    src/systemmonitor.cpp \
    src/homepage.cpp \
    src/panoramapage.cpp \
    src/displaypage.cpp \
    src/firmwareupdater.cpp \
    src/mediatransform.cpp \
    src/printerprotocol.cpp \
    src/replacejournal.cpp \
    src/runtimecontract.cpp \
    src/runtimebridge.cpp \
    src/settingspage.cpp \
    src/traymanager.cpp \
    src/mainwindow.cpp \
    src/splitconfig.cpp

RESOURCES += resources/resources.qrc

DISTFILES += \
    VERSION \
    LICENSE \
    LICENSES/picojson-BSD-2-Clause.txt \
    packaging/70-tryx-pase-access.rules \
    packaging/99-tryx-pase-printer.rules \
    packaging/metainfo/io.github.dxvsi.tryx_panorama_manager.metainfo.xml \
    packaging/scripts/check-release-contract.sh \
    packaging/scripts/create-source-archive.sh \
    packaging/scripts/verify-package-contents.sh \
    packaging/tryx-panorama-manager.1 \
    packaging/tryx-panorama-manager.desktop \
    systemd/90-tryx-panorama.preset \
    systemd/tryx-panorama.service \
    tests/check_no_bundled_video.sh

unix {
    SYSTEMD_USER_UNIT_DIR = $$system(pkg-config --variable=systemduserunitdir systemd)
    isEmpty(SYSTEMD_USER_UNIT_DIR): error("systemd user unit directory was not found")
    SYSTEMD_USER_PRESET_DIR = $$system(pkg-config --variable=systemduserpresetdir systemd)
    isEmpty(SYSTEMD_USER_PRESET_DIR): error("systemd user preset directory was not found")

    target.path = /usr/bin
    pase_udev_rules.path = /usr/lib/udev/rules.d
    pase_udev_rules.files = \
        packaging/70-tryx-pase-access.rules \
        packaging/99-tryx-pase-printer.rules
    tryx_systemd_user_unit.path = $$SYSTEMD_USER_UNIT_DIR
    tryx_systemd_user_unit.files = systemd/tryx-panorama.service
    tryx_systemd_user_preset.path = $$SYSTEMD_USER_PRESET_DIR
    tryx_systemd_user_preset.files = systemd/90-tryx-panorama.preset
    tryx_desktop_entry.path = /usr/share/applications
    tryx_desktop_entry.files = packaging/tryx-panorama-manager.desktop
    tryx_icon.path = /usr/share/icons/hicolor/256x256/apps
    tryx_icon.files = resources/tryx-panorama.png
    tryx_metainfo.path = /usr/share/metainfo
    tryx_metainfo.files = \
        packaging/metainfo/io.github.dxvsi.tryx_panorama_manager.metainfo.xml
    tryx_manpage.path = /usr/share/man/man1
    tryx_manpage.files = packaging/tryx-panorama-manager.1
    tryx_license.path = /usr/share/licenses/tryx-panorama-manager
    tryx_license.files = \
        LICENSE \
        LICENSES/picojson-BSD-2-Clause.txt
    INSTALLS += target pase_udev_rules tryx_systemd_user_unit \
        tryx_systemd_user_preset \
        tryx_desktop_entry tryx_icon tryx_metainfo tryx_manpage tryx_license
}
