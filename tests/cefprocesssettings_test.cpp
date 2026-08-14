#include "engine/cef/cefprocesssettings.h"

#include "include/cef_version.h"
#include "include/internal/cef_string_wrappers.h"

#include <QTemporaryDir>
#include <QtTest>

#include <array>
#include <filesystem>
#include <fstream>
#include <span>

class CefProcessSettingsTest final : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase();
    void usesDedicatedHelperWithSandbox();
    void configuresUserAgentProduct();
    void explicitFlagDisablesSandbox();
    void configuresThreadingAndRootCache();
    void defaultsToWindowlessRendering();
    void explicitWindowedModeDisablesWindowlessRendering();
    void rejectsRelativeRootCache();
    void rejectsMissingHelper();

  private:
    QTemporaryDir temporaryDirectory;
    std::filesystem::path browserPath;
    std::filesystem::path helperPath;
    eden::engine::cef::CefProcessSettingsResult
    createSettings(std::span<const std::string_view> arguments, const std::filesystem::path &rootCachePath = {}) const;
};

void CefProcessSettingsTest::initTestCase() {
    QVERIFY(temporaryDirectory.isValid());
    const std::filesystem::path directoryPath = temporaryDirectory.path().toStdString();
    browserPath = directoryPath / "eden-browser";
    helperPath = directoryPath / "eden-helper";
    std::ofstream helper(helperPath);
    QVERIFY(helper.good());
}

eden::engine::cef::CefProcessSettingsResult CefProcessSettingsTest::createSettings(
    std::span<const std::string_view> arguments,
    const std::filesystem::path &rootCachePath
) const {
    return eden::engine::cef::createCefProcessSettings(browserPath, "Eden", "0.3.0", arguments, rootCachePath);
}

void CefProcessSettingsTest::usesDedicatedHelperWithSandbox() {
    const std::array<std::string_view, 1> arguments{"--engine=cef"};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments);
    QVERIFY(result.valid);
    QCOMPARE(
        QString::fromStdString(CefString(&result.settings.browser_subprocess_path).ToString()),
        QString::fromStdString(helperPath.string())
    );
    QCOMPARE(result.settings.no_sandbox, 0);
    QCOMPARE(result.settings.remote_debugging_port, 0);
}

void CefProcessSettingsTest::configuresUserAgentProduct() {
    const std::array<std::string_view, 0> arguments{};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments);
    QVERIFY(result.valid);
    const QString expected = QStringLiteral("Chrome/%1.%2.%3.%4 Eden/0.3.0")
                                 .arg(CHROME_VERSION_MAJOR)
                                 .arg(CHROME_VERSION_MINOR)
                                 .arg(CHROME_VERSION_BUILD)
                                 .arg(CHROME_VERSION_PATCH);
    QCOMPARE(QString::fromStdString(CefString(&result.settings.user_agent_product).ToString()), expected);
}

void CefProcessSettingsTest::explicitFlagDisablesSandbox() {
    const std::array<std::string_view, 2> arguments{"--engine=cef", "--no-sandbox"};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments);
    QVERIFY(result.valid);
    QCOMPARE(result.settings.no_sandbox, 1);
}

void CefProcessSettingsTest::configuresThreadingAndRootCache() {
    const std::array<std::string_view, 0> arguments{};
    const std::filesystem::path rootCachePath =
        std::filesystem::path(temporaryDirectory.path().toStdString()) / "cef-root";
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments, rootCachePath);
    QVERIFY(result.valid);
    QCOMPARE(result.settings.multi_threaded_message_loop, 1);
    QCOMPARE(
        QString::fromStdString(CefString(&result.settings.root_cache_path).ToString()),
        QString::fromStdString(rootCachePath.string())
    );
}

void CefProcessSettingsTest::defaultsToWindowlessRendering() {
    const std::array<std::string_view, 1> arguments{"--engine=cef"};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments);
    QVERIFY(result.valid);
    QCOMPARE(result.settings.windowless_rendering_enabled, 1);
    QCOMPARE(result.settings.multi_threaded_message_loop, 1);
}

void CefProcessSettingsTest::explicitWindowedModeDisablesWindowlessRendering() {
    const std::array<std::string_view, 2> arguments{"--engine=cef", "--engine-compositing=windowed"};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments);
    QVERIFY(result.valid);
    QCOMPARE(result.settings.windowless_rendering_enabled, 0);
}

void CefProcessSettingsTest::rejectsRelativeRootCache() {
    const std::array<std::string_view, 0> arguments{};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments, "relative");
    QVERIFY(!result.valid);
}

void CefProcessSettingsTest::rejectsMissingHelper() {
    std::error_code error;
    QVERIFY(std::filesystem::remove(helperPath, error));
    QVERIFY(!error);
    const std::array<std::string_view, 0> arguments{};
    const eden::engine::cef::CefProcessSettingsResult result = createSettings(arguments);
    QVERIFY(!result.valid);
}

QTEST_GUILESS_MAIN(CefProcessSettingsTest)

#include "cefprocesssettings_test.moc"
