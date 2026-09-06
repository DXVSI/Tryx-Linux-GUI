#include "runtimebadgetext.h"
#include "runtimeapplyrequestcodec.h"

#include <QtTest>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>

class BadgeEcho final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.tryx.BadgeTest")
public slots:
    TryxRuntimeApplyWithBadgesV1 Echo(TryxRuntimeApplyWithBadgesV1 value) { return value; }
    TryxRuntimeSavedLayoutsSnapshotV2 EchoSaved(TryxRuntimeSavedLayoutsSnapshotV2 value) { return value; }
    TryxRuntimeDisplaySnapshotV1 EchoDisplay(TryxRuntimeDisplaySnapshotV1 value) { return value; }
};

class BadgeTextTests final : public QObject {
    Q_OBJECT

private slots:
    void normalizeText_data();
    void normalizeText();
    void canonicalSlots();
    void wireContract();
    void codecAndFingerprint();
    void savedLayoutWireContract();
    void coherentDisplayWireContract();
    void coherentDisplayValidation();
};

void BadgeTextTests::coherentDisplayWireContract() {
    registerTryxRuntimeMetaTypes();
    const QByteArray oldDisplay("(tsbbibsbbssasasassssasasssss)");
    QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<TryxRuntimeDisplayState>())), oldDisplay);
    const QByteArray newDisplay = "(utttsss" + oldDisplay + "(u(ss)(ss)(ss)(ss)))";
    QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<TryxRuntimeDisplaySnapshotV1>())), newDisplay);
    TryxRuntimeDisplaySnapshotV1 expected;
    expected.revision = expected.display.revision = 9;
    expected.connectionRevision = 12;
    expected.physicalGeneration = 3;
    expected.productId = QStringLiteral("391a:1021");
    expected.status = QStringLiteral("HostAccepted");
    expected.acceptedOperationId = QStringLiteral("91f96ce0-ce3b-43a5-afee-a9ecfc99eaa9");
    expected.display.valid = true;
    expected.display.deviceSerial = QStringLiteral("fixture");
    expected.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Full")};
    expected.badges.primaryGpu = {QStringLiteral("Custom"), QStringLiteral("GPU")};
    expected.badges.secondaryCpu = {QStringLiteral("Custom"), QStringLiteral("Right")};
    expected.badges.secondaryGpu = {QStringLiteral("Custom"), QStringLiteral("Вторая")};
    auto bus = QDBusConnection::sessionBus();
    BadgeEcho echo;
    const QString path = QStringLiteral("/org/tryx/BadgeDisplayTest");
    QVERIFY(bus.registerObject(path, &echo, QDBusConnection::ExportAllSlots));
    QDBusInterface interface(bus.baseService(), path, QStringLiteral("org.tryx.BadgeTest"), bus);
    const QDBusReply<TryxRuntimeDisplaySnapshotV1> reply = interface.call(QStringLiteral("EchoDisplay"), QVariant::fromValue(expected));
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), expected);
    bus.unregisterObject(path);
}

void BadgeTextTests::coherentDisplayValidation() {
    TryxRuntimeDisplaySnapshotV1 snapshot;
    QVERIFY(tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot.status = QStringLiteral("Pending");
    QVERIFY(tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("private")};
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = {};
    snapshot.status = QStringLiteral("HostAccepted");
    snapshot.productId = QStringLiteral("391a:1021");
    snapshot.revision = snapshot.display.revision = 7;
    snapshot.physicalGeneration = 1;
    snapshot.display.valid = true;
    snapshot.display.deviceSerial = QStringLiteral("fixture");
    snapshot.display.screenMode = QStringLiteral("Full Screen");
    snapshot.display.playMode = QStringLiteral("Single");
    snapshot.display.settingsPosition = QStringLiteral("Top");
    snapshot.display.settingsColor = QStringLiteral("#dcdcdc");
    snapshot.display.settingsAlign = QStringLiteral("Left");
    snapshot.display.settingsBadges = {QStringLiteral("CPU Badge")};
    snapshot.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Accepted")};
    QVERIFY(tryxDisplaySnapshotV1IsValid(snapshot));
    const auto accepted = snapshot;
    snapshot.productId = QStringLiteral("391a:1011");
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = accepted;
    snapshot.badges.primaryGpu = {QStringLiteral("Custom"), QStringLiteral("hidden")};
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = accepted;
    snapshot.display.revision = 6;
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = accepted;
    snapshot.acceptedOperationId = QStringLiteral("invalid");
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = accepted;
    snapshot.display.brightness = 101;
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = accepted;
    snapshot.badges.primaryCpu.text.prepend(QStringLiteral(" "));
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
    snapshot = accepted;
    snapshot.schemaVersion = 2;
    QVERIFY(!tryxDisplaySnapshotV1IsValid(snapshot));
}

void BadgeTextTests::savedLayoutWireContract() {
    registerTryxRuntimeMetaTypes();
    const QByteArray oldLayout("(ustsssa(usstub)(assssassssasisasassssbb(bibbbbbbb)))");
    QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<TryxRuntimeSavedLayoutV1>())), oldLayout);
    const QByteArray newLayout = oldLayout.left(oldLayout.size() - 1) + "(u(ss)(ss)(ss)(ss)))";
    QCOMPARE(QByteArray(QDBusMetaType::typeToSignature(QMetaType::fromType<TryxRuntimeSavedLayoutV2>())), newLayout);
    TryxRuntimeSavedLayoutsSnapshotV2 expected;
    TryxRuntimeSavedLayoutV2 layout;
    layout.layoutId = QStringLiteral("uuid-fixture");
    layout.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Мой ПК")};
    expected.layouts.append(layout);
    auto bus = QDBusConnection::sessionBus();
    BadgeEcho echo;
    const QString path = QStringLiteral("/org/tryx/BadgeSavedTest");
    QVERIFY(bus.registerObject(path, &echo, QDBusConnection::ExportAllSlots));
    QDBusInterface interface(bus.baseService(), path, QStringLiteral("org.tryx.BadgeTest"), bus);
    const QDBusReply<TryxRuntimeSavedLayoutsSnapshotV2> reply = interface.call(QStringLiteral("EchoSaved"), QVariant::fromValue(expected));
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), expected);
    TryxRuntimeSavedLayoutV1 legacy;
    legacy.name = QStringLiteral("unchanged");
    QVERIFY(!tryxSavedLayoutV2ToV1(layout, &legacy));
    QCOMPARE(legacy.name, QStringLiteral("unchanged"));
    bus.unregisterObject(path);
}

void BadgeTextTests::normalizeText_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("expected");
    const QString automatic = QStringLiteral("Auto");
    const QString custom = QStringLiteral("Custom");
    QTest::newRow("auto") << automatic << QString() << true << QString();
    QTest::newRow("auto-cannot-carry-hidden-text")
        << automatic << QStringLiteral("private") << false << QString();
    QTest::newRow("unknown-mode")
        << QStringLiteral("Other") << QStringLiteral("hello") << false << QString();
    QTest::newRow("wrong-case-mode")
        << QStringLiteral("custom") << QStringLiteral("hello") << false << QString();
    QTest::newRow("empty-custom") << custom << QString() << false << QString();
    QTest::newRow("blank-custom")
        << custom << QStringLiteral("   ") << false << QString();
    QTest::newRow("ascii")
        << custom << QStringLiteral("My PC") << true << QStringLiteral("My PC");
    QTest::newRow("trim-outer-spaces")
        << custom << QStringLiteral("  My  PC  ") << true << QStringLiteral("My  PC");
    QTest::newRow("cyrillic")
        << custom << QStringLiteral("Моя сборка") << true << QStringLiteral("Моя сборка");
    QTest::newRow("maximum-ascii")
        << custom << QString(32, QLatin1Char('x')) << true << QString(32, QLatin1Char('x'));
    QTest::newRow("ascii-overflow")
        << custom << QString(33, QLatin1Char('x')) << false << QString();
    const char32_t scalar = 0x1f680;
    const QString supplementary = QString::fromUcs4(&scalar, 1);
    QTest::newRow("maximum-scalars-and-utf8-bytes")
        << custom << supplementary.repeated(32) << true << supplementary.repeated(32);
    QTest::newRow("scalar-overflow")
        << custom << supplementary.repeated(33) << false << QString();
    QTest::newRow("html-is-plain-text")
        << custom << QStringLiteral("<b>PC</b>") << true << QStringLiteral("<b>PC</b>");
    const QList<char16_t> forbidden{
        0x0000, 0x0009, 0x000a, 0x000d, 0x001b, 0x007f, 0x0085,
        0x200b, 0x200d, 0x2028, 0x2029, 0x202e, 0x2066, 0xfeff,
        0xd800, 0xdc00};
    for (const char16_t point : forbidden) {
        const QByteArray name = QByteArray("forbidden-") + QByteArray::number(point, 16);
        QTest::newRow(name.constData())
            << custom << (QStringLiteral("private") + QChar(point)) << false << QString();
    }
    QTest::newRow("leading-newline-not-trimmed-away")
        << custom << QStringLiteral("\nhello") << false << QString();
    QTest::newRow("high-surrogate-before-ascii")
        << custom << (QString(QChar(0xd800)) + QStringLiteral("a")) << false << QString();
}

void BadgeTextTests::normalizeText() {
    QFETCH(QString, mode);
    QFETCH(QString, text);
    QFETCH(bool, valid);
    QFETCH(QString, expected);
    const TryxRuntimeBadgeTextV1 input{mode, text};
    TryxRuntimeBadgeTextV1 result{QStringLiteral("Custom"), QStringLiteral("sentinel")};
    const auto before = result;
    QString error;
    QCOMPARE(tryxNormalizeBadgeTextV1(input, &result, &error), valid);
    if (valid) {
        QVERIFY(error.isEmpty());
        QCOMPARE(result.mode, mode);
        QCOMPARE(result.text, expected);
    } else {
        QVERIFY(!error.isEmpty());
        QCOMPARE(result, before);
        QVERIFY(!error.contains(QStringLiteral("private")));
    }
}

void BadgeTextTests::canonicalSlots() {
    TryxRuntimeOverlayBadgesV1 input;
    input.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("  One  ")};
    input.primaryGpu = {QStringLiteral("Custom"), QStringLiteral("GPU one")};
    input.secondaryCpu = {QStringLiteral("Custom"), QStringLiteral("Two")};
    input.secondaryGpu = {QStringLiteral("Custom"), QStringLiteral("GPU two")};
    TryxRuntimeOverlayBadgesV1 output;
    const QStringList both{QStringLiteral("CPU Badge"), QStringLiteral("GPU Badge")};
    QVERIFY(tryxNormalizeOverlayBadgesV1(input, both, both, true, &output));
    QCOMPARE(output.primaryCpu.text, QStringLiteral("One"));
    QCOMPARE(output.secondaryCpu, input.secondaryCpu);
    QVERIFY(tryxNormalizeOverlayBadgesV1(input, {QStringLiteral("CPU Badge")}, both, false, &output));
    QCOMPARE(output.primaryGpu, TryxRuntimeBadgeTextV1());
    QCOMPARE(output.secondaryCpu, TryxRuntimeBadgeTextV1());
    QCOMPARE(output.secondaryGpu, TryxRuntimeBadgeTextV1());
    input.secondaryGpu.text = QStringLiteral("bad\ntext");
    QVERIFY(!tryxNormalizeOverlayBadgesV1(input, {}, {}, false, &output));
    input = {};
    input.schemaVersion = 2;
    QVERIFY(!tryxNormalizeOverlayBadgesV1(input, both, both, true, &output));
}

void BadgeTextTests::wireContract() {
    registerTryxRuntimeMetaTypes();
    const auto signature = [](QMetaType type) { return QByteArray(QDBusMetaType::typeToSignature(type)); };
    const QByteArray oldApply("(assssassssasisasassssbb(bibbbbbbb))");
    QCOMPARE(signature(QMetaType::fromType<TryxRuntimeApplyRequest>()), oldApply);
    QCOMPARE(tryxRuntimeApiVersion(), 8U);
    QCOMPARE(signature(QMetaType::fromType<TryxRuntimeBadgeTextV1>()), QByteArray("(ss)"));
    QCOMPARE(signature(QMetaType::fromType<TryxRuntimeOverlayBadgesV1>()), QByteArray("(u(ss)(ss)(ss)(ss))"));
    QCOMPARE(signature(QMetaType::fromType<TryxRuntimeApplyWithBadgesV1>()), QByteArray("(u") + oldApply + "(u(ss)(ss)(ss)(ss)))");
    TryxRuntimeApplyWithBadgesV1 expected;
    expected.request.settingsBadges = {QStringLiteral("CPU Badge")};
    expected.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("Мой CPU")};
    expected.badges.secondaryGpu = {QStringLiteral("Custom"), QStringLiteral("Other side")};
    auto bus = QDBusConnection::sessionBus();
    QVERIFY(bus.isConnected());
    BadgeEcho echo;
    const QString path = QStringLiteral("/org/tryx/BadgeTest");
    QVERIFY(bus.registerObject(path, &echo, QDBusConnection::ExportAllSlots));
    QDBusInterface interface(bus.baseService(), path, QStringLiteral("org.tryx.BadgeTest"), bus);
    const QDBusReply<TryxRuntimeApplyWithBadgesV1> reply = interface.call(QStringLiteral("Echo"), QVariant::fromValue(expected));
    bus.unregisterObject(path);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    QCOMPARE(reply.value(), expected);
}

void BadgeTextTests::codecAndFingerprint() {
    using namespace tryx::runtime_apply_request_codec;
    TryxRuntimeApplyWithBadgesV1 request;
    request.badges.primaryCpu = {QStringLiteral("Custom"), QStringLiteral("private text")};
    const auto json = runtimeApplyWithBadgesV1ToJson(request);
    QCOMPARE(json.value(QStringLiteral("request")).toObject(), runtimeApplyRequestToJson(request.request));
    TryxRuntimeApplyWithBadgesV1 decoded;
    QVERIFY(runtimeApplyWithBadgesV1FromJson(json, &decoded));
    QCOMPARE(decoded, request);
    const auto before = runtimeApplyWithBadgesV1Fingerprint(request);
    request.badges.secondaryCpu = request.badges.primaryCpu;
    QVERIFY(runtimeApplyWithBadgesV1Fingerprint(request) != before);
    auto invalid = json;
    invalid.insert(QStringLiteral("schemaVersion"), 2);
    QVERIFY(!runtimeApplyWithBadgesV1FromJson(invalid, &decoded));
    invalid = json;
    invalid.insert(QStringLiteral("extra"), true);
    QVERIFY(!runtimeApplyWithBadgesV1FromJson(invalid, &decoded));
    invalid = json;
    invalid.remove(QStringLiteral("badges"));
    QVERIFY(!runtimeApplyWithBadgesV1FromJson(invalid, &decoded));
    QCOMPARE(decoded.badges.primaryCpu.text, QStringLiteral("private text"));
}

QTEST_GUILESS_MAIN(BadgeTextTests)
#include "badgetext_tests.moc"
