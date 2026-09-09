#include "core/settings/settingsstore.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QStyleHints>

#include <algorithm>

namespace eden::core {

    static QString settingsDirectory() {
        return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/eden";
    }

    SettingsStore::SettingsStore(QObject *parent)
        : QObject(parent) {
        connect(
            QGuiApplication::styleHints(),
            &QStyleHints::colorSchemeChanged,
            this,
            &SettingsStore::systemDarkChanged
        );
    }

    SettingsStore::~SettingsStore() = default;

    SettingsStore *SettingsStore::instance() {
        static SettingsStore settings;
        return &settings;
    }

    void SettingsStore::initialize() {
        if (m_settings) {
            return;
        }
        const QString directory = settingsDirectory();
        QDir().mkpath(directory);
        m_settings = std::make_unique<QSettings>(directory + "/eden.conf", QSettings::IniFormat);

        const QString storedTheme = m_settings->value("appearance/theme", m_theme).toString();
        const QString nextTheme = storedTheme == "system" || storedTheme == "light" || storedTheme == "dark"
                                      ? storedTheme
                                      : QString("system");
        const QString nextThemeId = m_settings->value("appearance/themeId", m_themeId).toString();

        const auto updateString = [this](QString &current, const QString &next, void (SettingsStore::*signal)()) {
            if (current != next) {
                current = next;
                emit(this->*signal)();
            }
        };
        updateString(m_theme, nextTheme, &SettingsStore::themeChanged);
        updateString(m_themeId, nextThemeId, &SettingsStore::themeIdChanged);
        const QString storedPlacement = m_settings->value("devtools/placement", m_devToolsPlacement).toString();
        updateString(
            m_devToolsPlacement,
            storedPlacement == "bottom" ? QString("bottom") : QString("right"),
            &SettingsStore::devToolsPlacementChanged
        );
        m_devToolsPaneWidth =
            std::clamp(m_settings->value("devtools/paneWidth", m_devToolsPaneWidth).toInt(), 280, 1600);
        m_devToolsPaneHeight =
            std::clamp(m_settings->value("devtools/paneHeight", m_devToolsPaneHeight).toInt(), 180, 1200);
        m_windowWidth = std::clamp(m_settings->value("window/width", m_windowWidth).toInt(), 600, 16384);
        m_windowHeight = std::clamp(m_settings->value("window/height", m_windowHeight).toInt(), 440, 16384);
        m_windowMaximized = m_settings->value("window/maximized", m_windowMaximized).toBool();
    }

    QString SettingsStore::theme() const {
        return m_theme;
    }

    QString SettingsStore::themeId() const {
        return m_themeId;
    }

    bool SettingsStore::systemDark() const {
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }

    QString SettingsStore::licenseText(const QString &fileName) const {
        if (fileName.contains('/') || fileName.contains("..")) {
            return {};
        }
        QFile file(":/qt/qml/Eden/Ui/resources/licenses/" + fileName);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

    void SettingsStore::setTheme(const QString &theme) {
        if (theme != "system" && theme != "light" && theme != "dark") {
            return;
        }
        setString("appearance/theme", theme, m_theme, &SettingsStore::themeChanged);
    }

    void SettingsStore::setThemeId(const QString &themeId) {
        setString("appearance/themeId", themeId, m_themeId, &SettingsStore::themeIdChanged);
    }

    QString SettingsStore::devToolsPlacement() const {
        return m_devToolsPlacement;
    }

    int SettingsStore::devToolsPaneWidth() const {
        return m_devToolsPaneWidth;
    }

    int SettingsStore::devToolsPaneHeight() const {
        return m_devToolsPaneHeight;
    }

    void SettingsStore::setDevToolsPlacement(const QString &placement) {
        if (placement != "right" && placement != "bottom") {
            return;
        }
        setString("devtools/placement", placement, m_devToolsPlacement, &SettingsStore::devToolsPlacementChanged);
    }

    void SettingsStore::setDevToolsPaneSize(int width, int height) {
        const int nextWidth = std::clamp(width, 280, 1600);
        const int nextHeight = std::clamp(height, 180, 1200);
        if (nextWidth == m_devToolsPaneWidth && nextHeight == m_devToolsPaneHeight) {
            return;
        }
        m_devToolsPaneWidth = nextWidth;
        m_devToolsPaneHeight = nextHeight;
        persist("devtools/paneWidth", nextWidth);
        persist("devtools/paneHeight", nextHeight);
    }

    int SettingsStore::windowWidth() const {
        return m_windowWidth;
    }

    int SettingsStore::windowHeight() const {
        return m_windowHeight;
    }

    bool SettingsStore::windowMaximized() const {
        return m_windowMaximized;
    }

    void SettingsStore::setWindowGeometry(int width, int height, bool maximized) {
        const int nextWidth = std::clamp(width, 600, 16384);
        const int nextHeight = std::clamp(height, 440, 16384);
        if (nextWidth == m_windowWidth && nextHeight == m_windowHeight && maximized == m_windowMaximized) {
            return;
        }
        m_windowWidth = nextWidth;
        m_windowHeight = nextHeight;
        m_windowMaximized = maximized;
        persist("window/width", nextWidth);
        persist("window/height", nextHeight);
        persist("window/maximized", maximized);
    }

    void SettingsStore::persist(const QString &key, const QVariant &value) {
        if (!m_settings) {
            return;
        }
        m_settings->setValue(key, value);
        m_settings->sync();
    }

    void SettingsStore::setString(
        const QString &key,
        const QString &value,
        QString &current,
        void (SettingsStore::*signal)()
    ) {
        if (value == current) {
            return;
        }
        current = value;
        persist(key, value);
        emit(this->*signal)();
    }

}
