QT += concurrent core dbus gui qml quick quickcontrols2

CONFIG += c++17 lrelease embed_translations
TARGET = tryx-panorama-manager
TEMPLATE = app

!versionAtLeast(QT_VERSION, 6.4.0) {
    error("tryx-panorama-manager requires Qt 6.4 or newer")
}

VERSION = $$cat($$PWD/VERSION, lines)
isEmpty(VERSION): error("VERSION is empty or missing")
DEFINES += TRYX_APP_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD/src $$PWD/src/quick
INCLUDEPATH += $$PWD/include

TRANSLATIONS += translations/tryx-panorama_ru.ts
LRELEASE_DIR = build/quick/i18n

DESTDIR = $$PWD/build/quick
OBJECTS_DIR = $$PWD/build/quick/obj
MOC_DIR = $$PWD/build/quick/moc
RCC_DIR = $$PWD/build/quick/rcc

HEADERS += \
    src/applicationpaths.h \
    src/systemmonitor.h \
    src/runtimecontract.h \
    src/mediatransform.h \
    src/quick/appsettingscontroller.h \
    src/quick/devicemediaworkflowcontroller.h \
    src/quick/firmwarecontroller.h \
    src/quick/linuxtraycontroller.h \
    src/quick/mediacatalogmodel.h \
    src/quick/mediaeditorcontroller.h \
    src/quick/mediapreviewcontroller.h \
    src/quick/operationlistmodel.h \
    src/quick/runtimebootstrap.h \
    src/quick/runtimeclient.h \
    src/quick/systemmetricsmodel.h \
    src/quick/windowchromecontroller.h

SOURCES += \
    src/systemmonitor.cpp \
    src/runtimecontract.cpp \
    src/mediatransform.cpp \
    src/core/config.cpp \
    src/quick/appsettingscontroller.cpp \
    src/quick/devicemediaworkflowcontroller.cpp \
    src/quick/firmwarecontroller.cpp \
    src/quick/linuxtraycontroller.cpp \
    src/quick/main.cpp \
    src/quick/mediacatalogmodel.cpp \
    src/quick/mediaeditorcontroller.cpp \
    src/quick/mediapreviewcontroller.cpp \
    src/quick/operationlistmodel.cpp \
    src/quick/runtimebootstrap.cpp \
    src/quick/runtimeclient.cpp \
    src/quick/systemmetricsmodel.cpp \
    src/quick/windowchromecontroller.cpp

RESOURCES += resources/quick.qrc

QML_FILES = \
    qml/Main.qml \
    qml/components/MediaEditor.qml \
    qml/components/MediaExportPicker.qml \
    qml/components/MediaFilePicker.qml \
    qml/components/FirmwareFilePicker.qml \
    qml/components/FirmwarePanel.qml \
    qml/components/MetricCard.qml \
    qml/components/NavButton.qml \
    qml/components/OperationBanner.qml \
    qml/components/PrimaryButton.qml \
    qml/components/WindowResizeHandle.qml \
    qml/pages/HomePage.qml \
    qml/pages/PanoramaPage.qml \
    qml/pages/SettingsPage.qml

QML_TEST_FILES = \
    tests/quick/qml/tst_firmwarefilepickerlayout.qml \
    tests/quick/qml/tst_homepagelayout.qml \
    tests/quick/qml/tst_mediaeditorlayout.qml \
    tests/quick/qml/tst_mediaexportpickerlayout.qml \
    tests/quick/qml/tst_mediafilepickerlayout.qml \
    tests/quick/qml/tst_panoramalayout.qml \
    tests/quick/qml/tst_settingslayout.qml

QML_ALL_FILES = $$QML_FILES $$QML_TEST_FILES
QML_LINT_FILES =
for(qml_file, QML_ALL_FILES) {
    QML_LINT_FILES += $$shell_path($$absolute_path($$qml_file, $$PWD))
}

DISTFILES += \
    $$QML_FILES \
    resources/tryx-panorama.png \
    tests/quick/quick_tests.pro \
    tests/quick/linuxtraycontroller_tests.pro \
    tests/quick/linuxtraycontroller_tests.cpp \
    tests/quick/tst_quickmodels.cpp \
    $$QML_TEST_FILES

QMLLINT = $$[QT_HOST_BINS]/qmllint
QMLTESTRUNNER = $$[QT_HOST_BINS]/qmltestrunner
QMLIMPORTSCANNER = $$[QT_HOST_LIBEXECS]/qmlimportscanner
QMLLINT_FLAGS =
versionAtLeast(QT_VERSION, 6.8.0) {
    QMLLINT_FLAGS += -W 0
} else {
    QMLLINT_FLAGS += --deferred-property-id info
}
exists($$QMLLINT) {
    qml_lint.target = qml-lint
    qml_lint.commands = \
        $$QMLLINT $$QMLLINT_FLAGS $$QML_LINT_FILES
    QMAKE_EXTRA_TARGETS += qml_lint
}

quick_tests.target = quick-check
quick_tests.depends = \
    $$relative_path($$DESTDIR/$$TARGET, $$OUT_PWD)
quick_tests.commands = \
    $$QMLLINT $$QMLLINT_FLAGS $$QML_LINT_FILES && \
    QT_QPA_PLATFORM=offscreen $$QMLTESTRUNNER \
        -input $$shell_path($$PWD/tests/quick/qml) \
        -import $$shell_path($$PWD/qml) -o -,txt && \
    cd $$shell_path($$PWD/tests/quick) && \
        $$QMAKE_QMAKE quick_tests.pro && $(MAKE) && \
        TRYX_QMLIMPORTSCANNER=$$shell_path($$QMLIMPORTSCANNER) \
        TRYX_QML_ROOT=$$shell_path($$PWD/qml) \
        TRYX_QML_IMPORT_PATH=$$shell_path($$[QT_INSTALL_QML]) \
            $$shell_path($$PWD/build/quick-tests/tryx-quick-tests) && \
        $$QMAKE_QMAKE linuxtraycontroller_tests.pro && $(MAKE) && \
        dbus-run-session -- \
            $$shell_path($$PWD/build/linuxtray-tests/linuxtraycontroller-tests) \
                -txt && \
    env -u DBUS_SESSION_BUS_ADDRESS QT_QPA_PLATFORM=offscreen \
        $$shell_path($$PWD/build/quick/tryx-panorama-manager) \
        --smoke-test
QMAKE_EXTRA_TARGETS += quick_tests

unix {
    target.path = /usr/bin
    INSTALLS += target
}
