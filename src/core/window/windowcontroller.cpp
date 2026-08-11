#include "core/window/windowcontroller.h"
#include "core/bookmarks/bookmarkstore.h"
#include "core/downloads/downloadmanager.h"
#include "core/history/historystore.h"
#include "core/omni/omniboxcontroller.h"
#include "core/settings/settingsstore.h"
#include "core/settings/shortcutregistry.h"
#include "core/urlsanitizer.h"
#include "core/window/tabmodel.h"
#include "engine/engineprofile.h"
#include "engine/engineview.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace eden::core {

WindowController::WindowController(QObject *parent)
    : QObject(parent),
      m_shortcuts(std::make_unique<ShortcutRegistry>()) {
    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(1000);
    connect(&m_sessionTimer, &QTimer::timeout, this, &WindowController::saveSession);
    connect(m_shortcuts.get(), &ShortcutRegistry::commandTriggered, this, &WindowController::executeCommand);
}

WindowController::~WindowController() {
    saveSession();
}

TabModel *WindowController::tabs() const {
    return m_tabs.get();
}

int WindowController::activeIndex() const {
    return m_activeIndex;
}

QObject *WindowController::currentEngine() const {
    return m_tabs ? m_tabs->engineAt(m_activeIndex) : nullptr;
}

QUrl WindowController::currentUrl() const {
    if (!m_tabs || m_activeIndex < 0 || m_activeIndex >= m_tabs->rowCount()) {
        return {};
    }
    return urlWithoutCredentials(m_tabs->data(m_tabs->index(m_activeIndex), TabModel::UrlRole).toUrl());
}

QString WindowController::displayUrl() const {
    QUrl url = currentUrl();
    if (url.scheme() == "eden" || url.scheme() == "about") {
        return url.toString();
    }
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toDisplayString(QUrl::RemovePassword);
}

QString WindowController::mode() const {
    return m_privateWindow ? "private" : "normal";
}

OmniboxController *WindowController::omnibox() const {
    return m_omnibox.get();
}

ShortcutRegistry *WindowController::shortcuts() const {
    return m_shortcuts.get();
}

HistoryStore *WindowController::history() const {
    return m_history.get();
}

BookmarkStore *WindowController::bookmarks() const {
    return m_bookmarks.get();
}

DownloadManager *WindowController::downloads() const {
    return m_downloads.get();
}

QString WindowController::openPane() const {
    return m_openPane;
}

int WindowController::paneWidth() const {
    return m_paneWidth;
}

bool WindowController::sidebarExpanded() const {
    return m_sidebarExpanded;
}

bool WindowController::findVisible() const {
    return m_findVisible;
}

bool WindowController::commandPaletteVisible() const {
    return m_commandPaletteVisible;
}

void WindowController::initialize(bool privateWindow, const QString &engineName) {
    if (m_initialized) {
        return;
    }
    m_initialized = true;
    m_privateWindow = privateWindow;
    m_history = std::make_unique<HistoryStore>();
    m_bookmarks = std::make_unique<BookmarkStore>();
    m_downloads = std::make_unique<DownloadManager>();
    connect(m_bookmarks.get(), &QAbstractItemModel::rowsInserted, this, &WindowController::currentBookmarkedChanged);
    connect(m_bookmarks.get(), &QAbstractItemModel::rowsRemoved, this, &WindowController::currentBookmarkedChanged);
    connect(m_bookmarks.get(), &QAbstractItemModel::modelReset, this, &WindowController::currentBookmarkedChanged);
    if (engineName != "qtwebengine") {
        qWarning("Only the qtwebengine backend is available in Phase 1");
    }
    if (privateWindow) {
        m_privateProfile = engine::EngineProfile::createPrivateProfile(this);
        m_profile = m_privateProfile.get();
    } else {
        m_profile = engine::EngineProfile::defaultProfile();
    }
    connect(m_profile, &engine::EngineProfile::downloadStarted, m_downloads.get(), &DownloadManager::beginDownload);
    connect(m_profile, &engine::EngineProfile::downloadUpdated, m_downloads.get(), &DownloadManager::updateDownload);
    auto factory = [this] {
        return engine::EngineFactory::create(m_backend, m_profile);
    };
    m_tabs = std::make_unique<TabModel>(std::move(factory), privateWindow);
    m_omnibox = std::make_unique<OmniboxController>(m_tabs.get(), m_history.get(), m_bookmarks.get());
    connect(m_tabs.get(), &TabModel::tabAdded, this, [this](int index, bool background) {
        connectEngine(m_tabs->engineViewAt(index));
        if (!background) {
            setActiveIndex(index);
        }
    });
    connect(m_tabs.get(), &TabModel::operationOccurred, this, &WindowController::scheduleSessionSave);
    connect(m_tabs.get(), &TabModel::tabMoved, this, [this](int from, int to) {
        int nextActiveIndex = m_activeIndex;
        if (m_activeIndex == from) {
            nextActiveIndex = to;
        } else if (from < m_activeIndex && m_activeIndex <= to) {
            --nextActiveIndex;
        } else if (to <= m_activeIndex && m_activeIndex < from) {
            ++nextActiveIndex;
        }
        setActiveIndex(nextActiveIndex);
    });
    connect(m_tabs.get(), &TabModel::externalViewRequested, this, [this](engine::EngineNewViewRequest *request) {
        if (!request) {
            return;
        }
        engine::EngineView *target = nullptr;
        if (request->disposition() == engine::EngineView::Disposition::CurrentTab) {
            target = m_tabs->engineViewAt(m_activeIndex);
        } else {
            const bool background = request->disposition() == engine::EngineView::Disposition::NewBackgroundTab;
            const int row = newTab(QUrl("about:blank"), background);
            target = m_tabs->engineViewAt(row);
        }
        if (!target) {
            return;
        }
        if (!request->openIn(target) && !request->requestedUrl().isEmpty()) {
            target->load(request->requestedUrl());
        }
    });
    connect(m_tabs.get(), &TabModel::tabCloseRequestedForWindow, this, &WindowController::closeWindowRequested);
    connect(m_tabs.get(), &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int first, int last) {
        if (!m_tabs || m_tabs->rowCount() == 0) {
            setActiveIndex(-1);
            return;
        }
        const bool activeRemoved = m_activeIndex >= first && m_activeIndex <= last;
        int nextActiveIndex = m_activeIndex;
        if (m_activeIndex > last) {
            nextActiveIndex = m_activeIndex - (last - first + 1);
        } else if (activeRemoved) {
            nextActiveIndex = first;
        }
        nextActiveIndex = std::min(nextActiveIndex, m_tabs->rowCount() - 1);
        if (nextActiveIndex == m_activeIndex) {
            if (activeRemoved) {
                emit currentEngineChanged();
                emit currentBookmarkedChanged();
                scheduleSessionSave();
            }
            return;
        }
        setActiveIndex(nextActiveIndex);
    });
    emit tabsChanged();
    emit modeChanged();
    if (privateWindow) {
        newTab(QUrl("about:blank"));
    } else {
        restoreSession();
    }
    if (m_tabs->rowCount() == 0) {
        newTab(QUrl("about:blank"));
    }
}

void WindowController::setActiveIndex(int index) {
    if (!m_tabs) {
        index = -1;
    } else if (index < -1 || index >= m_tabs->rowCount()) {
        return;
    }
    if (m_activeIndex == index) {
        return;
    }
    m_activeIndex = index;
    emit activeIndexChanged();
    emit currentEngineChanged();
    emit currentBookmarkedChanged();
    scheduleSessionSave();
}

int WindowController::newTab(const QUrl &url, bool background) {
    return m_tabs ? m_tabs->addTab(url, background) : -1;
}

void WindowController::closeTab(int index) {
    if (m_tabs) {
        m_tabs->closeTab(index);
    }
}

void WindowController::navigate(const QUrl &url) {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->load(url);
    } else if (m_tabs && m_activeIndex >= 0) {
        const int internalIndex = m_activeIndex;
        newTab(url);
        m_tabs->closeTab(internalIndex);
    }
}

void WindowController::navigateText(const QString &text, bool controlEnter) {
    if (m_omnibox) {
        navigate(m_omnibox->destination(text, controlEnter));
    }
}

void WindowController::activateSuggestion(int row) {
    if (m_omnibox) {
        navigate(m_omnibox->suggestionUrl(row));
    }
}

void WindowController::back() {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->back();
    }
}

void WindowController::forward() {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->forward();
    }
}

QVariantList WindowController::navigationHistory(int direction, int maximumItems) const {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        return view->navigationHistory(direction, maximumItems);
    }
    return {};
}

void WindowController::navigateHistory(int offset) {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->goToHistoryOffset(offset);
    }
}

void WindowController::reload() {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->reload();
    }
}

void WindowController::stop() {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->stop();
    }
}

void WindowController::find(const QString &text, bool backward, bool caseSensitive) {
    engine::EngineView::FindFlags flags;
    if (backward) {
        flags |= engine::EngineView::FindBackward;
    }
    if (caseSensitive) {
        flags |= engine::EngineView::FindCaseSensitive;
    }
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        view->findInPage(text, flags);
    }
}

void WindowController::toggleBookmark() {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        m_bookmarks->toggle(view->url(), view->title());
        emit currentBookmarkedChanged();
    }
}

bool WindowController::currentBookmarked() const {
    if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
        return m_bookmarks->contains(view->url());
    }
    return false;
}

void WindowController::openNewWindow(bool privateWindow) {
    QStringList arguments;
    arguments.append("--engine=qtwebengine");
    if (privateWindow) {
        arguments.append("--private");
    }
    QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments);
}

void WindowController::openSettingsTab() {
    openInternalTab(QUrl("eden://settings"));
}

void WindowController::openThemeEditorTab() {
    openInternalTab(QUrl("eden://theme-editor"));
}

void WindowController::openInternalTab(const QUrl &url) {
    if (!m_tabs) {
        return;
    }
    for (int row = 0; row < m_tabs->rowCount(); ++row) {
        if (m_tabs->data(m_tabs->index(row), TabModel::UrlRole).toUrl() == url) {
            setActiveIndex(row);
            return;
        }
    }
    newTab(url);
}

void WindowController::saveSession() {
    if (!m_initialized || m_privateWindow || !m_tabs) {
        return;
    }
    QJsonArray tabsArray;
    for (int row = 0; row < m_tabs->rowCount(); ++row) {
        QJsonObject tab;
        tab.insert("url", urlWithoutCredentials(m_tabs->data(m_tabs->index(row), TabModel::UrlRole).toUrl()).toString());
        tab.insert("title", m_tabs->data(m_tabs->index(row), TabModel::TitleRole).toString());
        tab.insert("pinned", m_tabs->data(m_tabs->index(row), TabModel::PinnedRole).toBool());
        tabsArray.append(tab);
    }
    QJsonObject window;
    window.insert("activeIndex", m_activeIndex);
    window.insert("layout", SettingsStore::instance()->tabLayout());
    window.insert("tabs", tabsArray);
    QSaveFile file(sessionPath());
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(window).toJson(QJsonDocument::Compact));
    file.commit();
}

void WindowController::setOpenPane(const QString &pane) {
    if (pane != "" && pane != "history" && pane != "bookmarks" && pane != "downloads") {
        return;
    }
    if (m_openPane == pane) {
        return;
    }
    m_openPane = pane;
    emit openPaneChanged();
}

void WindowController::beginPaneResize() {
    m_paneResizeStartWidth = m_paneWidth;
}

void WindowController::resizePane(qreal horizontalDelta) {
    const int nextWidth = std::clamp(m_paneResizeStartWidth - qRound(horizontalDelta), 260, 520);
    if (m_paneWidth == nextWidth) {
        return;
    }
    m_paneWidth = nextWidth;
    emit paneWidthChanged();
}

void WindowController::setSidebarExpanded(bool expanded) {
    if (m_sidebarExpanded == expanded) {
        return;
    }
    m_sidebarExpanded = expanded;
    emit sidebarExpandedChanged();
}

void WindowController::setFindVisible(bool visible) {
    if (m_findVisible == visible) {
        return;
    }
    m_findVisible = visible;
    emit findVisibleChanged();
}

void WindowController::setCommandPaletteVisible(bool visible) {
    if (m_commandPaletteVisible == visible) {
        return;
    }
    m_commandPaletteVisible = visible;
    emit commandPaletteVisibleChanged();
}

void WindowController::connectEngine(engine::EngineView *view) {
    if (!view) {
        return;
    }
    connect(view, &engine::EngineView::loadingChanged, this, [this, view] {
        if (!m_privateWindow && !view->isLoading()) {
            m_history->recordVisit(view->url(), view->title());
        }
    });
    connect(view, &engine::EngineView::urlChanged, this, [this, view] {
        if (view == qobject_cast<engine::EngineView *>(currentEngine())) {
            emit currentEngineChanged();
            emit currentBookmarkedChanged();
        }
    });
    connect(view, &engine::EngineView::titleChanged, this, [this, view] {
        if (view == qobject_cast<engine::EngineView *>(currentEngine())) {
            emit currentEngineChanged();
        }
    });
}

void WindowController::restoreSession() {
    QFile file(sessionPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject window = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray tabsArray = window.value("tabs").toArray();
    for (const QJsonValue &value : tabsArray) {
        const QJsonObject tab = value.toObject();
        const int row = newTab(urlWithoutCredentials(QUrl(tab.value("url").toString())), true);
        m_tabs->pinTab(row, tab.value("pinned").toBool());
    }
    setActiveIndex(std::clamp(window.value("activeIndex").toInt(0), 0, std::max(0, m_tabs->rowCount() - 1)));
    const QString layout = window.value("layout").toString();
    if (!layout.isEmpty()) {
        SettingsStore::instance()->setTabLayout(layout);
    }
}

void WindowController::scheduleSessionSave() {
    if (!m_privateWindow) {
        m_sessionTimer.start();
    }
}

void WindowController::executeCommand(const QString &id) {
    if (id == "new_tab") {
        newTab();
    } else if (id == "close_tab") {
        closeTab(m_activeIndex);
    } else if (id == "restore_tab") {
        if (m_tabs && m_tabs->undoClose()) {
            setActiveIndex(std::max(0, m_tabs->rowCount() - 1));
        }
    } else if (id == "next_tab" && m_tabs && m_tabs->rowCount() > 0) {
        setActiveIndex((m_activeIndex + 1) % m_tabs->rowCount());
    } else if (id == "previous_tab" && m_tabs && m_tabs->rowCount() > 0) {
        setActiveIndex((m_activeIndex + m_tabs->rowCount() - 1) % m_tabs->rowCount());
    } else if (id == "focus_omnibox") {
        emit focusOmniboxRequested();
    } else if (id == "bookmark") {
        toggleBookmark();
    } else if (id == "find") {
        setFindVisible(true);
    } else if (id == "private_window") {
        openNewWindow(true);
    } else if (id == "devtools") {
        if (engine::EngineView *view = m_tabs ? m_tabs->engineViewAt(m_activeIndex) : nullptr) {
            view->openDevTools();
        }
    } else if (id == "command_palette") {
        setCommandPaletteVisible(true);
    } else if (id == "settings") {
        openSettingsTab();
    } else if (id == "history") {
        setOpenPane("history");
    } else if (id == "downloads") {
        setOpenPane("downloads");
    } else if (id == "quit") {
        QCoreApplication::quit();
    }
}

QString WindowController::sessionPath() const {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return directory + "/session.json";
}

}
