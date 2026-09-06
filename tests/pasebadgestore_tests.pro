QT += core gui dbus testlib
CONFIG += c++17 console testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = pasebadgestore-tests
DEFINES += TRYX_CONFIGURATION_FORMAT_BACKUP_TESTING
INCLUDEPATH += $$PWD/../src
DESTDIR = $$PWD/../build/pasebadgestore-tests
OBJECTS_DIR = $$PWD/../build/pasebadgestore-tests/obj
MOC_DIR = $$PWD/../build/pasebadgestore-tests/moc
SOURCES += $$PWD/pasebadgestore_tests.cpp \
    $$PWD/../src/pasemetricsconfigstore.cpp \
    $$PWD/../src/configurationformatbackup.cpp \
    $$PWD/../src/paseoverlayconfig.cpp \
    $$PWD/../src/runtimeapplyrequestcodec.cpp \
    $$PWD/../src/runtimecontract.cpp \
    $$PWD/../src/runtimebadgetext.cpp
