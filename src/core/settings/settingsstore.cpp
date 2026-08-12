#include "core/settings/settingsstore.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QStyleHints>

#include <algorithm>
#include <array>

namespace eden::core {

struct SearchEnginePreset {
    QString name;
    QString url;
};

static const std::array<SearchEnginePreset, 6> &searchEnginePresets() {
    static const std::array<SearchEnginePreset, 6> presets = {
        SearchEnginePreset{"DuckDuckGo", "https://duckduckgo.com/?q=%1"},
        SearchEnginePreset{"Google", "https://www.google.com/search?q=%1"},
        SearchEnginePreset{"Bing", "https://www.bing.com/search?q=%1"},
        SearchEnginePreset{"Brave Search", "https://search.brave.com/search?q=%1"},
        SearchEnginePreset{"Startpage", "https://www.startpage.com/sp/search?query=%1"},
        SearchEnginePreset{"Ecosia", "https://www.ecosia.org/search?q=%1"},
    };
    return presets;
}

static const SearchEnginePreset *searchEnginePreset(const QString &id) {
    const auto &presets = searchEnginePresets();
    const auto found =
        std::find_if(presets.cbegin(), presets.cend(), [&id](const SearchEnginePreset &preset) { return preset.name == id; });
    return found == presets.cend() ? nullptr : &*found;
}

static bool isPresetSearchUrl(const QString &url) {
    const auto &presets = searchEnginePresets();
    return std::any_of(presets.cbegin(), presets.cend(), [&url](const SearchEnginePreset &preset) { return preset.url == url; });
}

static QString settingsDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/eden";
}

SettingsStore::SettingsStore(QObject *parent)
    : QObject(parent) {
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &SettingsStore::systemDarkChanged);
    connect(this, &SettingsStore::searchEngineChanged, this, &SettingsStore::searchEngineActionsChanged);
    connect(this, &SettingsStore::searchUrlChanged, this, &SettingsStore::searchEngineActionsChanged);
    connect(this, &SettingsStore::searchUrlChanged, this, &SettingsStore::customSearchEngineChanged);
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
    const QString nextTheme = storedTheme == "system" || storedTheme == "light" || storedTheme == "dark" ? storedTheme : QString("system");
    const QString storedLayout = m_settings->value("appearance/tabLayout", m_tabLayout).toString();
    const QString nextLayout = storedLayout == "horizontal" || storedLayout == "sidebar" ? storedLayout : QString("horizontal");
    const QString nextThemeId = m_settings->value("appearance/themeId", m_themeId).toString();
    const QString nextSearchEngine = m_settings->value("search/name", m_searchEngine).toString();
    const QString nextSearchUrl = m_settings->value("search/url", m_searchUrl).toString();
    const bool nextSearchSuggestions = m_settings->value("search/suggestions", m_searchSuggestions).toBool();

    const auto updateString = [this](QString &current, const QString &next, void (SettingsStore::*signal)()) {
        if (current != next) {
            current = next;
            emit(this->*signal)();
        }
    };
    updateString(m_theme, nextTheme, &SettingsStore::themeChanged);
    updateString(m_themeId, nextThemeId, &SettingsStore::themeIdChanged);
    updateString(m_tabLayout, nextLayout, &SettingsStore::tabLayoutChanged);
    updateString(m_searchEngine, nextSearchEngine, &SettingsStore::searchEngineChanged);
    updateString(m_searchUrl, nextSearchUrl, &SettingsStore::searchUrlChanged);
    if (m_searchSuggestions != nextSearchSuggestions) {
        m_searchSuggestions = nextSearchSuggestions;
        emit searchSuggestionsChanged();
    }
}

QString SettingsStore::theme() const {
    return m_theme;
}

QString SettingsStore::themeId() const {
    return m_themeId;
}

QString SettingsStore::tabLayout() const {
    return m_tabLayout;
}

QString SettingsStore::searchEngine() const {
    return m_searchEngine;
}

QString SettingsStore::searchUrl() const {
    return m_searchUrl;
}

bool SettingsStore::searchSuggestions() const {
    return m_searchSuggestions;
}

QVariantList SettingsStore::searchEngineActions() const {
    QVariantList actions;
    const bool custom = customSearchEngine();
    for (const SearchEnginePreset &preset : searchEnginePresets()) {
        QVariantMap action;
        action.insert("id", preset.name);
        action.insert("title", preset.name);
        action.insert("subtitle", preset.url);
        action.insert("icon", !custom && preset.url == m_searchUrl ? "check" : "");
        actions.append(action);
    }
    QVariantMap customAction;
    customAction.insert("id", "custom");
    customAction.insert("title", "Custom");
    customAction.insert("subtitle", "Provide your own name and search URL");
    customAction.insert("icon", custom ? "check" : "");
    actions.append(customAction);
    return actions;
}

bool SettingsStore::customSearchEngine() const {
    return m_customSearchEngine || !isPresetSearchUrl(m_searchUrl);
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

void SettingsStore::selectSearchEngine(const QString &id) {
    if (id == "custom") {
        if (!m_customSearchEngine) {
            m_customSearchEngine = true;
            emit customSearchEngineChanged();
            emit searchEngineActionsChanged();
        }
        return;
    }
    const SearchEnginePreset *preset = searchEnginePreset(id);
    if (!preset) {
        return;
    }
    const bool wasCustom = customSearchEngine();
    const bool engineWillChange = m_searchEngine != preset->name;
    const bool urlWillChange = m_searchUrl != preset->url;
    m_customSearchEngine = false;
    setSearchEngine(preset->name);
    setSearchUrl(preset->url);
    if (wasCustom && !urlWillChange) {
        emit customSearchEngineChanged();
    }
    if (!urlWillChange && !engineWillChange) {
        emit searchEngineActionsChanged();
    }
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

void SettingsStore::setTabLayout(const QString &tabLayout) {
    if (tabLayout != "horizontal" && tabLayout != "sidebar") {
        return;
    }
    setString("appearance/tabLayout", tabLayout, m_tabLayout, &SettingsStore::tabLayoutChanged);
}

void SettingsStore::setSearchEngine(const QString &searchEngine) {
    setString("search/name", searchEngine, m_searchEngine, &SettingsStore::searchEngineChanged);
}

void SettingsStore::setSearchUrl(const QString &searchUrl) {
    setString("search/url", searchUrl, m_searchUrl, &SettingsStore::searchUrlChanged);
}

void SettingsStore::setSearchSuggestions(bool enabled) {
    if (enabled == m_searchSuggestions) {
        return;
    }
    m_searchSuggestions = enabled;
    persist("search/suggestions", enabled);
    emit searchSuggestionsChanged();
}

void SettingsStore::persist(const QString &key, const QVariant &value) {
    if (!m_settings) {
        return;
    }
    m_settings->setValue(key, value);
    m_settings->sync();
}

void SettingsStore::setString(const QString &key, const QString &value, QString &current, void (SettingsStore::*signal)()) {
    if (value == current) {
        return;
    }
    current = value;
    persist(key, value);
    emit(this->*signal)();
}

}
