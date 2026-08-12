#include "core/settings/settingsstore.h"
#include "core/settings/theme/themeeditormodel.h"
#include "core/settings/theme/thememanager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUuid>
#include <QtQml>
#include <QtTest>

class ThemeManagerTest final : public QObject {
    Q_OBJECT

  private slots:
    void darkModeUsesLightIcons();
    void editorOrganizesSchemesAndSections();
    void legacyThemesMigrateToSharedBase();
    void previewResolvesThemeTokens();
    void themeEditorPageLoads();
    void settingsPageLoads();
    void searchEngineSelectionLivesInSettingsStore();
    void draftPreviewsSavesAndExports();

  private:
    void registerUiSingletons();
    bool loadPage(const char *typeName);
};

void ThemeManagerTest::registerUiSingletons() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Settings", eden::core::SettingsStore::instance());
    qmlRegisterSingletonInstance("Eden.Ui", 1, 0, "Themes", eden::core::ThemeManager::instance());
}

bool ThemeManagerTest::loadPage(const char *typeName) {
    registerUiSingletons();
    QQmlEngine engine;
    engine.addImportPath(QCoreApplication::applicationDirPath());
    QSignalSpy warnings(&engine, &QQmlEngine::warnings);
    QQmlComponent component(&engine);
    component.loadFromModule("Eden.Ui", typeName);
    if (!component.isReady()) {
        qWarning("%s", qPrintable(component.errorString()));
        return false;
    }
    QObject controller;
    std::unique_ptr<QObject> page(component.createWithInitialProperties({{"controller", QVariant::fromValue(&controller)}}));
    if (!page) {
        qWarning("%s", qPrintable(component.errorString()));
        return false;
    }
    auto *item = qobject_cast<QQuickItem *>(page.get());
    if (!item) {
        return false;
    }
    QQuickWindow window;
    window.resize(1200, 800);
    item->setParentItem(window.contentItem());
    item->setSize(window.size());
    window.show();
    QTest::qWait(100);
    const QImage rendered = window.grabWindow();
    const bool clean = !rendered.isNull() && warnings.count() == 0;
    item->setParentItem(nullptr);
    page.reset();
    return clean;
}

void ThemeManagerTest::darkModeUsesLightIcons() {
    eden::core::SettingsStore *settings = eden::core::SettingsStore::instance();
    eden::core::ThemeManager *themes = eden::core::ThemeManager::instance();
    settings->setTheme("light");
    QCOMPARE(themes->primary(), QColor("#3584E4"));
    QCOMPARE(themes->primaryContainer(), QColor("#99C1F1"));
    QCOMPARE(themes->iconColor(), QColor("#475569"));
    QCOMPARE(themes->tabBarBackground(), QColor("#33000000"));
    QVERIFY(themes->toolbarGradientEnabled());
    QCOMPARE(themes->toolbarGradientStart(), QColor("#F8FAFD"));
    QCOMPARE(themes->menuBorderWidth(), 0);
    QCOMPARE(themes->overlayShadowColor(), QColor("#42000000"));
    QCOMPARE(themes->overlayShadowBlur(), 24.0);
    settings->setTheme("dark");
    QCOMPARE(themes->iconColor(), QColor("#EDF2F7"));
    QCOMPARE(themes->tabBarBackground(), QColor("#52000000"));
    QCOMPARE(themes->toolbarGradientStart(), QColor("#222931"));
    QCOMPARE(themes->overlayShadowColor(), QColor("#82000000"));
    QCOMPARE(themes->contentBorderWidth(), 0);
    QCOMPARE(themes->paneBorderWidth(), 0);
    QCOMPARE(themes->tabMinimumWidth(), 96);
    QCOMPARE(themes->tabMaximumWidth(), 240);
    QCOMPARE(themes->pinnedTabWidth(), 44);
}

void ThemeManagerTest::editorOrganizesSchemesAndSections() {
    eden::core::ThemeManager *themes = eden::core::ThemeManager::instance();
    auto *editor = qobject_cast<eden::core::ThemeEditorModel *>(themes->editorTokens());
    QVERIFY(editor);
    QVERIFY(themes->beginEditing("eden-default"));

    QVERIFY(editor->firstIndexForSection("Core palette") >= 0);
    QCOMPARE(editor->firstIndexForSection("Overview"), -1);
    QVERIFY(!editor->sectionDescription("Surfaces").isEmpty());
    QVERIFY(editor->navigationSections().size() >= 10);

    bool foundBasePrimary = false;
    bool foundTabBarBackground = false;
    bool foundUnit = false;
    for (int row = 0; row < editor->rowCount(); ++row) {
        const QModelIndex index = editor->index(row);
        const QString path = editor->data(index, eden::core::ThemeEditorModel::PathRole).toString();
        const QString lightPath = editor->data(index, eden::core::ThemeEditorModel::LightPathRole).toString();
        const QString darkPath = editor->data(index, eden::core::ThemeEditorModel::DarkPathRole).toString();
        foundBasePrimary |= path == "base.colors.primary";
        foundTabBarBackground |= lightPath == "light.colors.tabBarBackground" && darkPath == "dark.colors.tabBarBackground";
        foundUnit |= !editor->data(index, eden::core::ThemeEditorModel::UnitRole).toString().isEmpty();
        QVERIFY(!editor->data(index, eden::core::ThemeEditorModel::DescriptionRole).toString().isEmpty());
    }
    QVERIFY(foundBasePrimary);
    QVERIFY(foundTabBarBackground);
    QVERIFY(foundUnit);
    themes->cancelEditing();
}

void ThemeManagerTest::legacyThemesMigrateToSharedBase() {
    eden::core::SettingsStore *settings = eden::core::SettingsStore::instance();
    eden::core::ThemeManager *themes = eden::core::ThemeManager::instance();
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString id = "legacy-theme-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject lightColors{{"primary", "#345678"}, {"toolbarBackground", "#ABCDEF"}, {"icon", "#111111"}};
    QJsonObject darkColors{{"primary", "#FEDCBA"}, {"toolbarBackground", "#202124"}, {"icon", "#EEEEEE"}};
    QJsonObject colors{{"light", lightColors}, {"dark", darkColors}};
    QJsonObject root{{"formatVersion", 1},     {"id", id},         {"name", "Legacy Theme"},
                     {"author", "Eden Tests"}, {"colors", colors}, {"metrics", QJsonObject{{"windowRadius", 16}}}};
    const QString path = directory.filePath("legacy.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(root).toJson()) > 0);
    file.close();
    QVERIFY(themes->installTheme(QUrl::fromLocalFile(path)));
    settings->setTheme("light");
    QCOMPARE(themes->primary(), QColor("#345678"));
    QCOMPARE(themes->toolbarGradientStart(), QColor("#ABCDEF"));
    QVERIFY(!themes->toolbarGradientEnabled());
    settings->setTheme("dark");
    QCOMPARE(themes->primary(), QColor("#345678"));
    QCOMPARE(themes->toolbarGradientStart(), QColor("#202124"));
    QVERIFY(QFile::remove(themes->themeDirectory() + "/" + id + ".json"));
    themes->refresh();
    QVERIFY(themes->activateTheme("eden-default"));
}

void ThemeManagerTest::previewResolvesThemeTokens() {
    eden::core::ThemeManager *themes = eden::core::ThemeManager::instance();
    QVERIFY(themes->themePreview("missing-theme", false).isEmpty());

    const QVariantMap dark = themes->themePreview("eden-default", true);
    QCOMPARE(dark.value("surface").value<QColor>(), QColor("#12161B"));
    QCOMPARE(dark.value("toolbarGradientStart").value<QColor>(), QColor("#222931"));
    QCOMPARE(dark.value("tabBarBackground").value<QColor>(), QColor("#52000000"));
    QCOMPARE(dark.value("primary").value<QColor>(), QColor("#3584E4"));
    QCOMPARE(dark.value("toolbarGradientEnabled").toBool(), true);
    QCOMPARE(dark.value("windowRadius").toInt(), 12);
    QCOMPARE(dark.value("controlRadius").toInt(), 20);

    const QVariantMap light = themes->themePreview("eden-default", false);
    QCOMPARE(light.value("surface").value<QColor>(), QColor("#FAFBFC"));
    QCOMPARE(light.value("toolbarGradientStart").value<QColor>(), QColor("#F8FAFD"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString id = "preview-theme-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject root{{"formatVersion", 2},
                     {"id", id},
                     {"name", "Preview Theme"},
                     {"dark", QJsonObject{{"colors", QJsonObject{{"background", "#101010"}}}}}};
    const QString path = directory.filePath("preview.json");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write(QJsonDocument(root).toJson()) > 0);
    file.close();
    QVERIFY(themes->installTheme(QUrl::fromLocalFile(path)));

    const QVariantMap partial = themes->themePreview(id, true);
    QCOMPARE(partial.value("surface").value<QColor>(), QColor("#101010"));
    QCOMPARE(partial.value("toolbarGradientStart").value<QColor>(), QColor("#222931"));
    QCOMPARE(partial.value("primary").value<QColor>(), QColor("#3584E4"));
    QCOMPARE(partial.value("windowRadius").toInt(), 12);

    QVERIFY(QFile::remove(themes->themeDirectory() + "/" + id + ".json"));
    themes->refresh();
    QVERIFY(themes->activateTheme("eden-default"));
}

void ThemeManagerTest::themeEditorPageLoads() {
    QVERIFY(loadPage("ThemeEditorPage"));
}

void ThemeManagerTest::settingsPageLoads() {
    QVERIFY(loadPage("SettingsPage"));
}

void ThemeManagerTest::searchEngineSelectionLivesInSettingsStore() {
    eden::core::SettingsStore *settings = eden::core::SettingsStore::instance();
    settings->selectSearchEngine("custom");
    QVERIFY(settings->customSearchEngine());
    QCOMPARE(settings->searchEngineActions().constLast().toMap().value("icon").toString(), QString("check"));

    settings->selectSearchEngine("Google");
    QCOMPARE(settings->searchEngine(), QString("Google"));
    QCOMPARE(settings->searchUrl(), QString("https://www.google.com/search?q=%1"));
    QVERIFY(!settings->customSearchEngine());

    const QString selectedUrl = settings->searchUrl();
    settings->selectSearchEngine("missing");
    QCOMPARE(settings->searchUrl(), selectedUrl);
    settings->selectSearchEngine("DuckDuckGo");
}

void ThemeManagerTest::draftPreviewsSavesAndExports() {
    eden::core::ThemeManager *themes = eden::core::ThemeManager::instance();
    themes->refresh();
    QVERIFY(themes->beginEditing("eden-default"));
    const QString id = "test-theme-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    themes->setEditorId(id);
    themes->setEditorName("Test Theme");
    themes->setEditorAuthor("Eden Tests");
    QVERIFY(themes->setEditorToken("dark.colors.icon", "#FEF7FF"));
    QVERIFY(themes->setEditorColor("dark.shadow.color", QColor(0, 0, 0, 144)));
    QCOMPARE(themes->editorValue("dark.shadow.color").toString(), QString("#90000000"));
    QVERIFY(themes->setEditorGradientValue("dark.chrome", "angle", 135));
    QVERIFY(themes->setEditorGradientValue("dark.chrome", "startColor", QColor(18, 52, 86, 128)));
    QCOMPARE(themes->editorValue("dark.chrome.angle").toInt(), 135);
    QCOMPARE(themes->editorValue("dark.chrome.startColor").toString(), QString("#80123456"));
    QVERIFY(!themes->setEditorToken("base.icons.style", "material"));
    QVERIFY(themes->setEditorToken("base.icons.style", "outline"));
    QVERIFY(themes->setEditorToken("base.metrics.paneBorderWidth", 2));
    QCOMPARE(themes->iconStyle(), QString("outline"));
    QCOMPARE(themes->paneBorderWidth(), 2);
    QVERIFY(themes->saveEditingAs());
    QCOMPARE(themes->activeThemeId(), id);
    QVERIFY(QFile::exists(themes->themeDirectory() + "/" + id + ".json"));

    QTemporaryDir exportDirectory;
    QVERIFY(exportDirectory.isValid());
    const QString exportPath = exportDirectory.filePath("shared-theme.json");
    QVERIFY(themes->exportEditing(QUrl::fromLocalFile(exportPath)));
    QFile exported(exportPath);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(exported.readAll()).object();
    QCOMPARE(root.value("id").toString(), id);
    QCOMPARE(root.value("formatVersion").toInt(), 2);
    QCOMPARE(root.value("base").toObject().value("icons").toObject().value("style").toString(), QString("outline"));
    themes->cancelEditing();
    QVERIFY(themes->activateTheme("eden-default"));
    QVERIFY(QFile::remove(themes->themeDirectory() + "/" + id + ".json"));
    themes->refresh();
}

QTEST_MAIN(ThemeManagerTest)

#include "thememanager_test.moc"
