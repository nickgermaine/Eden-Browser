#include "engine/fontpolicy.h"

#include <QtTest>

using eden::engine::FontHinting;
using eden::engine::FontPolicy;

class FontPolicyTest final : public QObject {
    Q_OBJECT

  private slots:
    void parsesPangoDescriptions_data();
    void parsesPangoDescriptions();
    void resolvesGenericFamiliesThroughFontconfig();
    void cachesResolutionsUntilReset();
    void pinsGrayscaleAntialiasingAndSlightHinting();
    void reportsAUsableSystemFont();
};

void FontPolicyTest::parsesPangoDescriptions_data() {
    QTest::addColumn<QString>("description");
    QTest::addColumn<QString>("family");

    QTest::newRow("family and size") << "Google Sans 11" << "Google Sans";
    QTest::newRow("family only") << "Adwaita Sans" << "Adwaita Sans";
    QTest::newRow("style and size") << "Cantarell Bold Italic 11" << "Cantarell";
    QTest::newRow("fractional size") << "Noto Sans CJK JP 10.5" << "Noto Sans CJK JP";
    QTest::newRow("extra spacing") << "  DejaVu  Sans   12 " << "DejaVu Sans";
    QTest::newRow("size only") << "11" << QString();
    QTest::newRow("style only") << "Bold" << QString();
    QTest::newRow("empty") << QString() << QString();
}

void FontPolicyTest::parsesPangoDescriptions() {
    QFETCH(QString, description);
    QFETCH(QString, family);
    QCOMPARE(FontPolicy::familyFromDescription(description), family);
}

void FontPolicyTest::resolvesGenericFamiliesThroughFontconfig() {
    const QStringList generics = {
        QStringLiteral("sans-serif"),
        QStringLiteral("serif"),
        QStringLiteral("monospace"),
        QStringLiteral("cursive"),
        QStringLiteral("fantasy")
    };
    for (const QString &generic : generics) {
        const QString resolved = FontPolicy::resolveGenericFamily(generic);
        QVERIFY2(!resolved.isEmpty(), qPrintable(generic));
        QVERIFY2(resolved != generic, qPrintable(generic + " was not resolved to an installed family"));
    }
    QCOMPARE(FontPolicy::sansSerifFamily(), FontPolicy::resolveGenericFamily(QStringLiteral("sans-serif")));
    QCOMPARE(FontPolicy::serifFamily(), FontPolicy::resolveGenericFamily(QStringLiteral("serif")));
    QCOMPARE(FontPolicy::monospaceFamily(), FontPolicy::resolveGenericFamily(QStringLiteral("monospace")));
    QCOMPARE(FontPolicy::standardFamily(), FontPolicy::serifFamily());
    QCOMPARE(FontPolicy::resolveGenericFamily(QString()), QString());
    const QString substituted = FontPolicy::resolveGenericFamily(QStringLiteral("Eden Font That Is Not Installed"));
    QVERIFY(!substituted.isEmpty());
    QCOMPARE(substituted, FontPolicy::resolveGenericFamily(QStringLiteral("Eden Font That Is Not Installed")));
}

void FontPolicyTest::cachesResolutionsUntilReset() {
    const QString first = FontPolicy::sansSerifFamily();
    QCOMPARE(FontPolicy::sansSerifFamily(), first);
    const QString description = FontPolicy::systemDescription();
    FontPolicy::resetCache();
    QCOMPARE(FontPolicy::sansSerifFamily(), first);
    QCOMPARE(FontPolicy::systemDescription(), description);
}

void FontPolicyTest::pinsGrayscaleAntialiasingAndSlightHinting() {
    QCOMPARE(FontPolicy::subpixelAntialiasing(), false);
    QCOMPARE(FontPolicy::hinting(), FontHinting::Slight);
    QCOMPARE(FontPolicy::antialiasing(), true);
}

void FontPolicyTest::reportsAUsableSystemFont() {
    const QString description = FontPolicy::systemDescription();
    QVERIFY(!description.isEmpty());
    const QString family = FontPolicy::familyFromDescription(description);
    QVERIFY(!family.isEmpty());
    QCOMPARE(FontPolicy::systemFamily(), family);
}

QTEST_MAIN(FontPolicyTest)
#include "fontpolicy_test.moc"
