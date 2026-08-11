#pragma once

#include "engine/enginefactory.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <memory>

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

    Q_INVOKABLE void initialize(bool privateWindow, const QString &engineName = "qtwebengine");
    Q_INVOKABLE void setActiveIndex(int index);
    Q_INVOKABLE int newTab(const QUrl &url = QUrl("about:blank"), bool background = false);
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void navigate(const QUrl &url);
    Q_INVOKABLE void navigateText(const QString &text, bool controlEnter = false);
    Q_INVOKABLE void activateSuggestion(int row);
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
    void focusOmniboxRequested();
    void closeWindowRequested();

  private:
    void connectEngine(engine::EngineView *view);
    void restoreSession();
    void scheduleSessionSave();
    void executeCommand(const QString &id);
    void openInternalTab(const QUrl &url);
    QString sessionPath() const;

    engine::EngineFactory::Backend m_backend = engine::EngineFactory::Backend::QtWebEngine;
    std::unique_ptr<engine::EngineProfile> m_privateProfile;
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
    QString m_openPane;
    int m_paneWidth = 320;
    int m_paneResizeStartWidth = 320;
    bool m_sidebarExpanded = true;
    bool m_findVisible = false;
    bool m_commandPaletteVisible = false;
};

}
