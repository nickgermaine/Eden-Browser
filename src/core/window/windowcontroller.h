#pragma once

#include "core/bookmarks/bookmarkstore.h"
#include "core/downloads/downloadmanager.h"
#include "core/history/historystore.h"
#include "core/profiles/profilesettings.h"
#include "engine/enginefactory.h"

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class QQuickItem;
class QQuickWindow;

namespace eden::engine {
    class EngineProfile;
    class EngineProfileMap;
    class EngineView;
}

namespace eden::passwords {
    class CredentialVault;
}

namespace eden::core {

    class OmniboxController;
    class ProfileContext;
    class ShortcutRegistry;
    class TabModel;

    class WindowController : public QObject {
        Q_OBJECT
        Q_PROPERTY(TabModel *tabs READ tabs NOTIFY tabsChanged)
        Q_PROPERTY(int activeIndex READ activeIndex WRITE setActiveIndex NOTIFY activeIndexChanged)
        Q_PROPERTY(QObject *currentEngine READ currentEngine NOTIFY currentEngineChanged)
        Q_PROPERTY(QUrl currentUrl READ currentUrl NOTIFY currentUrlChanged)
        Q_PROPERTY(QString displayUrl READ displayUrl NOTIFY currentUrlChanged)
        Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
        Q_PROPERTY(OmniboxController *omnibox READ omnibox NOTIFY tabsChanged)
        Q_PROPERTY(ShortcutRegistry *shortcuts READ shortcuts CONSTANT)
        Q_PROPERTY(HistoryStore *history READ history NOTIFY profileChanged)
        Q_PROPERTY(BookmarkStore *bookmarks READ bookmarks NOTIFY profileChanged)
        Q_PROPERTY(DownloadManager *downloads READ downloads NOTIFY profileChanged)
        Q_PROPERTY(ProfileSettings *profileSettings READ profileSettings NOTIFY profileChanged)
        Q_PROPERTY(QObject *credentialVault READ credentialVault NOTIFY profileChanged)
        Q_PROPERTY(QObject *permissionStore READ permissionStore NOTIFY profileChanged)
        Q_PROPERTY(QString profileId READ profileId NOTIFY profileChanged)
        Q_PROPERTY(QString profileDisplayName READ profileDisplayName NOTIFY profileIdentityChanged)
        Q_PROPERTY(QString profileAvatarUrl READ profileAvatarUrl NOTIFY profileIdentityChanged)
        Q_PROPERTY(QColor profileColor READ profileColor NOTIFY profileIdentityChanged)
        Q_PROPERTY(QString openPane READ openPane WRITE setOpenPane NOTIFY openPaneChanged)
        Q_PROPERTY(int paneWidth READ paneWidth NOTIFY paneWidthChanged)
        Q_PROPERTY(int devToolsPaneWidth READ devToolsPaneWidth NOTIFY devToolsPaneSizeChanged)
        Q_PROPERTY(int devToolsPaneHeight READ devToolsPaneHeight NOTIFY devToolsPaneSizeChanged)
        Q_PROPERTY(bool sidebarExpanded READ sidebarExpanded WRITE setSidebarExpanded NOTIFY sidebarExpandedChanged)
        Q_PROPERTY(bool findVisible READ findVisible WRITE setFindVisible NOTIFY findVisibleChanged)
        Q_PROPERTY(
            bool commandPaletteVisible READ commandPaletteVisible WRITE setCommandPaletteVisible NOTIFY
                commandPaletteVisibleChanged
        )
        Q_PROPERTY(bool contentFullscreen READ contentFullscreen NOTIFY contentFullscreenChanged)
        Q_PROPERTY(bool currentBookmarked READ currentBookmarked NOTIFY currentBookmarkedChanged)
        Q_PROPERTY(QVariantList pageContextMenuActions READ pageContextMenuActions NOTIFY pageContextMenuChanged)
        Q_PROPERTY(QPoint pageContextMenuPosition READ pageContextMenuPosition NOTIFY pageContextMenuChanged)
        Q_PROPERTY(QString pageContextMenuSurface READ pageContextMenuSurface NOTIFY pageContextMenuChanged)
        Q_PROPERTY(QVariantMap javaScriptDialog READ javaScriptDialog NOTIFY javaScriptDialogChanged)
        Q_PROPERTY(QVariantMap permissionRequest READ permissionRequest NOTIFY permissionRequestChanged)
        Q_PROPERTY(QVariantMap displayCaptureRequest READ displayCaptureRequest NOTIFY displayCaptureRequestChanged)
        Q_PROPERTY(QVariantMap fileDialog READ fileDialog NOTIFY fileDialogChanged)
        Q_PROPERTY(QVariantMap credentialPrompt READ credentialPrompt NOTIFY credentialStateChanged)
        Q_PROPERTY(
            QString credentialDraftUsername READ credentialDraftUsername WRITE setCredentialDraftUsername NOTIFY
                credentialStateChanged
        )
        Q_PROPERTY(
            QString credentialDraftPassword READ credentialDraftPassword WRITE setCredentialDraftPassword NOTIFY
                credentialStateChanged
        )
        Q_PROPERTY(QVariantList autofillSuggestions READ autofillSuggestions NOTIFY credentialStateChanged)
        Q_PROPERTY(QPoint autofillPopupPosition READ autofillPopupPosition NOTIFY credentialStateChanged)
        Q_PROPERTY(bool credentialKeyVisible READ credentialKeyVisible NOTIFY credentialStateChanged)
        Q_PROPERTY(int tabDragRevision READ tabDragRevision NOTIFY tabDragRevisionChanged)
        Q_PROPERTY(int tabDragIndex READ tabDragIndex NOTIFY tabDragRevisionChanged)
        Q_PROPERTY(bool tabDragTorn READ tabDragTorn NOTIFY tabDragRevisionChanged)
        Q_PROPERTY(int tabPreviewRevision READ tabPreviewRevision NOTIFY tabPreviewRevisionChanged)

      public:
        explicit WindowController(QObject *parent = nullptr);
        ~WindowController() override;

        TabModel *tabs() const;
        int activeIndex() const;
        QObject *currentEngine() const;
        QUrl currentUrl() const;
        QString displayUrl() const;
        QString mode() const;
        OmniboxController *omnibox() const;
        ShortcutRegistry *shortcuts() const;
        HistoryStore *history() const;
        BookmarkStore *bookmarks() const;
        DownloadManager *downloads() const;
        ProfileSettings *profileSettings() const;
        QObject *credentialVault() const;
        QObject *permissionStore() const;
        QString profileId() const;
        QString profileDisplayName() const;
        QString profileAvatarUrl() const;
        QColor profileColor() const;
        void notifyProfileIdentityChanged();
        QString openPane() const;
        int paneWidth() const;
        bool sidebarExpanded() const;
        bool findVisible() const;
        bool commandPaletteVisible() const;
        bool contentFullscreen() const;
        QVariantList pageContextMenuActions() const;
        QPoint pageContextMenuPosition() const;
        QString pageContextMenuSurface() const;
        QVariantMap javaScriptDialog() const;
        QVariantMap permissionRequest() const;
        QVariantMap displayCaptureRequest() const;
        QVariantMap fileDialog() const;
        QVariantMap credentialPrompt() const;
        QString credentialDraftUsername() const;
        QString credentialDraftPassword() const;
        QVariantList autofillSuggestions() const;
        QPoint autofillPopupPosition() const;
        bool credentialKeyVisible() const;
        int tabDragRevision() const;
        int tabDragIndex() const;
        bool tabDragTorn() const;
        int tabPreviewRevision() const;

        void initialize(
            const std::shared_ptr<ProfileContext> &context,
            bool privateWindow,
            const QString &engineName = {},
            bool createInitialTab = true
        );
        bool isInitialized() const;
        bool isPrivateWindow() const;
        std::shared_ptr<ProfileContext> profileContext() const;
        QJsonObject sessionWindow() const;
        void restoreWindow(const QJsonObject &window);
        void closeWindowNow();
        QQuickWindow *window() const;

        Q_INVOKABLE void setActiveIndex(int index);
        Q_INVOKABLE int newTab(const QUrl &url = {}, bool background = false, const QString &backendId = {});
        Q_INVOKABLE int newTabAndFocusOmnibox();
        Q_INVOKABLE bool switchEngine(const QString &backendId);
        Q_INVOKABLE QVariantList tabContextMenuActions(int index) const;
        Q_INVOKABLE void executeTabContextMenuCommand(int index, const QString &command);
        Q_INVOKABLE QVariantMap tabPreview(int index);
        Q_INVOKABLE void closeTab(int index);
        Q_INVOKABLE void navigate(const QUrl &url);
        Q_INVOKABLE void navigateText(const QString &text, bool controlEnter = false);
        Q_INVOKABLE void activateSuggestion(int row);
        Q_INVOKABLE void beginTabDrag(int index, QQuickItem *visual, qreal pressX, qreal pressY);
        Q_INVOKABLE qreal tabDragTranslation(int index, qreal itemPosition) const;
        Q_INVOKABLE void tabDragEntered(QQuickItem *area, qreal x, qreal y);
        Q_INVOKABLE void tabDragMoved(QQuickItem *area, qreal x, qreal y);
        Q_INVOKABLE void tabDragLeft();
        Q_INVOKABLE bool tabDragDropped(QQuickItem *area, qreal x, qreal y);
        Q_INVOKABLE void back();
        Q_INVOKABLE void forward();
        Q_INVOKABLE QVariantList navigationHistory(int direction, int maximumItems = 20) const;
        Q_INVOKABLE void navigateHistory(int offset);
        Q_INVOKABLE void reload();
        Q_INVOKABLE void stop();
        Q_INVOKABLE void find(const QString &text, bool backward = false, bool caseSensitive = false);
        Q_INVOKABLE void toggleBookmark();
        Q_INVOKABLE bool currentBookmarked() const;
        Q_INVOKABLE void openNewWindow(bool privateWindow = false);
        Q_INVOKABLE void updateInternalPageUrl(const QUrl &url);
        Q_INVOKABLE void openSettingsTab();
        Q_INVOKABLE void openAboutTab();
        Q_INVOKABLE void openThemeEditorTab();
        Q_INVOKABLE QString settingsPath(const QUrl &url) const;
        Q_INVOKABLE QString settingsQueryValue(const QUrl &url, const QString &name) const;
        Q_INVOKABLE QUrl siteSettingsUrl(const QUrl &origin) const;
        Q_INVOKABLE QUrl passwordSiteUrl(const QString &site) const;
        Q_INVOKABLE void saveSession();
        Q_INVOKABLE void prepareToClose();
        Q_INVOKABLE void executePageContextMenuCommand(const QString &command);
        Q_INVOKABLE void dismissPageContextMenu();
        Q_INVOKABLE void resolveJavaScriptDialog(bool accepted, const QString &text = {});
        Q_INVOKABLE void resolvePermissionRequest(bool allowed);
        Q_INVOKABLE void dismissPermissionRequest();
        Q_INVOKABLE void resolveDisplayCaptureRequest(const QString &source = {});
        Q_INVOKABLE void resolveFileDialog(bool accepted, const QList<QUrl> &files = {});
        Q_INVOKABLE void acceptCredentialPrompt();
        Q_INVOKABLE void dismissCredentialPrompt();
        Q_INVOKABLE void fillSavedCredential(qint64 id);
        Q_INVOKABLE void fillSavedForm(qint64 id);
        Q_INVOKABLE void fillAutofillSuggestion(const QString &id);
        Q_INVOKABLE void refreshAutofillSuggestions();
        Q_INVOKABLE void copyCredentialUsername(qint64 id);
        Q_INVOKABLE void copyCredentialPassword(qint64 id);
        Q_INVOKABLE void beginPaneResize();
        Q_INVOKABLE void resizePane(qreal horizontalDelta);
        int devToolsPaneWidth() const;
        int devToolsPaneHeight() const;
        Q_INVOKABLE void beginDevToolsPaneResize();
        Q_INVOKABLE void resizeDevToolsPane(qreal delta, bool horizontal);
        Q_INVOKABLE void commitDevToolsPaneSize();
        void setOpenPane(const QString &pane);
        void setSidebarExpanded(bool expanded);
        void setFindVisible(bool visible);
        void setCommandPaletteVisible(bool visible);
        void setCredentialDraftUsername(const QString &username);
        void setCredentialDraftPassword(const QString &password);

      signals:
        void tabsChanged();
        void activeIndexChanged();
        void currentEngineChanged();
        void currentUrlChanged();
        void modeChanged();
        void profileChanged();
        void profileIdentityChanged();
        void openPaneChanged();
        void paneWidthChanged();
        void devToolsPaneSizeChanged();
        void sidebarExpandedChanged();
        void findVisibleChanged();
        void commandPaletteVisibleChanged();
        void contentFullscreenChanged();
        void currentBookmarkedChanged();
        void pageContextMenuChanged();
        void pageContextMenuRequested();
        void javaScriptDialogChanged();
        void javaScriptDialogRequested();
        void permissionRequestChanged();
        void permissionRequestRequested();
        void displayCaptureRequestChanged();
        void displayCaptureRequestRequested();
        void fileDialogChanged();
        void fileDialogRequested();
        void credentialStateChanged();
        void credentialPromptRequested();
        void autofillRequested();
        void focusOmniboxRequested();
        void closeWindowRequested();
        void transientMessageRequested(const QString &message);
        void tabDragRevisionChanged();
        void tabPreviewRevisionChanged();

      protected:
        bool eventFilter(QObject *watched, QEvent *event) override;

      private:
        void connectPrivateProfile(engine::EngineProfile *profile);
        void connectEngine(engine::EngineView *view);
        void finishPermissionRequest(bool allowed, bool persistDecision);
        void fillDefaultCredential(engine::EngineView *view);
        void requestCredentialFill(engine::EngineView *view, qint64 credentialId);
        void captureTabPreview(int index);
        void captureInternalPagePreview(int index);
        QVariantMap tabPreviewMetadata(int index, bool sampleMemory) const;
        QQuickItem *visibleDropArea() const;
        void updateTabDrag(const QPointF &windowPosition);
        void reorderDraggedTab();
        void beginTornDrag();
        void handleStripDrag(QQuickItem *area, qreal x, qreal y);
        void finalizeTornDrag(bool canceled);
        void endTabDrag(bool canceled);
        static void refreshTabDragVisuals();
        void scheduleSessionSave();
        void executeCommand(const QString &id);
        void setContentFullscreen(bool fullscreen);
        void toggleWindowFullscreen();
        void openInternalTab(const QUrl &url);

        std::shared_ptr<ProfileContext> m_context;
        engine::EngineFactory::Backend m_defaultBackend = engine::EngineFactory::Backend::QtWebEngine;
        std::unique_ptr<engine::EngineProfileMap> m_privateProfiles;
        std::unique_ptr<DownloadManager> m_privateDownloads;
        std::unique_ptr<TabModel> m_tabs;
        std::unique_ptr<class ThumbnailCache> m_thumbnailCache;
        std::unique_ptr<OmniboxController> m_omnibox;
        std::unique_ptr<ShortcutRegistry> m_shortcuts;
        int m_activeIndex = -1;
        bool m_privateWindow = false;
        bool m_initialized = false;
        bool m_registeredAsWindow = false;
        QString m_engineName;
        QString m_openPane;
        int m_paneWidth = 320;
        int m_paneResizeStartWidth = 320;
        int m_devToolsPaneWidth = 440;
        int m_devToolsPaneHeight = 320;
        int m_devToolsResizeStartWidth = 440;
        int m_devToolsResizeStartHeight = 320;
        bool m_sidebarExpanded = true;
        bool m_findVisible = false;
        bool m_commandPaletteVisible = false;
        bool m_contentFullscreen = false;
        int m_visibilityBeforeContentFullscreen = 0;
        int m_visibilityBeforeWindowFullscreen = 0;
        QVariantList m_pageContextMenuActions;
        QPoint m_pageContextMenuPosition;
        QString m_pageContextMenuSurface;
        QPointer<engine::EngineView> m_pageContextMenuEngine;
        QUrl m_pageContextMenuTarget;
        QVariantMap m_javaScriptDialog;
        QPointer<engine::EngineView> m_javaScriptDialogEngine;
        QVariantMap m_permissionRequest;
        QPointer<engine::EngineView> m_permissionRequestEngine;
        QVariantMap m_displayCaptureRequest;
        QPointer<engine::EngineView> m_displayCaptureRequestEngine;
        QVariantMap m_fileDialog;
        QPointer<engine::EngineView> m_fileDialogEngine;
        QVariantMap m_credentialPrompt;
        struct PendingCredentialUsername {
            QUrl origin;
            QString username;
        };
        QHash<engine::EngineView *, PendingCredentialUsername> m_pendingCredentialUsernames;
        QVariantList m_autofillSuggestions;
        QVariantMap m_autofillField;
        QString m_pendingCredentialPassword;
        QPointer<QQuickWindow> m_window;
        bool m_closeDeferredForTabDrag = false;
        int m_tabDragRevision = 0;
        int m_tabPreviewRevision = 0;
        QTimer m_tabDragGuard;
        QElapsedTimer m_previewCaptureClock;
        QHash<quint64, qint64> m_previewCaptureTimes;
        QTimer m_previewCaptureTimer;
        quint64 m_pendingPreviewTabId = 0;
        QSize m_windowedSize;
        QTimer m_geometryPersistTimer;
    };

}
