#pragma once

#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QColor>
#include <QJsonObject>
#include <QUrl>

#include <optional>

namespace eden::core {

    class ThemeManager final : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(QString activeThemeId READ activeThemeId NOTIFY activeThemeChanged)
        Q_PROPERTY(QString themeDirectory READ themeDirectory CONSTANT)
        Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
        Q_PROPERTY(QAbstractItemModel *editorTokens READ editorTokens CONSTANT)
        Q_PROPERTY(bool editing READ editing NOTIFY editorChanged)
        Q_PROPERTY(bool editorBuiltIn READ editorBuiltIn NOTIFY editorChanged)
        Q_PROPERTY(bool editorDirty READ editorDirty NOTIFY editorChanged)
        Q_PROPERTY(QString editorSourceId READ editorSourceId NOTIFY editorChanged)
        Q_PROPERTY(QString editorId READ editorId WRITE setEditorId NOTIFY editorChanged)
        Q_PROPERTY(QString editorName READ editorName WRITE setEditorName NOTIFY editorChanged)
        Q_PROPERTY(QString editorAuthor READ editorAuthor WRITE setEditorAuthor NOTIFY editorChanged)
        Q_PROPERTY(QColor primary READ primary NOTIFY themeChanged)
        Q_PROPERTY(QColor primaryText READ primaryText NOTIFY themeChanged)
        Q_PROPERTY(QColor primaryContainer READ primaryContainer NOTIFY themeChanged)
        Q_PROPERTY(QColor primaryContainerText READ primaryContainerText NOTIFY themeChanged)
        Q_PROPERTY(QColor surface READ surface NOTIFY themeChanged)
        Q_PROPERTY(QColor surfaceContainerLow READ surfaceContainerLow NOTIFY themeChanged)
        Q_PROPERTY(bool toolbarGradientEnabled READ toolbarGradientEnabled NOTIFY themeChanged)
        Q_PROPERTY(qreal toolbarGradientAngle READ toolbarGradientAngle NOTIFY themeChanged)
        Q_PROPERTY(QColor toolbarGradientStart READ toolbarGradientStart NOTIFY themeChanged)
        Q_PROPERTY(QColor toolbarGradientMiddle READ toolbarGradientMiddle NOTIFY themeChanged)
        Q_PROPERTY(QColor toolbarGradientEnd READ toolbarGradientEnd NOTIFY themeChanged)
        Q_PROPERTY(qreal toolbarGradientMiddlePosition READ toolbarGradientMiddlePosition NOTIFY themeChanged)
        Q_PROPERTY(QColor tabBarBackground READ tabBarBackground NOTIFY themeChanged)
        Q_PROPERTY(QColor surfaceContainer READ surfaceContainer NOTIFY themeChanged)
        Q_PROPERTY(QColor surfaceContainerHigh READ surfaceContainerHigh NOTIFY themeChanged)
        Q_PROPERTY(QColor surfaceContainerHighest READ surfaceContainerHighest NOTIFY themeChanged)
        Q_PROPERTY(QColor surfaceText READ surfaceText NOTIFY themeChanged)
        Q_PROPERTY(QColor surfaceVariantText READ surfaceVariantText NOTIFY themeChanged)
        Q_PROPERTY(QColor disabledText READ disabledText NOTIFY themeChanged)
        Q_PROPERTY(QColor outline READ outline NOTIFY themeChanged)
        Q_PROPERTY(QColor windowBorder READ windowBorder NOTIFY themeChanged)
        Q_PROPERTY(QColor contentBorder READ contentBorder NOTIFY themeChanged)
        Q_PROPERTY(QColor paneBorder READ paneBorder NOTIFY themeChanged)
        Q_PROPERTY(QColor menuBorder READ menuBorder NOTIFY themeChanged)
        Q_PROPERTY(QColor focusBorder READ focusBorder NOTIFY themeChanged)
        Q_PROPERTY(QColor error READ error NOTIFY themeChanged)
        Q_PROPERTY(QColor privatePrimary READ privatePrimary NOTIFY themeChanged)
        Q_PROPERTY(QColor privateBackground READ privateBackground NOTIFY themeChanged)
        Q_PROPERTY(QColor completionHint READ completionHint NOTIFY themeChanged)
        Q_PROPERTY(QColor iconColor READ iconColor NOTIFY themeChanged)
        Q_PROPERTY(QColor disabledIconColor READ disabledIconColor NOTIFY themeChanged)
        Q_PROPERTY(QColor windowShadowColor READ windowShadowColor NOTIFY themeChanged)
        Q_PROPERTY(QColor overlayShadowColor READ overlayShadowColor NOTIFY themeChanged)
        Q_PROPERTY(int windowRadius READ windowRadius NOTIFY themeChanged)
        Q_PROPERTY(int contentRadius READ contentRadius NOTIFY themeChanged)
        Q_PROPERTY(int cardRadius READ cardRadius NOTIFY themeChanged)
        Q_PROPERTY(int controlRadius READ controlRadius NOTIFY themeChanged)
        Q_PROPERTY(int menuRadius READ menuRadius NOTIFY themeChanged)
        Q_PROPERTY(int suggestionRadius READ suggestionRadius NOTIFY themeChanged)
        Q_PROPERTY(int workspaceInset READ workspaceInset NOTIFY themeChanged)
        Q_PROPERTY(int workspaceGap READ workspaceGap NOTIFY themeChanged)
        Q_PROPERTY(int tabMinimumWidth READ tabMinimumWidth NOTIFY themeChanged)
        Q_PROPERTY(int tabMaximumWidth READ tabMaximumWidth NOTIFY themeChanged)
        Q_PROPERTY(int pinnedTabWidth READ pinnedTabWidth NOTIFY themeChanged)
        Q_PROPERTY(int windowBorderWidth READ windowBorderWidth NOTIFY themeChanged)
        Q_PROPERTY(int contentBorderWidth READ contentBorderWidth NOTIFY themeChanged)
        Q_PROPERTY(int paneBorderWidth READ paneBorderWidth NOTIFY themeChanged)
        Q_PROPERTY(int menuBorderWidth READ menuBorderWidth NOTIFY themeChanged)
        Q_PROPERTY(int focusBorderWidth READ focusBorderWidth NOTIFY themeChanged)
        Q_PROPERTY(int windowShadowExtent READ windowShadowExtent NOTIFY themeChanged)
        Q_PROPERTY(qreal windowShadowBlur READ windowShadowBlur NOTIFY themeChanged)
        Q_PROPERTY(qreal windowShadowSpread READ windowShadowSpread NOTIFY themeChanged)
        Q_PROPERTY(qreal windowShadowVerticalOffset READ windowShadowVerticalOffset NOTIFY themeChanged)
        Q_PROPERTY(qreal windowShadowHorizontalOffset READ windowShadowHorizontalOffset NOTIFY themeChanged)
        Q_PROPERTY(QColor windowShadowAmbientColor READ windowShadowAmbientColor NOTIFY themeChanged)
        Q_PROPERTY(qreal windowShadowAmbientBlur READ windowShadowAmbientBlur NOTIFY themeChanged)
        Q_PROPERTY(qreal windowShadowAmbientVerticalOffset READ windowShadowAmbientVerticalOffset NOTIFY themeChanged)
        Q_PROPERTY(qreal overlayShadowBlur READ overlayShadowBlur NOTIFY themeChanged)
        Q_PROPERTY(qreal overlayShadowSpread READ overlayShadowSpread NOTIFY themeChanged)
        Q_PROPERTY(qreal overlayShadowVerticalOffset READ overlayShadowVerticalOffset NOTIFY themeChanged)
        Q_PROPERTY(QString iconStyle READ iconStyle NOTIFY themeChanged)
        Q_PROPERTY(QString activeIconStyle READ activeIconStyle NOTIFY themeChanged)
        Q_PROPERTY(int iconSize READ iconSize NOTIFY themeChanged)
        Q_PROPERTY(qreal disabledIconOpacity READ disabledIconOpacity NOTIFY themeChanged)
        Q_PROPERTY(int shortDuration READ shortDuration NOTIFY themeChanged)
        Q_PROPERTY(int mediumDuration READ mediumDuration NOTIFY themeChanged)
        Q_PROPERTY(int longDuration READ longDuration NOTIFY themeChanged)
        Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY themeChanged)
        Q_PROPERTY(int bodyFontSize READ bodyFontSize NOTIFY themeChanged)
        Q_PROPERTY(int labelFontSize READ labelFontSize NOTIFY themeChanged)
        Q_PROPERTY(int titleFontSize READ titleFontSize NOTIFY themeChanged)

      public:
        enum Role { IdRole = Qt::UserRole + 1, NameRole, AuthorRole, BuiltInRole, ActiveRole };

        explicit ThemeManager(QObject *parent = nullptr);
        ~ThemeManager() override;

        static ThemeManager *instance();

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString activeThemeId() const;
        QString themeDirectory() const;
        QString lastError() const;
        QAbstractItemModel *editorTokens() const;
        bool editing() const;
        bool editorBuiltIn() const;
        bool editorDirty() const;
        QString editorSourceId() const;
        QString editorId() const;
        QString editorName() const;
        QString editorAuthor() const;
        void setEditorId(const QString &id);
        void setEditorName(const QString &name);
        void setEditorAuthor(const QString &author);

        QColor primary() const;
        QColor primaryText() const;
        QColor primaryContainer() const;
        QColor primaryContainerText() const;
        QColor surface() const;
        QColor surfaceContainerLow() const;
        bool toolbarGradientEnabled() const;
        qreal toolbarGradientAngle() const;
        QColor toolbarGradientStart() const;
        QColor toolbarGradientMiddle() const;
        QColor toolbarGradientEnd() const;
        qreal toolbarGradientMiddlePosition() const;
        QColor tabBarBackground() const;
        QColor surfaceContainer() const;
        QColor surfaceContainerHigh() const;
        QColor surfaceContainerHighest() const;
        QColor surfaceText() const;
        QColor surfaceVariantText() const;
        QColor disabledText() const;
        QColor outline() const;
        QColor windowBorder() const;
        QColor contentBorder() const;
        QColor paneBorder() const;
        QColor menuBorder() const;
        QColor focusBorder() const;
        QColor error() const;
        QColor privatePrimary() const;
        QColor privateBackground() const;
        QColor completionHint() const;
        QColor iconColor() const;
        QColor disabledIconColor() const;
        QColor windowShadowColor() const;
        QColor overlayShadowColor() const;

        int windowRadius() const;
        int contentRadius() const;
        int cardRadius() const;
        int controlRadius() const;
        int menuRadius() const;
        int suggestionRadius() const;
        int workspaceInset() const;
        int workspaceGap() const;
        int tabMinimumWidth() const;
        int tabMaximumWidth() const;
        int pinnedTabWidth() const;
        int windowBorderWidth() const;
        int contentBorderWidth() const;
        int paneBorderWidth() const;
        int menuBorderWidth() const;
        int focusBorderWidth() const;
        int windowShadowExtent() const;
        qreal windowShadowBlur() const;
        qreal windowShadowSpread() const;
        qreal windowShadowVerticalOffset() const;
        qreal windowShadowHorizontalOffset() const;
        QColor windowShadowAmbientColor() const;
        qreal windowShadowAmbientBlur() const;
        qreal windowShadowAmbientVerticalOffset() const;
        qreal overlayShadowBlur() const;
        qreal overlayShadowSpread() const;
        qreal overlayShadowVerticalOffset() const;
        QString iconStyle() const;
        QString activeIconStyle() const;
        int iconSize() const;
        qreal disabledIconOpacity() const;
        int shortDuration() const;
        int mediumDuration() const;
        int longDuration() const;
        QString fontFamily() const;
        int bodyFontSize() const;
        int labelFontSize() const;
        int titleFontSize() const;

        Q_INVOKABLE QVariantMap themePreview(const QString &id, bool dark) const;
        Q_INVOKABLE void refresh();
        Q_INVOKABLE bool installTheme(const QUrl &source);
        Q_INVOKABLE bool activateTheme(const QString &id);
        Q_INVOKABLE bool beginEditing(const QString &id = {});
        Q_INVOKABLE void cancelEditing();
        Q_INVOKABLE bool revertEditing();
        Q_INVOKABLE QVariant editorValue(const QString &path) const;
        Q_INVOKABLE bool setEditorToken(const QString &path, const QVariant &value);
        Q_INVOKABLE bool setEditorColor(const QString &path, const QColor &color);
        Q_INVOKABLE bool setEditorGradientValue(const QString &path, const QString &key, const QVariant &value);
        Q_INVOKABLE bool saveEditing();
        Q_INVOKABLE bool saveEditingAs();
        Q_INVOKABLE bool exportEditing(const QUrl &destination);

      signals:
        void activeThemeChanged();
        void themeChanged();
        void lastErrorChanged();
        void editorChanged();

      private:
        struct ThemeDefinition {
            QString id;
            QString name;
            QString author;
            QString path;
            bool builtIn = false;
            QJsonObject root;
        };

        std::optional<ThemeDefinition>
        parseTheme(const QByteArray &data, const QString &path, bool builtIn, QString &error) const;
        QJsonObject migrateTheme(QJsonObject root) const;
        const ThemeDefinition &activeTheme() const;
        const ThemeDefinition &defaultTheme() const;
        QJsonValue themeValue(const QString &group, const QString &key) const;
        QJsonValue schemeValue(const QString &group, const QString &key) const;
        QJsonValue valueAtPath(const QJsonObject &root, const QString &path) const;
        QJsonObject
        rootWithValue(const QJsonObject &root, const QStringList &parts, const QJsonValue &value, int index = 0) const;
        const QJsonObject &effectiveRoot() const;
        QColor paletteColor(const QString &key) const;
        QColor colorValue(const QJsonValue &value) const;
        QColor gradientColor(const QString &key) const;
        QColor shadowColor() const;
        int integerToken(const QString &group, const QString &key, int minimum, int maximum) const;
        qreal realToken(const QString &group, const QString &key, qreal minimum, qreal maximum) const;
        QString stringToken(const QString &group, const QString &key) const;
        bool darkScheme() const;
        void setLastError(const QString &error);
        void selectConfiguredTheme();
        void setEditorIdentity(const QString &key, const QString &value);
        bool writeEditorTheme(const QString &path);
        bool finishEditorSave(const QString &id);

        class ThemeEditorModel *m_editorModel = nullptr;

        QList<ThemeDefinition> m_themes;
        int m_activeIndex = 0;
        QString m_lastError;
        bool m_editing = false;
        bool m_editorBuiltIn = false;
        bool m_editorDirty = false;
        QString m_editorSourceId;
        QJsonObject m_editorRoot;
    };

}
