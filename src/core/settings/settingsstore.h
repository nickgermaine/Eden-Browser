#pragma once

#include <QObject>
#include <QSettings>
#include <QVariantList>

#include <memory>

namespace eden::core {

class SettingsStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    Q_PROPERTY(QString themeId READ themeId WRITE setThemeId NOTIFY themeIdChanged)
    Q_PROPERTY(QString tabLayout READ tabLayout WRITE setTabLayout NOTIFY tabLayoutChanged)
    Q_PROPERTY(QString searchEngine READ searchEngine WRITE setSearchEngine NOTIFY searchEngineChanged)
    Q_PROPERTY(QString searchUrl READ searchUrl WRITE setSearchUrl NOTIFY searchUrlChanged)
    Q_PROPERTY(bool searchSuggestions READ searchSuggestions WRITE setSearchSuggestions NOTIFY searchSuggestionsChanged)
    Q_PROPERTY(QString defaultEngine READ defaultEngine WRITE setDefaultEngine NOTIFY defaultEngineChanged)
    Q_PROPERTY(QString defaultEngineName READ defaultEngineName NOTIFY defaultEngineChanged)
    Q_PROPERTY(QVariantList engineActions READ engineActions NOTIFY engineActionsChanged)
    Q_PROPERTY(QVariantList searchEngineActions READ searchEngineActions NOTIFY searchEngineActionsChanged)
    Q_PROPERTY(bool customSearchEngine READ customSearchEngine NOTIFY customSearchEngineChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)

  public:
    explicit SettingsStore(QObject *parent = nullptr);
    ~SettingsStore() override;

    static SettingsStore *instance();

    void initialize();

    QString theme() const;
    QString themeId() const;
    QString tabLayout() const;
    QString searchEngine() const;
    QString searchUrl() const;
    bool searchSuggestions() const;
    QString defaultEngine() const;
    QString defaultEngineName() const;
    QString devToolsPlacement() const;
    int devToolsPaneWidth() const;
    int devToolsPaneHeight() const;
    QVariantList searchEngineActions() const;
    QVariantList engineActions() const;
    bool customSearchEngine() const;
    bool systemDark() const;

    Q_INVOKABLE QString licenseText(const QString &fileName) const;
    Q_INVOKABLE void selectSearchEngine(const QString &id);
    Q_INVOKABLE void selectDefaultEngine(const QString &id);

    void setTheme(const QString &theme);
    void setThemeId(const QString &themeId);
    void setTabLayout(const QString &tabLayout);
    void setSearchEngine(const QString &searchEngine);
    void setSearchUrl(const QString &searchUrl);
    void setSearchSuggestions(bool enabled);
    void setDefaultEngine(const QString &backendId);
    void setDevToolsPlacement(const QString &placement);
    void setDevToolsPaneSize(int width, int height);

  signals:
    void themeChanged();
    void themeIdChanged();
    void tabLayoutChanged();
    void searchEngineChanged();
    void searchUrlChanged();
    void searchSuggestionsChanged();
    void defaultEngineChanged();
    void engineActionsChanged();
    void searchEngineActionsChanged();
    void customSearchEngineChanged();
    void systemDarkChanged();
    void devToolsPlacementChanged();

  private:
    void persist(const QString &key, const QVariant &value);
    void setString(const QString &key, const QString &value, QString &current, void (SettingsStore::*signal)());

    std::unique_ptr<QSettings> m_settings;
    QString m_theme = "system";
    QString m_themeId = "eden-default";
    QString m_tabLayout = "horizontal";
    QString m_searchEngine = "DuckDuckGo";
    QString m_searchUrl = "https://duckduckgo.com/?q=%1";
    bool m_searchSuggestions = true;
#if EDEN_ENGINE_CEF
    QString m_defaultEngine = "cef";
#else
    QString m_defaultEngine = "qtwebengine";
#endif
    bool m_customSearchEngine = false;
    QString m_devToolsPlacement = "right";
    int m_devToolsPaneWidth = 440;
    int m_devToolsPaneHeight = 320;
};

}
