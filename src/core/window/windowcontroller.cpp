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
#include <QCursor>
#include <QDataStream>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace eden::core {

static QList<WindowController *> &sessionWindows() {
    static QList<WindowController *> windows;
    return windows;
}

static QList<WindowController *> &browserWindows() {
    static QList<WindowController *> windows;
    return windows;
}

struct TabDragSession {
    QPointer<WindowController> grabOwner;
    QPointer<WindowController> current;
    QPointer<WindowController> origin;
    QPointer<WindowController> tornWindow;
    QPointer<QQuickItem> area;
    QPointer<QQuickItem> view;
    QPointF cursorInArea;
    QPointF pointerInWindow;
    qreal pressOffset = 0;
    qreal normalExtent = 0;
    qreal pinnedExtent = 0;
    qreal spacing = 0;
    int currentIndex = -1;
    int originIndex = -1;
    bool vertical = false;
    bool pinned = false;
    bool torn = false;
    bool dnd = false;
    bool hovering = false;
    bool tornCreated = false;
    bool active = false;
};

static TabDragSession &tabDragSession() {
    static TabDragSession session;
    return session;
}

static constexpr qreal kTabTearOffMargin = 48;

static void loadDropMetrics(TabDragSession &session, QQuickItem *area) {
    session.area = area;
    session.view = area ? area->findChild<QQuickItem *>("tabDropView") : nullptr;
    if (!area) {
        return;
    }
    const bool wasVertical = session.vertical;
    session.vertical = area->property("verticalDropLayout").toBool();
    session.normalExtent = area->property("dropNormalExtent").toReal();
    session.pinnedExtent = area->property("dropPinnedExtent").toReal();
    session.spacing = area->property("dropSpacing").toReal();
    const qreal draggedExtent = session.pinned ? session.pinnedExtent : session.normalExtent;
    if (wasVertical != session.vertical) {
        session.pressOffset = draggedExtent / 2;
    }
    session.pressOffset = std::clamp(session.pressOffset, 0.0, std::max(0.0, draggedExtent));
}

static qreal dragLeadingPosition(const TabDragSession &session) {
    QQuickItem *view = session.view;
    const QPointF local = session.area->mapToItem(view, session.cursorInArea);
    const qreal content =
        session.vertical ? local.y() + view->property("contentY").toReal() : local.x() + view->property("contentX").toReal();
    return content - session.pressOffset;
}

static int dragSlotIndex(const TabDragSession &session, qreal leading, int pinnedCount, int count, bool insertion) {
    const qreal pinnedStep = session.pinnedExtent + session.spacing;
    const qreal normalStep = session.normalExtent + session.spacing;
    if (session.pinned) {
        const int slot = pinnedStep > 0 ? qRound(leading / pinnedStep) : 0;
        return std::clamp(slot, 0, insertion ? pinnedCount : std::max(0, pinnedCount - 1));
    }
    const qreal normalStart = pinnedCount * pinnedStep;
    const int slot = pinnedCount + (normalStep > 0 ? qRound((leading - normalStart) / normalStep) : 0);
    return std::clamp(slot, pinnedCount, insertion ? count : std::max(pinnedCount, count - 1));
}

static bool &sessionShutdownStarted() {
    static bool started = false;
    return started;
}

static bool &sessionShutdownHookInstalled() {
    static bool installed = false;
    return installed;
}

WindowController::WindowController(QObject *parent)
    : QObject(parent),
      m_shortcuts(std::make_unique<ShortcutRegistry>()) {
    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(1000);
    connect(&m_sessionTimer, &QTimer::timeout, this, &WindowController::saveSession);
    connect(m_shortcuts.get(), &ShortcutRegistry::commandTriggered, this, &WindowController::executeCommand);
    m_tabDragGuard.setInterval(200);
    connect(&m_tabDragGuard, &QTimer::timeout, this, [this] {
        const TabDragSession &session = tabDragSession();
        if (session.active && !session.dnd && session.grabOwner == this && !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
            endTabDrag(false);
        }
    });
}

WindowController::~WindowController() {
    if (tabDragSession().active && tabDragSession().grabOwner == this) {
        tabDragSession() = {};
    }
    if (m_registeredAsWindow) {
        browserWindows().removeOne(this);
        m_registeredAsWindow = false;
    }
    if (m_registeredForSession) {
        if (!sessionShutdownStarted() && sessionWindows().size() == 1) {
            saveSession();
        }
        sessionWindows().removeOne(this);
        m_registeredForSession = false;
        if (!sessionShutdownStarted() && !sessionWindows().isEmpty()) {
            sessionWindows().constFirst()->saveSession();
        }
    }
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

QVariantList WindowController::pageContextMenuActions() const {
    return m_pageContextMenuActions;
}

QPoint WindowController::pageContextMenuPosition() const {
    return m_pageContextMenuPosition;
}

int WindowController::tabDragRevision() const {
    return m_tabDragRevision;
}

void WindowController::initialize(bool privateWindow, const QString &engineName, bool restorePreviousSession, bool createInitialTab) {
    if (m_initialized) {
        return;
    }
    m_initialized = true;
    m_privateWindow = privateWindow;
    m_engineName = engineName;
    QObject *windowCandidate = parent();
    while (windowCandidate && !m_window) {
        m_window = qobject_cast<QQuickWindow *>(windowCandidate);
        windowCandidate = windowCandidate->parent();
    }
    browserWindows().append(this);
    m_registeredAsWindow = true;
    if (!privateWindow) {
        sessionWindows().append(this);
        m_registeredForSession = true;
        if (!sessionShutdownHookInstalled()) {
            sessionShutdownHookInstalled() = true;
            connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, QCoreApplication::instance(), [] {
                if (!sessionWindows().isEmpty()) {
                    sessionWindows().constFirst()->saveSession();
                }
                sessionShutdownStarted() = true;
            });
        }
    }
    m_history = std::make_unique<HistoryStore>();
    m_bookmarks = std::make_unique<BookmarkStore>();
    m_downloads = std::make_unique<DownloadManager>();
    connect(m_bookmarks.get(), &QAbstractItemModel::rowsInserted, this, &WindowController::currentBookmarkedChanged);
    connect(m_bookmarks.get(), &QAbstractItemModel::rowsRemoved, this, &WindowController::currentBookmarkedChanged);
    connect(m_bookmarks.get(), &QAbstractItemModel::modelReset, this, &WindowController::currentBookmarkedChanged);
    if (engineName != "qtwebengine") {
        qWarning("Only the qtwebengine backend is available in Phase 1");
    }
    QQmlEngine *qml = qmlEngine(this);
    if (privateWindow) {
        m_privateProfile = engine::EngineProfile::createPrivateProfile(qml);
        m_profile = m_privateProfile.get();
    } else {
        m_profile = engine::EngineProfile::defaultProfile(qml);
    }
    connectProfile(m_profile);
    auto factory = [this] {
        return engine::EngineFactory::create(m_backend, m_profile);
    };
    m_tabs = std::make_unique<TabModel>(std::move(factory), privateWindow, nullptr, m_privateProfile);
    m_omnibox = std::make_unique<OmniboxController>(m_tabs.get(), m_history.get(), m_bookmarks.get());
    connect(m_tabs.get(), &TabModel::tabAdded, this, [this](int index, bool background) {
        connectProfile(m_tabs->profileAt(index));
        connectEngine(m_tabs->engineViewAt(index));
        if (!background) {
            setActiveIndex(index);
        }
    });
    connect(m_tabs.get(), &TabModel::tabTransferredOut, this, [this](engine::EngineView *view) {
        if (view) {
            disconnect(view, nullptr, this, nullptr);
        }
        if (m_pageContextMenuEngine == view) {
            dismissPageContextMenu();
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
        } else if (request->disposition() == engine::EngineView::Disposition::NewWindow) {
            WindowController *destination = createBrowserWindow(m_privateWindow);
            target = destination ? qobject_cast<engine::EngineView *>(destination->currentEngine()) : nullptr;
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
    connect(m_tabs.get(), &TabModel::tabCloseRequestedForWindow, this, [this] {
        const TabDragSession &session = tabDragSession();
        if (session.active && m_window && (session.grabOwner == this || session.tornWindow == this)) {
            m_closeDeferredForTabDrag = true;
            m_window->setVisible(false);
            return;
        }
        emit closeWindowRequested();
    });
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
    if (!privateWindow && restorePreviousSession) {
        restoreSession();
    }
    if (createInitialTab && m_tabs->rowCount() == 0) {
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
    dismissPageContextMenu();
    m_activeIndex = index;
    emit activeIndexChanged();
    emit currentEngineChanged();
    emit currentBookmarkedChanged();
    scheduleSessionSave();
}

int WindowController::newTab(const QUrl &url, bool background) {
    return m_tabs ? m_tabs->addTab(url, background) : -1;
}

int WindowController::newTabAndFocusOmnibox() {
    const int index = newTab();
    if (index >= 0) {
        QMetaObject::invokeMethod(this, [this] { emit focusOmniboxRequested(); }, Qt::QueuedConnection);
    }
    return index;
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
        m_omnibox->setQuery({});
    }
}

void WindowController::activateSuggestion(int row) {
    if (m_omnibox) {
        navigate(m_omnibox->suggestionUrl(row));
        m_omnibox->setQuery({});
    }
}

void WindowController::beginTabDrag(int index, QQuickItem *visual, qreal pressX, qreal pressY) {
    if (!m_tabs || !m_window || !visual || index < 0 || index >= m_tabs->rowCount() || tabDragSession().active) {
        return;
    }
    QQuickItem *area = visual;
    while (area && area->objectName() != "tabDropArea") {
        area = area->parentItem();
    }
    if (!area) {
        return;
    }
    setActiveIndex(index);
    TabDragSession &session = tabDragSession();
    session = {};
    session.grabOwner = this;
    session.current = this;
    session.origin = this;
    session.currentIndex = index;
    session.originIndex = index;
    session.pinned = m_tabs->data(m_tabs->index(index), TabModel::PinnedRole).toBool();
    loadDropMetrics(session, area);
    session.pressOffset = session.vertical ? pressY : pressX;
    const qreal draggedExtent = session.pinned ? session.pinnedExtent : session.normalExtent;
    session.pressOffset = std::clamp(session.pressOffset, 0.0, std::max(0.0, draggedExtent));
    session.cursorInArea = area->mapFromItem(visual, QPointF(pressX, pressY));
    session.active = true;
    QCoreApplication::instance()->installEventFilter(this);
    m_tabDragGuard.start();
    refreshTabDragVisuals();
}

bool WindowController::eventFilter(QObject *watched, QEvent *event) {
    TabDragSession &session = tabDragSession();
    if (!session.active || session.dnd || session.grabOwner != this) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        endTabDrag(true);
        return true;
    }
    if (!watched->isWindowType() || watched != m_window) {
        return QObject::eventFilter(watched, event);
    }
    if (event->type() == QEvent::MouseMove) {
        updateTabDrag(static_cast<QMouseEvent *>(event)->position());
    } else if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        updateTabDrag(static_cast<QMouseEvent *>(event)->position());
        endTabDrag(false);
    }
    return QObject::eventFilter(watched, event);
}

void WindowController::updateTabDrag(const QPointF &windowPosition) {
    TabDragSession &session = tabDragSession();
    WindowController *current = session.current;
    if (!current || !current->m_tabs || !session.area || !session.view) {
        endTabDrag(true);
        return;
    }
    session.pointerInWindow = windowPosition;
    const QPointF local = session.area->mapFromScene(windowPosition);
    const qreal perpendicular = session.vertical ? local.x() : local.y();
    const qreal perpendicularExtent = session.vertical ? session.area->width() : session.area->height();
    const qreal along = session.vertical ? local.y() : local.x();
    const qreal alongExtent = session.vertical ? session.area->height() : session.area->width();
    if (perpendicular < -kTabTearOffMargin || perpendicular > perpendicularExtent + kTabTearOffMargin || along < -kTabTearOffMargin ||
        along > alongExtent + kTabTearOffMargin) {
        beginTornDrag();
        return;
    }
    session.cursorInArea = local;
    reorderDraggedTab();
    refreshTabDragVisuals();
}

void WindowController::reorderDraggedTab() {
    TabDragSession &session = tabDragSession();
    WindowController *current = session.current;
    QQuickItem *view = session.view;
    if (!current || !current->m_tabs || !session.area || !view) {
        return;
    }
    const QPointF local = session.area->mapToItem(view, session.cursorInArea);
    const qreal along = session.vertical ? local.y() : local.x();
    const qreal viewExtent = session.vertical ? view->height() : view->width();
    const char *contentProperty = session.vertical ? "contentY" : "contentX";
    const char *contentExtentProperty = session.vertical ? "contentHeight" : "contentWidth";
    const qreal maximum = std::max<qreal>(0, view->property(contentExtentProperty).toReal() - viewExtent);
    qreal contentPosition = view->property(contentProperty).toReal();
    if (along < 40 && contentPosition > 0) {
        view->setProperty(contentProperty, std::max<qreal>(0, contentPosition - 18));
    } else if (along > viewExtent - 40 && contentPosition < maximum) {
        view->setProperty(contentProperty, std::min(maximum, contentPosition + 18));
    }
    const int count = current->m_tabs->rowCount();
    const int pinnedCount = current->m_tabs->pinnedCount();
    const int slot = dragSlotIndex(session, dragLeadingPosition(session), pinnedCount, count, false);
    if (slot != session.currentIndex && current->m_tabs->moveTab(session.currentIndex, slot)) {
        session.currentIndex = slot;
    }
}

void WindowController::beginTornDrag() {
    TabDragSession &session = tabDragSession();
    WindowController *source = session.current;
    QQuickWindow *sourceWindow = source->m_window;
    if (!sourceWindow) {
        endTabDrag(true);
        return;
    }
    const QPointF pointerInWindow = session.pointerInWindow;
    const bool waylandPlatform = QGuiApplication::platformName().startsWith("wayland");
    QQuickItem *sourceArea = session.area;
    QPointF anchored = pointerInWindow;
    QPointF attachPoint = pointerInWindow;
    if (sourceArea) {
        anchored = sourceArea->mapFromScene(pointerInWindow);
        if (session.vertical) {
            anchored.setX(sourceArea->width() / 2);
            anchored.setY(std::clamp(anchored.y(), 0.0, sourceArea->height()));
        } else {
            anchored.setY(sourceArea->height() / 2);
            anchored.setX(std::clamp(anchored.x(), 0.0, sourceArea->width()));
        }
        attachPoint = sourceArea->mapToScene(anchored);
    }
    WindowController *torn = source;
    if (source->m_tabs->rowCount() > 1) {
        const QPoint tornPosition = sourceWindow->position() + (pointerInWindow - attachPoint).toPoint();
        torn = createBrowserWindow(source->m_privateWindow, false, waylandPlatform ? QPoint() : tornPosition, sourceWindow->size());
        if (!torn || !torn->m_window) {
            return;
        }
        if (!source->m_tabs->transferTabTo(session.currentIndex, torn->m_tabs.get(), 0)) {
            torn->m_window->close();
            return;
        }
    }
    session.tornWindow = torn;
    session.tornCreated = torn != source;
    session.current = torn;
    session.currentIndex = 0;
    session.torn = true;
    session.dnd = true;
    session.hovering = false;
    loadDropMetrics(session, torn->visibleDropArea());
    session.cursorInArea = anchored;
    QCoreApplication::instance()->removeEventFilter(this);
    m_tabDragGuard.stop();
    refreshTabDragVisuals();

    QMimeData *mimeData = new QMimeData();
    mimeData->setData("application/x-eden-tab", QByteArrayLiteral("move"));
    QByteArray windowData;
    QDataStream windowStream(&windowData, QIODevice::WriteOnly);
    windowStream << reinterpret_cast<qintptr>(static_cast<QWindow *>(torn->m_window.data()));
    mimeData->setData("application/x-qt-mainwindowdrag-window", windowData);
    QByteArray offsetData;
    QDataStream offsetStream(&offsetData, QIODevice::WriteOnly);
    offsetStream << attachPoint.toPoint();
    mimeData->setData("application/x-qt-mainwindowdrag-position", offsetData);

    QDrag *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    QPixmap pixmap(1, 1);
    pixmap.fill(Qt::transparent);
    drag->setPixmap(pixmap);
    const auto settleTornMetrics = [] {
        TabDragSession &settled = tabDragSession();
        if (settled.active && settled.torn && settled.area) {
            loadDropMetrics(settled, settled.area);
        }
        refreshTabDragVisuals();
    };
    if (QQuickItem *tornArea = session.area) {
        connect(tornArea, &QQuickItem::widthChanged, drag, settleTornMetrics);
        connect(tornArea, &QQuickItem::heightChanged, drag, settleTornMetrics);
    }
    if (QQuickItem *tornView = session.view) {
        connect(tornView, &QQuickItem::widthChanged, drag, settleTornMetrics);
        connect(tornView, &QQuickItem::heightChanged, drag, settleTornMetrics);
    }
    QTimer::singleShot(0, drag, settleTornMetrics);
    if (!waylandPlatform) {
        QTimer *mover = new QTimer(drag);
        const QPoint grabOffset = attachPoint.toPoint();
        QPointer<WindowController> tornGuard(torn);
        connect(mover, &QTimer::timeout, drag, [tornGuard, grabOffset] {
            if (tornGuard && tornGuard->m_window && tornGuard->m_window->isVisible()) {
                tornGuard->m_window->setPosition(QCursor::pos() - grabOffset);
            }
        });
        mover->start(16);
    }
    const Qt::DropAction result = drag->exec(Qt::MoveAction);
    finalizeTornDrag(waylandPlatform && result == Qt::IgnoreAction);
    drag->deleteLater();
}

void WindowController::tabDragEntered(QQuickItem *area, qreal x, qreal y) {
    handleStripDrag(area, x, y);
}

void WindowController::tabDragMoved(QQuickItem *area, qreal x, qreal y) {
    handleStripDrag(area, x, y);
}

void WindowController::tabDragLeft() {
    TabDragSession &session = tabDragSession();
    if (session.active && session.dnd && session.current == this) {
        session.hovering = false;
        refreshTabDragVisuals();
    }
}

bool WindowController::tabDragDropped(QQuickItem *area, qreal x, qreal y) {
    TabDragSession &session = tabDragSession();
    if (!session.active || !session.dnd) {
        return false;
    }
    handleStripDrag(area, x, y);
    return session.current == this;
}

void WindowController::handleStripDrag(QQuickItem *area, qreal x, qreal y) {
    TabDragSession &session = tabDragSession();
    if (!session.active || !session.dnd || !m_tabs || !area) {
        return;
    }
    WindowController *current = session.current;
    if (!current || !current->m_tabs) {
        return;
    }
    if (current == this) {
        session.cursorInArea = QPointF(x, y);
        session.hovering = true;
        reorderDraggedTab();
        refreshTabDragVisuals();
        return;
    }
    QQuickItem *previousArea = session.area;
    QQuickItem *previousView = session.view;
    const bool previousVertical = session.vertical;
    const qreal previousPressOffset = session.pressOffset;
    loadDropMetrics(session, area);
    if (!session.view) {
        session.area = previousArea;
        session.view = previousView;
        session.vertical = previousVertical;
        session.pressOffset = previousPressOffset;
        return;
    }
    session.cursorInArea = QPointF(x, y);
    const int slot = dragSlotIndex(session, dragLeadingPosition(session), m_tabs->pinnedCount(), m_tabs->rowCount(), true);
    if (!current->m_tabs->transferTabTo(session.currentIndex, m_tabs.get(), slot)) {
        session.area = previousArea;
        session.view = previousView;
        session.vertical = previousVertical;
        session.pressOffset = previousPressOffset;
        return;
    }
    session.current = this;
    session.currentIndex = slot;
    session.torn = false;
    session.hovering = true;
    if (m_window) {
        m_window->raise();
        m_window->requestActivate();
    }
    refreshTabDragVisuals();
}

void WindowController::finalizeTornDrag(bool canceled) {
    TabDragSession &session = tabDragSession();
    if (!session.active) {
        return;
    }
    session.active = false;
    WindowController *current = session.current;
    WindowController *origin = session.origin;
    WindowController *torn = session.tornWindow;
    const bool endedTorn = session.torn;
    const int currentIndex = session.currentIndex;
    const int originIndex = session.originIndex;
    session = {};
    if (canceled && current && current->m_tabs && origin && origin == current && !endedTorn && currentIndex >= 0) {
        current->m_tabs->moveTab(currentIndex, originIndex);
    } else if (canceled && current && current->m_tabs && origin && origin != current && origin->m_tabs && currentIndex >= 0) {
        current->m_tabs->transferTabTo(currentIndex, origin->m_tabs.get(), originIndex);
        if (origin->m_window) {
            origin->m_window->raise();
            origin->m_window->requestActivate();
        }
    } else if (current && current->m_window) {
        if (endedTorn && torn == current) {
            current->m_window->requestActivate();
        }
    }
    for (WindowController *candidate : std::as_const(browserWindows())) {
        if (candidate && candidate->m_closeDeferredForTabDrag) {
            candidate->m_closeDeferredForTabDrag = false;
            emit candidate->closeWindowRequested();
        }
    }
    refreshTabDragVisuals();
}

void WindowController::endTabDrag(bool canceled) {
    TabDragSession &session = tabDragSession();
    if (!session.active || session.dnd) {
        return;
    }
    session.active = false;
    QCoreApplication::instance()->removeEventFilter(this);
    m_tabDragGuard.stop();
    WindowController *current = session.current;
    WindowController *origin = session.origin;
    if (canceled && current && current->m_tabs && origin == current && session.currentIndex >= 0) {
        current->m_tabs->moveTab(session.currentIndex, session.originIndex);
    }
    session = {};
    refreshTabDragVisuals();
}

qreal WindowController::tabDragTranslation(int index, qreal itemPosition) const {
    const TabDragSession &session = tabDragSession();
    if (!session.active || session.current != this || index != session.currentIndex || !session.view || !session.area || !m_tabs) {
        return 0;
    }
    if (session.dnd && !session.torn && !session.hovering) {
        return 0;
    }
    const qreal extent = session.pinned ? session.pinnedExtent : session.normalExtent;
    qreal leading = dragLeadingPosition(session);
    if (session.torn) {
        QQuickItem *area = session.area;
        QQuickItem *view = session.view;
        const qreal freshExtent = area->property(session.pinned ? "dropPinnedExtent" : "dropNormalExtent").toReal();
        const QPointF areaOrigin = area->mapToItem(view, QPointF(0, 0));
        const qreal contentOffset = session.vertical ? view->property("contentY").toReal() : view->property("contentX").toReal();
        const qreal areaStart = (session.vertical ? areaOrigin.y() : areaOrigin.x()) + contentOffset;
        const qreal areaExtent = session.vertical ? area->height() : area->width();
        leading =
            areaExtent > freshExtent ? std::clamp(leading, areaStart, areaStart + areaExtent - freshExtent) : std::max(leading, areaStart);
    } else {
        const int count = m_tabs->rowCount();
        const int pinnedCount = m_tabs->pinnedCount();
        const qreal pinnedStep = session.pinnedExtent + session.spacing;
        const qreal normalStep = session.normalExtent + session.spacing;
        qreal minimum = session.pinned ? 0 : pinnedCount * pinnedStep;
        qreal maximum = session.pinned ? std::max(0, pinnedCount - 1) * pinnedStep
                                       : pinnedCount * pinnedStep + std::max(0, count - 1 - pinnedCount) * normalStep;
        leading = std::clamp(leading, minimum, std::max(minimum, maximum));
    }
    return leading - itemPosition;
}

int WindowController::tabDragIndex() const {
    const TabDragSession &session = tabDragSession();
    return session.active && session.current == this ? session.currentIndex : -1;
}

bool WindowController::tabDragTorn() const {
    const TabDragSession &session = tabDragSession();
    return session.active && session.torn && session.current == this;
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
    if (WindowController *controller = createBrowserWindow(privateWindow)) {
        QMetaObject::invokeMethod(controller, [controller] { emit controller->focusOmniboxRequested(); }, Qt::QueuedConnection);
    }
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
    if (!m_initialized || m_privateWindow || !m_registeredForSession) {
        return;
    }
    QJsonArray windows;
    for (const WindowController *controller : std::as_const(sessionWindows())) {
        if (controller && controller->m_initialized && controller->m_registeredForSession) {
            windows.append(controller->sessionWindow());
        }
    }
    QJsonObject session;
    session.insert("version", 2);
    session.insert("windows", windows);
    QSaveFile file(sessionPath());
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    file.write(QJsonDocument(session).toJson(QJsonDocument::Compact));
    file.commit();
}

void WindowController::prepareToClose() {
    if (m_registeredAsWindow) {
        browserWindows().removeOne(this);
        m_registeredAsWindow = false;
    }
    if (!m_registeredForSession) {
        return;
    }
    if (sessionWindows().size() == 1) {
        saveSession();
    }
    sessionWindows().removeOne(this);
    m_registeredForSession = false;
    if (!sessionWindows().isEmpty()) {
        sessionWindows().constFirst()->saveSession();
    }
}

void WindowController::executePageContextMenuCommand(const QString &command) {
    if (m_pageContextMenuEngine) {
        m_pageContextMenuEngine->executeContextMenuCommand(command);
    }
    dismissPageContextMenu();
}

void WindowController::dismissPageContextMenu() {
    if (m_pageContextMenuActions.isEmpty() && !m_pageContextMenuEngine) {
        return;
    }
    m_pageContextMenuActions.clear();
    m_pageContextMenuEngine.clear();
    emit pageContextMenuChanged();
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

void WindowController::connectProfile(engine::EngineProfile *profile) {
    if (!profile || !m_downloads) {
        return;
    }
    connect(profile, &engine::EngineProfile::downloadStarted, m_downloads.get(), &DownloadManager::beginDownload, Qt::UniqueConnection);
    connect(profile, &engine::EngineProfile::downloadUpdated, m_downloads.get(), &DownloadManager::updateDownload, Qt::UniqueConnection);
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
    connect(view, &engine::EngineView::contextMenuRequested, this, [this, view](const engine::ContextMenuInfo &info) {
        if (view != qobject_cast<engine::EngineView *>(currentEngine())) {
            return;
        }
        QVariantList actions;
        const auto append = [&actions](const QString &id, const QString &title, const QString &icon = {}, bool enabled = true) {
            QVariantMap action;
            action.insert("id", id);
            action.insert("title", title);
            action.insert("icon", icon);
            action.insert("enabled", enabled);
            actions.append(action);
        };
        if (!info.linkUrl.isEmpty()) {
            append("open_link_new_tab", "Open link in new tab", "new-window");
            append("copy_link", "Copy link address", "copy");
            append("download_link", "Save link as", "download");
        }
        if (info.editable) {
            append("cut", "Cut");
            append("copy", "Copy", "copy");
            append("paste", "Paste");
            append("select_all", "Select all");
        } else if (!info.selectedText.isEmpty()) {
            append("copy", "Copy", "copy");
        }
        append("back", "Back", "arrow-left", view->canGoBack());
        append("forward", "Forward", "arrow-right", view->canGoForward());
        append("reload", "Reload", "refresh");
        append("view_source", "View page source", "code");
        append("inspect", "Inspect", "code");
        m_pageContextMenuActions = std::move(actions);
        m_pageContextMenuPosition = info.position;
        m_pageContextMenuEngine = view;
        emit pageContextMenuChanged();
        emit pageContextMenuRequested();
    });
}

WindowController *WindowController::createBrowserWindow(bool privateWindow, bool createInitialTab, const QPoint &position,
                                                        const QSize &size) {
    QQmlEngine *engine = qmlEngine(this);
    if (!engine) {
        return nullptr;
    }
    QQmlComponent component(engine);
    component.loadFromModule("Eden.Ui", "BrowserWindow");
    if (component.isError()) {
        qWarning().noquote() << component.errorString();
        return nullptr;
    }
    QObject *created = component.createWithInitialProperties({{"visible", false}});
    QQuickWindow *window = qobject_cast<QQuickWindow *>(created);
    WindowController *controller = window ? window->findChild<WindowController *>("windowController") : nullptr;
    if (!window || !controller) {
        delete created;
        return nullptr;
    }
    QQmlEngine::setObjectOwnership(window, QQmlEngine::CppOwnership);
    controller->initialize(privateWindow, m_engineName, false, createInitialTab);
    connect(window, &QQuickWindow::closing, window, [window] { window->deleteLater(); });
    if (size.isValid()) {
        window->resize(size);
    }
    if (!position.isNull()) {
        window->setPosition(position);
    }
    window->show();
    return controller;
}

QQuickItem *WindowController::visibleDropArea() const {
    if (!m_window) {
        return nullptr;
    }
    const QList<QQuickItem *> areas = m_window->findChildren<QQuickItem *>("tabDropArea");
    for (QQuickItem *area : areas) {
        if (area->isVisible() && area->opacity() > 0) {
            return area;
        }
    }
    return nullptr;
}

void WindowController::refreshTabDragVisuals() {
    for (WindowController *controller : std::as_const(browserWindows())) {
        if (!controller) {
            continue;
        }
        ++controller->m_tabDragRevision;
        emit controller->tabDragRevisionChanged();
    }
}

QJsonObject WindowController::sessionWindow() const {
    QJsonArray tabsArray;
    if (m_tabs) {
        for (int row = 0; row < m_tabs->rowCount(); ++row) {
            QJsonObject tab;
            tab.insert("url", urlWithoutCredentials(m_tabs->data(m_tabs->index(row), TabModel::UrlRole).toUrl()).toString());
            tab.insert("title", m_tabs->data(m_tabs->index(row), TabModel::TitleRole).toString());
            tab.insert("pinned", m_tabs->data(m_tabs->index(row), TabModel::PinnedRole).toBool());
            tabsArray.append(tab);
        }
    }
    QJsonObject window;
    window.insert("activeIndex", m_activeIndex);
    window.insert("layout", SettingsStore::instance()->tabLayout());
    window.insert("tabs", tabsArray);
    return window;
}

void WindowController::restoreWindow(const QJsonObject &window) {
    const QJsonArray tabsArray = window.value("tabs").toArray();
    for (const QJsonValue &value : tabsArray) {
        const QJsonObject tab = value.toObject();
        const int row = newTab(urlWithoutCredentials(QUrl(tab.value("url").toString())), true);
        m_tabs->pinTab(row, tab.value("pinned").toBool());
    }
    if (m_tabs->rowCount() > 0) {
        setActiveIndex(std::clamp(window.value("activeIndex").toInt(0), 0, m_tabs->rowCount() - 1));
    }
    const QString layout = window.value("layout").toString();
    if (!layout.isEmpty()) {
        SettingsStore::instance()->setTabLayout(layout);
    }
}

void WindowController::restoreSession() {
    QFile file(sessionPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QJsonArray windows = root.value("windows").toArray();
    if (windows.isEmpty() && root.contains("tabs")) {
        windows.append(root);
    }
    if (windows.isEmpty()) {
        return;
    }
    restoreWindow(windows.at(0).toObject());
    for (qsizetype index = 1; index < windows.size(); ++index) {
        WindowController *controller = createBrowserWindow(false, false);
        if (!controller) {
            continue;
        }
        controller->restoreWindow(windows.at(index).toObject());
        if (controller->m_tabs->rowCount() == 0) {
            controller->newTab(QUrl("about:blank"));
        }
    }
}

void WindowController::scheduleSessionSave() {
    if (!m_privateWindow) {
        m_sessionTimer.start();
    }
}

void WindowController::executeCommand(const QString &id) {
    if (id == "new_tab") {
        newTabAndFocusOmnibox();
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
