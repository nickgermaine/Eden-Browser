#include "core/profiles/profilesettings.h"

#include "core/profiles/profilepaths.h"
#include "engine/engineregistry.h"
#include "passwords/credentialvault.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSettings>
#include <QStringDecoder>

#include <algorithm>
#include <array>

namespace eden::core {

    static const QByteArray settingsHeader = QByteArrayLiteral("EDEN-SETTINGS-1\n");

    struct ProfileSearchPreset {
        QString name;
        QString url;
    };

    static const std::array<ProfileSearchPreset, 6> &profileSearchPresets() {
        static const std::array<ProfileSearchPreset, 6> presets = {
            ProfileSearchPreset{"DuckDuckGo", "https://duckduckgo.com/?q=%1"},
            ProfileSearchPreset{"Google", "https://www.google.com/search?q=%1"},
            ProfileSearchPreset{"Bing", "https://www.bing.com/search?q=%1"},
            ProfileSearchPreset{"Brave Search", "https://search.brave.com/search?q=%1"},
            ProfileSearchPreset{"Startpage", "https://www.startpage.com/sp/search?query=%1"},
            ProfileSearchPreset{"Ecosia", "https://www.ecosia.org/search?q=%1"},
        };
        return presets;
    }

    static const ProfileSearchPreset *profileSearchPreset(const QString &id) {
        const auto &presets = profileSearchPresets();
        const auto found = std::find_if(presets.cbegin(), presets.cend(), [&id](const ProfileSearchPreset &preset) {
            return preset.name == id;
        });
        return found == presets.cend() ? nullptr : &*found;
    }

    static bool isPresetSearchUrl(const QString &url) {
        const auto &presets = profileSearchPresets();
        return std::any_of(presets.cbegin(), presets.cend(), [&url](const ProfileSearchPreset &preset) {
            return preset.url == url;
        });
    }

    static QUrl pageDestination(const QString &value) {
        const QString trimmed = value.trimmed();
        if (trimmed.isEmpty()) {
            return QUrl(QStringLiteral("eden://newtab"));
        }
        const QUrl parsed(trimmed);
        if (parsed.isValid() && !parsed.scheme().isEmpty()) {
            return parsed;
        }
        return QUrl::fromUserInput(trimmed);
    }

    ProfileSettings::ProfileSettings(QString settingsPath, QObject *parent)
        : QObject(parent),
          m_settingsPath(std::move(settingsPath)) {
        connect(this, &ProfileSettings::searchEngineChanged, this, &ProfileSettings::searchEngineActionsChanged);
        connect(this, &ProfileSettings::searchUrlChanged, this, &ProfileSettings::searchEngineActionsChanged);
        connect(this, &ProfileSettings::searchUrlChanged, this, &ProfileSettings::customSearchEngineChanged);
        connect(this, &ProfileSettings::defaultEngineChanged, this, &ProfileSettings::engineActionsChanged);
    }

    ProfileSettings::~ProfileSettings() = default;

    void ProfileSettings::setVault(passwords::CredentialVault *vault) {
        m_vault = vault;
    }

    bool ProfileSettings::initialize() {
        if (m_initialized) {
            return true;
        }
        if (!m_vault || !m_vault->available()) {
            return setError(ProfileError::SettingsReadFailed);
        }
        QVariantMap values;
        bool needsWrite = false;
        QFile file(m_settingsPath);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray stored = file.readAll();
            if (file.error() != QFileDevice::NoError) {
                return setError(ProfileError::SettingsReadFailed);
            }
            file.close();
            if (stored.startsWith(settingsHeader)) {
                const QByteArray plain =
                    m_vault->openData(stored.sliced(settingsHeader.size()), QByteArrayLiteral("settings"));
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(plain, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    return setError(ProfileError::SettingsInvalid);
                }
                values = document.object().toVariantMap();
            } else {
                QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
                const QString text = decoder.decode(stored);
                if (stored.trimmed().isEmpty() || stored.startsWith("EDEN-") || decoder.hasError() ||
                    text.contains(QChar::Null)) {
                    return setError(ProfileError::SettingsInvalid);
                }
                QSettings legacy(m_settingsPath, QSettings::IniFormat);
                const QStringList keys = legacy.allKeys();
                if (legacy.status() != QSettings::NoError || keys.isEmpty()) {
                    return setError(ProfileError::SettingsInvalid);
                }
                for (const QString &key : keys) {
                    values.insert(key, legacy.value(key));
                }
                if (legacy.status() != QSettings::NoError) {
                    return setError(ProfileError::SettingsInvalid);
                }
                needsWrite = true;
            }
        } else if (QFileInfo::exists(m_settingsPath)) {
            return setError(ProfileError::SettingsReadFailed);
        } else {
            needsWrite = true;
        }
        m_values = std::move(values);
        if (needsWrite && !writeValues()) {
            return false;
        }

        const QString storedLayout = m_values.value("appearance/tabLayout", m_tabLayout).toString();
        const QString nextLayout =
            storedLayout == "horizontal" || storedLayout == "sidebar" ? storedLayout : QString("horizontal");
        const QString nextSearchEngine = m_values.value("search/name", m_searchEngine).toString();
        const QString nextSearchUrl = m_values.value("search/url", m_searchUrl).toString();
        const bool nextSearchSuggestions = m_values.value("search/suggestions", m_searchSuggestions).toBool();
        const QString nextHomePageUrl = m_values.value("startup/homePageUrl", m_homePageUrl).toString();
        const QString storedNewTabBehavior = m_values.value("startup/newTabBehavior", m_newTabBehavior).toString();
        const QString nextNewTabBehavior = storedNewTabBehavior == "new-tab-page" ||
                                                   storedNewTabBehavior == "home-page" ||
                                                   storedNewTabBehavior == "custom-url"
                                               ? storedNewTabBehavior
                                               : QStringLiteral("new-tab-page");
        const QString nextNewTabUrl = m_values.value("startup/newTabUrl", m_newTabUrl).toString();
        const QString storedEngine = m_values.value("engine/default", m_defaultEngine).toString();
        const engine::EngineRegistry *registry = engine::EngineRegistry::instance();
        const QString nextEngine = registry->contains(storedEngine) || registry->descriptors().isEmpty()
                                       ? storedEngine
                                       : registry->descriptors().constFirst().id;

        const auto updateString = [this](QString &current, const QString &next, void (ProfileSettings::*signal)()) {
            if (current != next) {
                current = next;
                emit(this->*signal)();
            }
        };
        updateString(m_tabLayout, nextLayout, &ProfileSettings::tabLayoutChanged);
        updateString(m_searchEngine, nextSearchEngine, &ProfileSettings::searchEngineChanged);
        updateString(m_searchUrl, nextSearchUrl, &ProfileSettings::searchUrlChanged);
        updateString(m_homePageUrl, nextHomePageUrl, &ProfileSettings::homePageUrlChanged);
        updateString(m_newTabBehavior, nextNewTabBehavior, &ProfileSettings::newTabBehaviorChanged);
        updateString(m_newTabUrl, nextNewTabUrl, &ProfileSettings::newTabUrlChanged);
        updateString(m_defaultEngine, nextEngine, &ProfileSettings::defaultEngineChanged);
        if (m_searchSuggestions != nextSearchSuggestions) {
            m_searchSuggestions = nextSearchSuggestions;
            emit searchSuggestionsChanged();
        }
        m_initialized = true;
        return setError(ProfileError::None);
    }

    ProfileError ProfileSettings::error() const {
        return m_error;
    }

    QString ProfileSettings::errorCode() const {
        return m_error == ProfileError::None ? QString() : profileErrorCode(m_error);
    }

    QString ProfileSettings::errorMessage() const {
        return profileErrorMessage(m_error);
    }

    bool ProfileSettings::setError(ProfileError error) {
        if (m_error != error) {
            m_error = error;
            emit errorChanged();
        }
        return error == ProfileError::None;
    }

    QString ProfileSettings::tabLayout() const {
        return m_tabLayout;
    }

    QString ProfileSettings::searchEngine() const {
        return m_searchEngine;
    }

    QString ProfileSettings::searchUrl() const {
        return m_searchUrl;
    }

    bool ProfileSettings::searchSuggestions() const {
        return m_searchSuggestions;
    }

    QString ProfileSettings::defaultEngine() const {
        return m_defaultEngine;
    }

    QString ProfileSettings::homePageUrl() const {
        return m_homePageUrl;
    }

    QString ProfileSettings::newTabBehavior() const {
        return m_newTabBehavior;
    }

    QString ProfileSettings::newTabUrl() const {
        return m_newTabUrl;
    }

    QUrl ProfileSettings::homePageDestination() const {
        return pageDestination(m_homePageUrl);
    }

    QUrl ProfileSettings::newTabDestination() const {
        if (m_newTabBehavior == "home-page") {
            return homePageDestination();
        }
        if (m_newTabBehavior == "custom-url") {
            return pageDestination(m_newTabUrl);
        }
        return QUrl(QStringLiteral("eden://newtab"));
    }

    QString ProfileSettings::defaultEngineName() const {
        const std::optional<engine::Backend> backend =
            engine::EngineRegistry::instance()->backendForId(m_defaultEngine);
        return backend ? engine::EngineRegistry::instance()->displayName(*backend) : QString("Unavailable");
    }

    QVariantList ProfileSettings::engineActions() const {
        return engine::EngineRegistry::instance()->selectionActions(m_defaultEngine);
    }

    QVariantList ProfileSettings::searchEngineActions() const {
        QVariantList actions;
        const bool custom = customSearchEngine();
        for (const ProfileSearchPreset &preset : profileSearchPresets()) {
            QVariantMap action;
            action.insert("id", preset.name);
            action.insert("title", preset.name);
            action.insert("subtitle", preset.url);
            action.insert("icon", !custom && preset.url == m_searchUrl ? "check" : "search");
            actions.append(action);
        }
        QVariantMap customAction;
        customAction.insert("id", "custom");
        customAction.insert("title", "Custom");
        customAction.insert("subtitle", "Provide your own name and search URL");
        customAction.insert("icon", custom ? "check" : "tuning");
        actions.append(customAction);
        return actions;
    }

    bool ProfileSettings::customSearchEngine() const {
        return m_customSearchEngine || !isPresetSearchUrl(m_searchUrl);
    }

    void ProfileSettings::selectSearchEngine(const QString &id) {
        if (id == "custom") {
            if (!m_customSearchEngine) {
                m_customSearchEngine = true;
                emit customSearchEngineChanged();
                emit searchEngineActionsChanged();
            }
            return;
        }
        const ProfileSearchPreset *preset = profileSearchPreset(id);
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

    void ProfileSettings::selectDefaultEngine(const QString &id) {
        setDefaultEngine(id);
    }

    void ProfileSettings::setTabLayout(const QString &tabLayout) {
        if (tabLayout != "horizontal" && tabLayout != "sidebar") {
            return;
        }
        setString("appearance/tabLayout", tabLayout, m_tabLayout, &ProfileSettings::tabLayoutChanged);
    }

    void ProfileSettings::setSearchEngine(const QString &searchEngine) {
        setString("search/name", searchEngine, m_searchEngine, &ProfileSettings::searchEngineChanged);
    }

    void ProfileSettings::setSearchUrl(const QString &searchUrl) {
        setString("search/url", searchUrl, m_searchUrl, &ProfileSettings::searchUrlChanged);
    }

    void ProfileSettings::setSearchSuggestions(bool enabled) {
        if (enabled == m_searchSuggestions) {
            return;
        }
        m_searchSuggestions = enabled;
        persist("search/suggestions", enabled);
        emit searchSuggestionsChanged();
    }

    void ProfileSettings::setDefaultEngine(const QString &backendId) {
        if (!engine::EngineRegistry::instance()->contains(backendId)) {
            return;
        }
        setString("engine/default", backendId, m_defaultEngine, &ProfileSettings::defaultEngineChanged);
    }

    void ProfileSettings::setHomePageUrl(const QString &url) {
        setString("startup/homePageUrl", url, m_homePageUrl, &ProfileSettings::homePageUrlChanged);
    }

    void ProfileSettings::setNewTabBehavior(const QString &behavior) {
        if (behavior != "new-tab-page" && behavior != "home-page" && behavior != "custom-url") {
            return;
        }
        setString("startup/newTabBehavior", behavior, m_newTabBehavior, &ProfileSettings::newTabBehaviorChanged);
    }

    void ProfileSettings::setNewTabUrl(const QString &url) {
        setString("startup/newTabUrl", url, m_newTabUrl, &ProfileSettings::newTabUrlChanged);
    }

    void ProfileSettings::persist(const QString &key, const QVariant &value) {
        if (!m_initialized) {
            return;
        }
        m_values.insert(key, value);
        writeValues();
    }

    bool ProfileSettings::writeValues() {
        if (!m_vault || !m_vault->available()) {
            return setError(ProfileError::SettingsWriteFailed);
        }
        const QByteArray plain = QJsonDocument(QJsonObject::fromVariantMap(m_values)).toJson(QJsonDocument::Compact);
        const QByteArray sealed = m_vault->sealData(plain, QByteArrayLiteral("settings"));
        if (sealed.isEmpty()) {
            return setError(ProfileError::SettingsWriteFailed);
        }
        QSaveFile file(m_settingsPath);
        if (!file.open(QIODevice::WriteOnly)) {
            return setError(ProfileError::SettingsWriteFailed);
        }
        if (file.write(settingsHeader) != settingsHeader.size() || file.write(sealed) != sealed.size() ||
            !file.commit() || !ProfilePaths::restrictFile(m_settingsPath)) {
            return setError(ProfileError::SettingsWriteFailed);
        }
        return setError(ProfileError::None);
    }

    void ProfileSettings::setString(
        const QString &key,
        const QString &value,
        QString &current,
        void (ProfileSettings::*signal)()
    ) {
        if (value == current) {
            return;
        }
        current = value;
        persist(key, value);
        emit(this->*signal)();
    }

}
