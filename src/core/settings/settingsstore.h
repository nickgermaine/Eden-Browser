#pragma once

#include <QObject>
#include <QSettings>

#include <memory>

namespace eden::core {

    class SettingsStore final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
        Q_PROPERTY(QString themeId READ themeId WRITE setThemeId NOTIFY themeIdChanged)
        Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)

      public:
        explicit SettingsStore(QObject *parent = nullptr);
        ~SettingsStore() override;

        static SettingsStore *instance();

        void initialize();

        QString theme() const;
        QString themeId() const;
        QString devToolsPlacement() const;
        int devToolsPaneWidth() const;
        int devToolsPaneHeight() const;
        int windowWidth() const;
        int windowHeight() const;
        bool windowMaximized() const;
        bool systemDark() const;

        Q_INVOKABLE QString licenseText(const QString &fileName) const;

        void setTheme(const QString &theme);
        void setThemeId(const QString &themeId);
        void setDevToolsPlacement(const QString &placement);
        void setDevToolsPaneSize(int width, int height);
        void setWindowGeometry(int width, int height, bool maximized);

      signals:
        void themeChanged();
        void themeIdChanged();
        void systemDarkChanged();
        void devToolsPlacementChanged();

      private:
        void persist(const QString &key, const QVariant &value);
        void setString(const QString &key, const QString &value, QString &current, void (SettingsStore::*signal)());

        std::unique_ptr<QSettings> m_settings;
        QString m_theme = "system";
        QString m_themeId = "eden-default";
        QString m_devToolsPlacement = "right";
        int m_devToolsPaneWidth = 440;
        int m_devToolsPaneHeight = 320;
        int m_windowWidth = 1360;
        int m_windowHeight = 860;
        bool m_windowMaximized = false;
    };

}
