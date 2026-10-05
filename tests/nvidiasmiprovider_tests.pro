QT += core testlib

CONFIG += c++17 console testcase
CONFIG -= app_bundle
DEFINES += TRYX_NVIDIA_TESTING
TEMPLATE = app
TARGET = nvidiasmiprovider-tests

INCLUDEPATH += $$PWD/../src

DESTDIR = $$PWD/../build/nvidiasmiprovider-tests
OBJECTS_DIR = $$PWD/../build/nvidiasmiprovider-tests/obj
MOC_DIR = $$PWD/../build/nvidiasmiprovider-tests/moc

HEADERS += \
    $$PWD/../src/gpuinventory.h \
    $$PWD/../src/nvidiaprocesssupervisor.h \
    $$PWD/../src/nvidiasmiparser.h \
    $$PWD/../src/nvidiasmiprovider.h

SOURCES += \
    nvidiasmiprovider_tests.cpp \
    $$PWD/../src/gpuinventory.cpp \
    $$PWD/../src/nvidiaprocesssupervisor.cpp \
    $$PWD/../src/nvidiasmiparser.cpp \
    $$PWD/../src/nvidiasmiprovider.cpp
