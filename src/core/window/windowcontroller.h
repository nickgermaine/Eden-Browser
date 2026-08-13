#pragma once

#include "engine/enginefactory.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

class QJsonObject;
class QQuickItem;
class QQuickWindow;

namespace eden::engine {
class EngineProfile;
class EngineProfileMap;
class EngineView;
}

namespace eden::core {

class BookmarkStore;
class DownloadManager;
class HistoryStore;
class OmniboxController;
class ShortcutRegistry;
class TabModel;

class WindowController : public QObject {
    Q_OBJECT
    Q_PROPERTY(TabModel *tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex WRITE setActiveIndex NOTIFY activeIndexChanged)
    Q_PROPERTY(QObject *currentEngine READ currentEngine NOTIFY currentEngineChanged)
    Q_PROPERTY(QUrl currentUrl READ currentUrl NOTIFY currentEngineChanged)
    Q_PROPERTY(QString displayUrl READ displayUrl NOTIFY currentEngineChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY modeChanged)
    Q_PROPERTY(OmniboxController *omnibox READ omnibox NOTIFY tabsChanged)
    Q_PROPERTY(ShortcutRegistry *shortcuts READ shortcuts CONSTANT)
    Q_PROPERTY(HistoryStore *history READ history CONSTANT)
    Q_PROPERTY(BookmarkStore *bookmarks READ bookmarks CONSTANT)
    Q_PROPERTY(DownloadManager *downloads READ downloads CONSTANT)
    Q_PROPERTY(QString openPane READ openPane WRITE setOpenPane NOTIFY openPaneChanged)
    Q_PROPERTY(int paneWidth READ paneWidth NOTIFY paneWidthChanged)
    Q_PROPERTY(int devToolsPaneWidth READ devToolsPaneWidth NOTIFY devToolsPaneSizeChanged)
    Q_PROPERTY(int devToolsPaneHeight READ devToolsPaneHeight NOTIFY devToolsPaneSizeChanged)
    Q_PROPERTY(bool sidebarExpanded READ sidebarExpanded WRITE setSidebarExpanded NOTIFY sidebarExpandedChanged)
    Q_PROPERTY(bool findVisible READ findVisible WRITE setFindVisible NOTIFY findVisibleChanged)
    Q_PROPERTY(bool commandPaletteVisible READ commandPaletteVisible WRITE setCommandPaletteVisible NOTIFY commandPaletteVisibleChanged)
    Q_PROPERTY(bool contentFullscreen READ contentFullscreen NOTIFY contentFullscreenChanged)
    Q_PROPERTY(bool currentBookmarked READ currentBookmarked NOTIFY currentBookmarkedChanged)
    Q_PROPERTY(QVariantList pageContextMenuActions READ pageContextMenuActions NOTIFY pageContextMenuChanged)
    Q_PROPERTY(QPoint pageContextMenuPosition READ pageContextMenuPosition NOTIFY pageContextMenuChanged)
    Q_PROPERTY(QVariantMap javaScriptDialog READ javaScriptDialog NOTIFY javaScriptDialogChanged)
    Q_PROPERTY(QVariantMap permissionRequest READ permissionRequest NOTIFY permissionRequestChanged)
    Q_PROPERTY(QVariantMap fileDialog READ fileDialog NOTIFY fileDialogChanged)
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
    QString openPane() const;
    int paneWidth() const;
    bool sidebarExpanded() const;
    bool findVisible() const;
    bool commandPaletteVisible() const;
    bool contentFullscreen() const;
    QVariantList pageContextMenuActions() const;
    QPoint pageContextMenuPosition() const;
    QVariantMap javaScriptDialog() const;
    QVariantMap permissionRequest() const;
    QVariantMap fileDialog() const;
    int tabDragRevision() const;
    int tabDragIndex() const;
    bool tabDragTorn() const;
    int tabPreviewRevision() const;

    Q_INVOKABLE void initialize(bool privateWindow, const QString &engineName = {}, bool restorePreviousSession = true,
                                bool createInitialTab = true);
    Q_INVOKABLE void setActiveIndex(int index);
    Q_INVOKABLE int newTab(const QUrl &url = QUrl("about:blank"), bool background = false, const QString &backendId = {});
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
    Q_INVOKABLE void saveSession();
    Q_INVOKABLE void prepareToClose();
    Q_INVOKABLE void executePageContextMenuCommand(const QString &command);
    Q_INVOKABLE void dismissPageContextMenu();
    Q_INVOKABLE void resolveJavaScriptDialog(bool accepted, const QString &text = {});
    Q_INVOKABLE void resolvePermissionRequest(bool allowed);
    Q_INVOKABLE void resolveFileDialog(bool accepted, const QList<QUrl> &files = {});
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

  signals:
    void tabsChanged();
    void activeIndexChanged();
    void currentEngineChanged();
    void modeChanged();
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
    void fileDialogChanged();
    void fileDialogRequested();
    void focusOmniboxRequested();
    void closeWindowRequested();
    void tabDragRevisionChanged();
    void tabPreviewRevisionChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void connectProfile(engine::EngineProfile *profile);
    void connectEngine(engine::EngineView *view);
    void captureTabPreview(int index);
    void captureInternalPagePreview(int index);
    QVariantMap tabPreviewMetadata(int index, bool sampleMemory) const;
    WindowController *createBrowserWindow(bool privateWindow, bool createInitialTab = true, const QPoint &position = {},
                                          const QSize &size = {});
    QQuickItem *visibleDropArea() const;
    void updateTabDrag(const QPointF &windowPosition);
    void reorderDraggedTab();
    void beginTornDrag();
    void handleStripDrag(QQuickItem *area, qreal x, qreal y);
    void finalizeTornDrag(bool canceled);
    void endTabDrag(bool canceled);
    static void refreshTabDragVisuals();
    void restoreWindow(const QJsonObject &window);
    QJsonObject sessionWindow() const;
    void restoreSession();
    void scheduleSessionSave();
    void executeCommand(const QString &id);
    void setContentFullscreen(bool fullscreen);
    void toggleWindowFullscreen();
    void openInternalTab(const QUrl &url);
    QString sessionPath() const;

#if EDEN_ENGINE_CEF
    engine::EngineFactory::Backend m_defaultBackend = engine::EngineFactory::Backend::Cef;
#else
    engine::EngineFactory::Backend m_defaultBackend = engine::EngineFactory::Backend::QtWebEngine;
#endif
    std::unique_ptr<engine::EngineProfileMap> m_profiles;
    std::unique_ptr<TabModel> m_tabs;
    std::unique_ptr<class ThumbnailCache> m_thumbnailCache;
    std::unique_ptr<OmniboxController> m_omnibox;
    std::unique_ptr<HistoryStore> m_history;
    std::unique_ptr<BookmarkStore> m_bookmarks;
    std::unique_ptr<DownloadManager> m_downloads;
    std::unique_ptr<ShortcutRegistry> m_shortcuts;
    QTimer m_sessionTimer;
    int m_activeIndex = -1;
    bool m_privateWindow = false;
    bool m_initialized = false;
    bool m_registeredForSession = false;
#if EDEN_ENGINE_CEF
    QString m_engineName = "cef";
#else
    QString m_engineName = "qtwebengine";
#endif
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
    QPointer<engine::EngineView> m_pageContextMenuEngine;
    QUrl m_pageContextMenuTarget;
    QVariantMap m_javaScriptDialog;
    QPointer<engine::EngineView> m_javaScriptDialogEngine;
    QVariantMap m_permissionRequest;
    QPointer<engine::EngineView> m_permissionRequestEngine;
    QVariantMap m_fileDialog;
    QPointer<engine::EngineView> m_fileDialogEngine;
    QPointer<QQuickWindow> m_window;
    bool m_registeredAsWindow = false;
    bool m_closeDeferredForTabDrag = false;
    int m_tabDragRevision = 0;
    int m_tabPreviewRevision = 0;
    QTimer m_tabDragGuard;
};

}
