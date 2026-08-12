#pragma once

#include "engine/enginefactory.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

class QJsonObject;
class QQuickItem;
class QQuickWindow;

namespace eden::engine {
class EngineProfile;
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
    Q_PROPERTY(bool sidebarExpanded READ sidebarExpanded WRITE setSidebarExpanded NOTIFY sidebarExpandedChanged)
    Q_PROPERTY(bool findVisible READ findVisible WRITE setFindVisible NOTIFY findVisibleChanged)
    Q_PROPERTY(bool commandPaletteVisible READ commandPaletteVisible WRITE setCommandPaletteVisible NOTIFY commandPaletteVisibleChanged)
    Q_PROPERTY(bool currentBookmarked READ currentBookmarked NOTIFY currentBookmarkedChanged)
    Q_PROPERTY(QVariantList pageContextMenuActions READ pageContextMenuActions NOTIFY pageContextMenuChanged)
    Q_PROPERTY(QPoint pageContextMenuPosition READ pageContextMenuPosition NOTIFY pageContextMenuChanged)
    Q_PROPERTY(int tabDragRevision READ tabDragRevision NOTIFY tabDragRevisionChanged)
    Q_PROPERTY(int tabDragIndex READ tabDragIndex NOTIFY tabDragRevisionChanged)
    Q_PROPERTY(bool tabDragTorn READ tabDragTorn NOTIFY tabDragRevisionChanged)

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
    QVariantList pageContextMenuActions() const;
    QPoint pageContextMenuPosition() const;
    int tabDragRevision() const;
    int tabDragIndex() const;
    bool tabDragTorn() const;

    Q_INVOKABLE void initialize(bool privateWindow, const QString &engineName = "qtwebengine", bool restorePreviousSession = true,
                                bool createInitialTab = true);
    Q_INVOKABLE void setActiveIndex(int index);
    Q_INVOKABLE int newTab(const QUrl &url = QUrl("about:blank"), bool background = false);
    Q_INVOKABLE int newTabAndFocusOmnibox();
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
    Q_INVOKABLE void openSettingsTab();
    Q_INVOKABLE void openThemeEditorTab();
    Q_INVOKABLE void saveSession();
    Q_INVOKABLE void prepareToClose();
    Q_INVOKABLE void executePageContextMenuCommand(const QString &command);
    Q_INVOKABLE void dismissPageContextMenu();
    Q_INVOKABLE void beginPaneResize();
    Q_INVOKABLE void resizePane(qreal horizontalDelta);
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
    void sidebarExpandedChanged();
    void findVisibleChanged();
    void commandPaletteVisibleChanged();
    void currentBookmarkedChanged();
    void pageContextMenuChanged();
    void pageContextMenuRequested();
    void focusOmniboxRequested();
    void closeWindowRequested();
    void tabDragRevisionChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void connectProfile(engine::EngineProfile *profile);
    void connectEngine(engine::EngineView *view);
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
    void openInternalTab(const QUrl &url);
    QString sessionPath() const;

    engine::EngineFactory::Backend m_backend = engine::EngineFactory::Backend::QtWebEngine;
    std::shared_ptr<engine::EngineProfile> m_privateProfile;
    engine::EngineProfile *m_profile = nullptr;
    std::unique_ptr<TabModel> m_tabs;
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
    QString m_engineName = "qtwebengine";
    QString m_openPane;
    int m_paneWidth = 320;
    int m_paneResizeStartWidth = 320;
    bool m_sidebarExpanded = true;
    bool m_findVisible = false;
    bool m_commandPaletteVisible = false;
    QVariantList m_pageContextMenuActions;
    QPoint m_pageContextMenuPosition;
    QPointer<engine::EngineView> m_pageContextMenuEngine;
    QPointer<QQuickWindow> m_window;
    bool m_registeredAsWindow = false;
    bool m_closeDeferredForTabDrag = false;
    int m_tabDragRevision = 0;
    QTimer m_tabDragGuard;
};

}
