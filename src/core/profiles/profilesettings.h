#pragma once

#include "core/profiles/profileerror.h"

#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace eden::passwords {
    class CredentialVault;
}

namespace eden::core {

    class ProfileSettings final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QString tabLayout READ tabLayout WRITE setTabLayout NOTIFY tabLayoutChanged)
        Q_PROPERTY(QString searchEngine READ searchEngine WRITE setSearchEngine NOTIFY searchEngineChanged)
        Q_PROPERTY(QString searchUrl READ searchUrl WRITE setSearchUrl NOTIFY searchUrlChanged)
        Q_PROPERTY(
            bool searchSuggestions READ searchSuggestions WRITE setSearchSuggestions NOTIFY searchSuggestionsChanged
        )
        Q_PROPERTY(QString defaultEngine READ defaultEngine WRITE setDefaultEngine NOTIFY defaultEngineChanged)
        Q_PROPERTY(QString homePageUrl READ homePageUrl WRITE setHomePageUrl NOTIFY homePageUrlChanged)
        Q_PROPERTY(QString newTabBehavior READ newTabBehavior WRITE setNewTabBehavior NOTIFY newTabBehaviorChanged)
        Q_PROPERTY(QString newTabUrl READ newTabUrl WRITE setNewTabUrl NOTIFY newTabUrlChanged)
        Q_PROPERTY(QString defaultEngineName READ defaultEngineName NOTIFY defaultEngineChanged)
        Q_PROPERTY(QVariantList engineActions READ engineActions NOTIFY engineActionsChanged)
        Q_PROPERTY(QVariantList searchEngineActions READ searchEngineActions NOTIFY searchEngineActionsChanged)
        Q_PROPERTY(bool customSearchEngine READ customSearchEngine NOTIFY customSearchEngineChanged)
        Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorChanged)
        Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)

      public:
        explicit ProfileSettings(QString settingsPath, QObject *parent = nullptr);
        ~ProfileSettings() override;

        void setVault(passwords::CredentialVault *vault);
        bool initialize();
        ProfileError error() const;
        QString errorCode() const;
        QString errorMessage() const;

        QString tabLayout() const;
        QString searchEngine() const;
        QString searchUrl() const;
        bool searchSuggestions() const;
        QString defaultEngine() const;
        QString homePageUrl() const;
        QString newTabBehavior() const;
        QString newTabUrl() const;
        QUrl homePageDestination() const;
        QUrl newTabDestination() const;
        QString defaultEngineName() const;
        QVariantList engineActions() const;
        QVariantList searchEngineActions() const;
        bool customSearchEngine() const;

        Q_INVOKABLE void selectSearchEngine(const QString &id);
        Q_INVOKABLE void selectDefaultEngine(const QString &id);

        void setTabLayout(const QString &tabLayout);
        void setSearchEngine(const QString &searchEngine);
        void setSearchUrl(const QString &searchUrl);
        void setSearchSuggestions(bool enabled);
        void setDefaultEngine(const QString &backendId);
        void setHomePageUrl(const QString &url);
        void setNewTabBehavior(const QString &behavior);
        void setNewTabUrl(const QString &url);

      signals:
        void tabLayoutChanged();
        void searchEngineChanged();
        void searchUrlChanged();
        void searchSuggestionsChanged();
        void defaultEngineChanged();
        void homePageUrlChanged();
        void newTabBehaviorChanged();
        void newTabUrlChanged();
        void engineActionsChanged();
        void searchEngineActionsChanged();
        void customSearchEngineChanged();
        void errorChanged();

      private:
        void persist(const QString &key, const QVariant &value);
        bool writeValues();
        bool setError(ProfileError error);
        void setString(const QString &key, const QString &value, QString &current, void (ProfileSettings::*signal)());

        QString m_settingsPath;
        QPointer<passwords::CredentialVault> m_vault;
        QVariantMap m_values;
        bool m_initialized = false;
        ProfileError m_error = ProfileError::None;
        QString m_tabLayout = "horizontal";
        QString m_searchEngine = "DuckDuckGo";
        QString m_searchUrl = "https://duckduckgo.com/?q=%1";
        bool m_searchSuggestions = true;
        QString m_homePageUrl = "eden://newtab";
        QString m_newTabBehavior = "new-tab-page";
        QString m_newTabUrl = "eden://newtab";
#if EDEN_ENGINE_CEF
        QString m_defaultEngine = "cef";
#else
        QString m_defaultEngine = "qtwebengine";
#endif
        bool m_customSearchEngine = false;
    };

}
