QT += core dbus gui testlib

CONFIG += c++17 console testcase link_pkgconfig
CONFIG -= app_bundle
TEMPLATE = app
TARGET = printerprotocol-tests

PKGCONFIG += protobuf libudev libusb-1.0
DEFINES += TRYX_PROTOCOL_TESTING

PROTOC_VERSION = $$system(protoc --version)
PROTOC_VERSION = $$last(PROTOC_VERSION)
PROTOBUF_RUNTIME_VERSION = $$system(pkg-config --modversion protobuf)
isEmpty(PROTOC_VERSION): error("protoc was not found")
isEmpty(PROTOBUF_RUNTIME_VERSION): error("protobuf pkg-config metadata was not found")
PROTOC_NORMALIZED_VERSION = $$PROTOC_VERSION
PROTOC_PATCH_VERSION = $$section(PROTOC_VERSION, ., 2, 2)
isEmpty(PROTOC_PATCH_VERSION): PROTOC_NORMALIZED_VERSION = $${PROTOC_VERSION}.0
PROTOBUF_RUNTIME_NORMALIZED_VERSION = $$PROTOBUF_RUNTIME_VERSION
PROTOBUF_RUNTIME_PATCH_VERSION = $$section(PROTOBUF_RUNTIME_VERSION, ., 2, 2)
isEmpty(PROTOBUF_RUNTIME_PATCH_VERSION): PROTOBUF_RUNTIME_NORMALIZED_VERSION = $${PROTOBUF_RUNTIME_VERSION}.0
!equals(PROTOC_NORMALIZED_VERSION, $$PROTOBUF_RUNTIME_NORMALIZED_VERSION): error("protoc $$PROTOC_VERSION does not match libprotobuf $$PROTOBUF_RUNTIME_VERSION")

DESTDIR = $$PWD/../build/tests
OBJECTS_DIR = $$PWD/../build/tests/obj
MOC_DIR = $$PWD/../build/tests/moc

PROTO_DIR = $$PWD/../protocol/wire-v1
PROTO_GEN_DIR = $$PWD/../build/tests/generated/proto
PROTO_FILES = \
    $$PROTO_DIR/configuration.proto \
    $$PROTO_DIR/overlay.proto \
    $$PROTO_DIR/transport.proto

INCLUDEPATH += $$PWD/../src $$PWD/../include $$PROTO_GEN_DIR
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

HEADERS += \
    $$PWD/../src/deleteintentstore.h \
    $$PWD/../src/devicemediaartifactstore.h \
    $$PWD/../src/devicemanagermessages.h \
    $$PWD/../src/printerprotocol.h \
    $$PWD/../src/printermediaupload.h \
    $$PWD/../src/turrismediaclient.h \
    $$PWD/../src/paseconfigurationclient.h \
    $$PWD/../src/pasemediaclient.h \
    $$PWD/../src/printertransactionchannel.h \
    $$PWD/../src/printermediahelpers_p.h \
    $$PWD/../src/usbprintertransport.h \
    $$PWD/../src/printeroperation_p.h \
    $$PWD/../src/printerprotocolconstants_p.h \
    $$PWD/../src/printerproductprofile.h \
    $$PWD/../src/printerframecodec.h \
    $$PWD/../src/printerframecodec_p.h \
    $$PWD/../src/printerdiscovery_p.h \
    $$PWD/../src/runtimebridge.h \
    $$PWD/../src/devicemanager.h \
    $$PWD/../src/deviceworker.h \
    $$PWD/../src/legacydevicesession.h \
    $$PWD/../src/printerclasssession.h \
    $$PWD/../src/deviceworkersessioncontext_p.h \
    $$PWD/../src/deviceworkermetrics_p.h \
    $$PWD/../src/firmwarebridge.h \
    $$PWD/../src/firmwarerecoveryjournal.h \
    $$PWD/../src/firmwareupdater.h \
    $$PWD/../src/gpuinventory.h \
    $$PWD/../src/nvidiaprocesssupervisor.h \
    $$PWD/../src/nvidiasmiparser.h \
    $$PWD/../src/nvidiasmiprovider.h \
    $$PWD/../src/systemmonitor.h \
    $$PWD/../src/mediacatalogstore.h \
    $$PWD/../src/mediatransform.h \
    $$PWD/../src/paseoverlayconfig.h \
    $$PWD/../src/pasemetricsconfigstore.h \
    $$PWD/../src/privateruntimepaths.h \
    $$PWD/../src/printermediafileintegrity.h \
    $$PWD/../src/printermediaidentity.h \
    $$PWD/../src/printermediapreparer.h \
    $$PWD/../src/printermediavalidator.h \
    $$PWD/../src/printeroperationcoordinator.h \
    $$PWD/../src/printersessioncontroller.h \
    $$PWD/../src/printerlifecycle_p.h \
    $$PWD/../src/replacejournal.h \
    $$PWD/../src/retrycachestore.h \
    $$PWD/../src/retrycachetransitionstore.h \
    $$PWD/../src/runtimeapplyrequestcodec.h \
    $$PWD/../src/runtimedowngradestore.h \
    $$PWD/../src/runtimepresentationpreferencesstore.h \
    $$PWD/../src/savedlayoutstore.h \
    $$PWD/../src/supportsnapshot.h \
    $$PWD/../src/runtimecontract.h \
    $$PWD/../src/turrismediaformat.h
SOURCES += \
    printerprotocol_tests.cpp \
    $$PWD/../src/deleteintentstore.cpp \
    $$PWD/../src/devicemediaartifactstore.cpp \
    $$PWD/../src/printerprotocol.cpp \
    $$PWD/../src/printermediaupload.cpp \
    $$PWD/../src/turrismediaclient.cpp \
    $$PWD/../src/paseconfigurationclient.cpp \
    $$PWD/../src/pasemediaclient.cpp \
    $$PWD/../src/printeroperation.cpp \
    $$PWD/../src/printertransactionchannel.cpp \
    $$PWD/../src/printermediahelpers.cpp \
    $$PWD/../src/usbprintertransport.cpp \
    $$PWD/../src/printerproductprofile.cpp \
    $$PWD/../src/printerframecodec.cpp \
    $$PWD/../src/printerdiscovery.cpp \
    $$PWD/../src/runtimebridge.cpp \
    $$PWD/../src/devicemanager.cpp \
    $$PWD/../src/deviceworker.cpp \
    $$PWD/../src/legacydevicesession.cpp \
    $$PWD/../src/printerclasssession.cpp \
    $$PWD/../src/deviceworkermetrics.cpp \
    $$PWD/../src/firmwarebridge.cpp \
    $$PWD/../src/firmwarerecoveryjournal.cpp \
    $$PWD/../src/firmwareupdater.cpp \
    $$PWD/../src/gpuinventory.cpp \
    $$PWD/../src/nvidiaprocesssupervisor.cpp \
    $$PWD/../src/nvidiasmiparser.cpp \
    $$PWD/../src/nvidiasmiprovider.cpp \
    $$PWD/../src/systemmonitor.cpp \
    $$PWD/../src/mediacatalogstore.cpp \
    $$PWD/../src/mediatransform.cpp \
    $$PWD/../src/paseoverlayconfig.cpp \
    $$PWD/../src/pasemetricsconfigstore.cpp \
    $$PWD/../src/configurationformatbackup.cpp \
    $$PWD/../src/privateruntimepaths.cpp \
    $$PWD/../src/printermediafileintegrity.cpp \
    $$PWD/../src/printermediaidentity.cpp \
    $$PWD/../src/printermediapreparer.cpp \
    $$PWD/../src/printermediavalidator.cpp \
    $$PWD/../src/printeroperationcoordinator.cpp \
    $$PWD/../src/printersessioncontroller.cpp \
    $$PWD/../src/replacejournal.cpp \
    $$PWD/../src/retrycachestore.cpp \
    $$PWD/../src/retrycachetransitionstore.cpp \
    $$PWD/../src/runtimeapplyrequestcodec.cpp \
    $$PWD/../src/runtimedowngradestore.cpp \
    $$PWD/../src/runtimepresentationpreferencesstore.cpp \
    $$PWD/../src/savedlayoutstore.cpp \
    $$PWD/../src/supportsnapshot.cpp \
    $$PWD/../src/runtimebadgetext.cpp \
    $$PWD/../src/runtimecontract.cpp \
    $$PWD/../src/turrismediaformat.cpp \
    $$PWD/../src/core/protocol.cpp \
    $$PWD/../src/core/device.cpp \
    $$PWD/../src/core/adb.cpp \
    $$PWD/../src/core/media.cpp \
    $$PWD/../src/core/config.cpp
