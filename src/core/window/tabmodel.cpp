#include "core/window/tabmodel.h"
#include "core/urlsanitizer.h"
#include "engine/engineprofile.h"
#include "engine/engineregistry.h"
#include "engine/engineview.h"

#include <QJsonObject>

#include <algorithm>
#include <atomic>

namespace eden::core {

static quint64 nextTabId() {
    static std::atomic<quint64> next = 1;
    return next.fetch_add(1, std::memory_order_relaxed);
}

static QString internalPageTitle(const QString &page) {
    if (page == "settings") {
        return "Settings";
    }
    if (page == "theme-editor") {
        return "Theme Editor";
    }
    return "Eden";
}

QString TabModel::internalPageForUrl(const QUrl &url) {
    if (url.scheme() != "eden") {
        return {};
    }
    const QString host = url.host();
    if (host == "settings" || host == "theme-editor") {
        return host;
    }
    return {};
}

TabModel::TabModel(LegacyViewFactory factory, bool privateMode, QObject *parent, std::shared_ptr<engine::EngineProfile> profileLease)
    : TabModel([factory = std::move(factory)](engine::Backend, engine::EngineProfile *) { return factory ? factory() : nullptr; },
               [profileLease = std::move(profileLease)](engine::Backend) { return profileLease; }, engine::EngineRegistry::instance(),
               engine::Backend::QtWebEngine, privateMode, parent) {}

TabModel::TabModel(ViewFactory factory, ProfileResolver profileResolver, const engine::EngineRegistry *registry,
                   engine::Backend defaultBackend, bool privateMode, QObject *parent)
    : QAbstractListModel(parent),
      m_factory(std::move(factory)),
      m_profileResolver(std::move(profileResolver)),
      m_registry(registry ? registry : engine::EngineRegistry::instance()),
      m_defaultBackend(defaultBackend),
      m_privateMode(privateMode) {
    if (!m_registry->contains(m_defaultBackend) && !m_registry->descriptors().isEmpty()) {
        m_defaultBackend = m_registry->descriptors().constFirst().backend;
    }
    m_undoTimer.setSingleShot(true);
    m_undoTimer.setInterval(10000);
    connect(&m_undoTimer, &QTimer::timeout, this, [this] {
        m_closedTab.reset();
        emit canUndoCloseChanged();
    });
}

TabModel::~TabModel() = default;

int TabModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_tabs.size());
}

QVariant TabModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const Tab &tab = *m_tabs.at(index.row());
    engine::EngineView *view = tab.view.get();
    if (role == TitleRole) {
        if (!view || view->title().isEmpty()) {
            return tab.title.isEmpty() ? tab.url.host() : tab.title;
        }
        return view->title();
    }
    if (role == UrlRole) {
        const QUrl currentUrl = view && !view->url().isEmpty() ? view->url() : tab.url;
        return urlWithoutCredentials(currentUrl);
    }
    if (role == FaviconRole) {
        return view && !view->faviconUrl().isEmpty() ? view->faviconUrl() : tab.favicon;
    }
    if (role == LoadingRole) {
        return view && view->isLoading();
    }
    if (role == ProgressRole) {
        return view ? view->loadProgress() : 100;
    }
    if (role == PinnedRole) {
        return tab.pinned;
    }
    if (role == AudibleRole) {
        return view && view->isAudible();
    }
    if (role == MutedRole) {
        return view ? view->isMuted() : tab.state.value("muted").toBool();
    }
    if (role == PrivateRole) {
        return tab.privateMode;
    }
    if (role == EngineViewRole) {
        return QVariant::fromValue(static_cast<QObject *>(view));
    }
    if (role == InternalPageRole) {
        return tab.internalPage;
    }
    if (role == EngineNameRole) {
        return m_registry->displayName(tab.backend);
    }
    if (role == TabIdRole) {
        return QVariant::fromValue(tab.id);
    }
    if (role == DiscardedRole) {
        return !view && tab.internalPage.isEmpty();
    }
    return {};
}

QHash<int, QByteArray> TabModel::roleNames() const {
    return {{TitleRole, "title"},
            {UrlRole, "url"},
            {FaviconRole, "favicon"},
            {LoadingRole, "isLoading"},
            {ProgressRole, "progress"},
            {PinnedRole, "isPinned"},
            {AudibleRole, "isAudible"},
            {MutedRole, "isMuted"},
            {PrivateRole, "isPrivate"},
            {EngineViewRole, "engineView"},
            {InternalPageRole, "internalPage"},
            {EngineNameRole, "engineName"},
            {TabIdRole, "tabId"},
            {DiscardedRole, "discarded"}};
}

int TabModel::addTab(const QUrl &url, bool background, const QString &backendId) {
    std::unique_ptr<Tab> tab = std::make_unique<Tab>();
    tab->id = nextTabId();
    tab->privateMode = m_privateMode;
    tab->backend = resolvedBackend(backendId);
    tab->url = url.isEmpty() ? QUrl("about:blank") : url;
    tab->state.insert("url", tab->url);
    tab->internalPage = internalPageForUrl(tab->url);
    if (!tab->internalPage.isEmpty()) {
        tab->title = internalPageTitle(tab->internalPage);
    }
    const int row = rowCount();
    beginInsertRows({}, row, row);
    m_tabs.push_back(std::move(tab));
    endInsertRows();
    if (m_tabs.back()->internalPage.isEmpty()) {
        createView(row);
    }
    emit countChanged();
    emit operationOccurred();
    emit tabAdded(row, background);
    return row;
}

bool TabModel::closeTab(int row) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    const bool hadUndo = canUndoClose();
    const bool removedPinned = m_tabs.at(row)->pinned;
    if (m_tabs.at(row)->view) {
        m_tabs.at(row)->view->closeDevTools();
    }
    beginRemoveRows({}, row, row);
    std::unique_ptr<Tab> removed = std::move(m_tabs.at(row));
    m_tabs.erase(m_tabs.begin() + row);
    endRemoveRows();
    updateCachedState(*removed);
    removed->view.reset();
    removed->profileLease.reset();
    m_closedTab = std::make_unique<ClosedTab>();
    m_closedTab->tab = std::move(removed);
    m_closedTab->index = row;
    m_undoTimer.start();
    emit countChanged();
    if (removedPinned) {
        emit pinnedCountChanged();
    }
    if (!hadUndo) {
        emit canUndoCloseChanged();
    }
    emit operationOccurred();
    if (m_tabs.empty()) {
        emit tabCloseRequestedForWindow();
    }
    return true;
}

bool TabModel::undoClose() {
    if (!m_closedTab) {
        return false;
    }
    const bool restoredPinned = m_closedTab->tab->pinned;
    const int row =
        restoredPinned ? std::clamp(m_closedTab->index, 0, pinnedCount()) : std::clamp(m_closedTab->index, pinnedCount(), rowCount());
    beginInsertRows({}, row, row);
    m_tabs.insert(m_tabs.begin() + row, std::move(m_closedTab->tab));
    endInsertRows();
    m_closedTab.reset();
    m_undoTimer.stop();
    emit countChanged();
    if (restoredPinned) {
        emit pinnedCountChanged();
    }
    emit canUndoCloseChanged();
    emit operationOccurred();
    return true;
}

bool TabModel::moveTab(int from, int to) {
    if (from < 0 || from >= rowCount() || to < 0 || to >= rowCount() || from == to) {
        return false;
    }
    const int destination = to > from ? to + 1 : to;
    if (!beginMoveRows({}, from, from, {}, destination)) {
        return false;
    }
    std::unique_ptr<Tab> moved = std::move(m_tabs.at(from));
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, std::move(moved));
    endMoveRows();
    emit tabMoved(from, to);
    emit operationOccurred();
    return true;
}

void TabModel::pinTab(int row, bool pinned) {
    if (row < 0 || row >= rowCount() || m_tabs.at(row)->pinned == pinned) {
        return;
    }
    m_tabs.at(row)->pinned = pinned;
    emit dataChanged(index(row), index(row), {PinnedRole});
    emit pinnedCountChanged();
    emit operationOccurred();
    const int boundary = pinned ? pinnedCount() - 1 : pinnedCount();
    if (row != boundary) {
        moveTab(row, boundary);
    }
}

int TabModel::duplicateTab(int row) {
    if (row < 0 || row >= rowCount()) {
        return -1;
    }
    const int added = addTab(data(index(row), UrlRole).toUrl(), false, backendIdAt(row));
    if (added < 0) {
        return added;
    }
    const int target = std::clamp(row + 1, pinnedCount(), rowCount() - 1);
    if (target != added && moveTab(added, target)) {
        return target;
    }
    return added;
}

void TabModel::closeOthers(int row) {
    if (row < 0 || row >= rowCount()) {
        return;
    }
    bool removedAny = false;
    for (int candidate = rowCount() - 1; candidate >= 0; --candidate) {
        if (candidate == row || m_tabs.at(candidate)->pinned) {
            continue;
        }
        beginRemoveRows({}, candidate, candidate);
        m_tabs.erase(m_tabs.begin() + candidate);
        endRemoveRows();
        removedAny = true;
    }
    if (!removedAny) {
        return;
    }
    emit countChanged();
    emit operationOccurred();
}

void TabModel::toggleMuted(int row) {
    if (engine::EngineView *view = engineViewAt(row)) {
        view->setMuted(!view->isMuted());
    }
}

QObject *TabModel::engineAt(int row) const {
    return engineViewAt(row);
}

bool TabModel::convertTab(int row, const QString &backendId, bool rehydrate) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    const std::optional<engine::Backend> backend = m_registry->backendForId(backendId);
    if (!backend) {
        return false;
    }
    Tab &tab = *m_tabs.at(row);
    if (tab.backend == *backend) {
        return !rehydrate || ensureEngine(row);
    }
    discardTab(row);
    tab.backend = *backend;
    bool created = true;
    if (rehydrate && tab.internalPage.isEmpty()) {
        created = createView(row);
    }
    emit dataChanged(index(row), index(row),
                     {EngineNameRole, EngineViewRole, LoadingRole, ProgressRole, AudibleRole, MutedRole, DiscardedRole});
    emit operationOccurred();
    return created;
}

bool TabModel::convertAllTabs(const QString &backendId, int activeIndex) {
    const std::optional<engine::Backend> backend = m_registry->backendForId(backendId);
    if (!backend) {
        return false;
    }
    bool converted = true;
    for (int row = 0; row < rowCount(); ++row) {
        if (row == activeIndex) {
            converted = convertTab(row, backendId, true) && converted;
        } else if (m_tabs.at(row)->backend == *backend) {
            converted = discardTab(row) && converted;
        } else {
            converted = convertTab(row, backendId, false) && converted;
        }
    }
    return converted;
}

bool TabModel::ensureEngine(int row) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    Tab &tab = *m_tabs.at(row);
    return tab.view || !tab.internalPage.isEmpty() || createView(row);
}

bool TabModel::setInternalPage(int row, const QUrl &url) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    const QString page = internalPageForUrl(url);
    if (page.isEmpty()) {
        return false;
    }
    discardTab(row);
    Tab &tab = *m_tabs.at(row);
    tab.url = url;
    tab.internalPage = page;
    tab.title = internalPageTitle(page);
    tab.favicon = QUrl();
    tab.state = QVariantMap();
    tab.state.insert("url", tab.url);
    tab.state.insert("title", tab.title);
    emit dataChanged(index(row), index(row),
                     {TitleRole, UrlRole, FaviconRole, LoadingRole, ProgressRole, AudibleRole, MutedRole, EngineViewRole, InternalPageRole,
                      DiscardedRole});
    emit operationOccurred();
    return true;
}

bool TabModel::setInternalPageUrl(int row, const QUrl &url) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    Tab &tab = *m_tabs.at(row);
    if (tab.internalPage.isEmpty() || internalPageForUrl(url) != tab.internalPage || tab.url == url) {
        return false;
    }
    tab.url = url;
    tab.state.insert("url", tab.url);
    emit dataChanged(index(row), index(row), {UrlRole});
    emit operationOccurred();
    return true;
}

bool TabModel::setDefaultBackend(const QString &backendId) {
    const std::optional<engine::Backend> backend = m_registry->backendForId(backendId);
    if (!backend) {
        return false;
    }
    m_defaultBackend = *backend;
    return true;
}

bool TabModel::transferTabTo(int row, TabModel *destination, int destinationRow) {
    if (!destination || row < 0 || row >= rowCount() || m_privateMode != destination->m_privateMode ||
        !destination->m_registry->contains(m_tabs.at(row)->backend)) {
        return false;
    }
    const bool transferredPinned = m_tabs.at(row)->pinned;
    if (destination == this) {
        int target = std::clamp(destinationRow > row ? destinationRow - 1 : destinationRow, 0, rowCount() - 1);
        target =
            transferredPinned ? std::clamp(target, 0, std::max(0, pinnedCount() - 1)) : std::clamp(target, pinnedCount(), rowCount() - 1);
        return target == row || moveTab(row, target);
    }
    destinationRow = transferredPinned ? std::clamp(destinationRow, 0, destination->pinnedCount())
                                       : std::clamp(destinationRow, destination->pinnedCount(), destination->rowCount());

    destination->m_tabs.reserve(destination->m_tabs.size() + 1);
    engine::EngineView *view = m_tabs.at(row)->view.get();
    if (view) {
        disconnect(view, nullptr, this, nullptr);
    }

    beginRemoveRows({}, row, row);
    std::unique_ptr<Tab> transferred = std::move(m_tabs.at(row));
    m_tabs.erase(m_tabs.begin() + row);
    endRemoveRows();
    emit tabTransferredOut(view);

    destination->connectTab(*transferred);
    destination->beginInsertRows({}, destinationRow, destinationRow);
    destination->m_tabs.insert(destination->m_tabs.begin() + destinationRow, std::move(transferred));
    destination->endInsertRows();

    emit countChanged();
    emit destination->countChanged();
    if (transferredPinned) {
        emit pinnedCountChanged();
        emit destination->pinnedCountChanged();
    }
    emit operationOccurred();
    emit destination->operationOccurred();
    emit destination->tabAdded(destinationRow, false);
    if (view) {
        emit destination->tabEngineCreated(destinationRow, view);
    }
    if (m_tabs.empty()) {
        emit tabCloseRequestedForWindow();
    }
    return true;
}

int TabModel::restoreTab(const QUrl &url, const QString &title, bool pinned, const QString &backendId) {
    std::unique_ptr<Tab> tab = std::make_unique<Tab>();
    tab->id = nextTabId();
    tab->privateMode = m_privateMode;
    tab->backend = resolvedBackend(backendId);
    tab->url = url.isEmpty() ? QUrl("about:blank") : url;
    tab->title = title;
    tab->pinned = pinned;
    tab->state.insert("url", tab->url);
    tab->state.insert("title", tab->title);
    tab->internalPage = internalPageForUrl(tab->url);
    const int row = pinned ? pinnedCount() : rowCount();
    beginInsertRows({}, row, row);
    m_tabs.insert(m_tabs.begin() + row, std::move(tab));
    endInsertRows();
    emit countChanged();
    if (pinned) {
        emit pinnedCountChanged();
    }
    emit tabAdded(row, true);
    return row;
}

void TabModel::restoreTabs(const QJsonArray &tabs) {
    for (const QJsonValue &value : tabs) {
        const QJsonObject tab = value.toObject();
        restoreTab(urlWithoutCredentials(QUrl(tab.value("url").toString())), tab.value("title").toString(), tab.value("pinned").toBool(),
                   tab.value("backend").toString());
    }
    if (!tabs.isEmpty()) {
        emit operationOccurred();
    }
}

QJsonArray TabModel::sessionTabs() const {
    QJsonArray tabs;
    for (int row = 0; row < rowCount(); ++row) {
        QJsonObject tab;
        tab.insert("url", data(index(row), UrlRole).toUrl().toString());
        tab.insert("title", data(index(row), TitleRole).toString());
        tab.insert("pinned", data(index(row), PinnedRole).toBool());
        tab.insert("backend", backendIdAt(row));
        tabs.append(tab);
    }
    return tabs;
}

int TabModel::pinnedCount() const {
    return static_cast<int>(std::count_if(m_tabs.cbegin(), m_tabs.cend(), [](const std::unique_ptr<Tab> &tab) { return tab->pinned; }));
}

bool TabModel::canUndoClose() const {
    return static_cast<bool>(m_closedTab);
}

engine::EngineView *TabModel::engineViewAt(int row) const {
    if (row < 0 || row >= rowCount()) {
        return nullptr;
    }
    return m_tabs.at(row)->view.get();
}

engine::EngineProfile *TabModel::profileAt(int row) const {
    if (row < 0 || row >= rowCount()) {
        return nullptr;
    }
    return m_tabs.at(row)->profileLease.get();
}

QString TabModel::backendIdAt(int row) const {
    if (row < 0 || row >= rowCount()) {
        return {};
    }
    return m_registry->idForBackend(m_tabs.at(row)->backend);
}

QString TabModel::defaultBackendId() const {
    return m_registry->idForBackend(m_defaultBackend);
}

quint64 TabModel::tabIdAt(int row) const {
    if (row < 0 || row >= rowCount()) {
        return 0;
    }
    return m_tabs.at(row)->id;
}

bool TabModel::isDiscarded(int row) const {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    const Tab &tab = *m_tabs.at(row);
    return !tab.view && tab.internalPage.isEmpty();
}

qint64 TabModel::rendererProcessIdAt(int row) const {
    engine::EngineView *view = engineViewAt(row);
    return view ? view->rendererProcessId() : 0;
}

int TabModel::rendererProcessUseCount(qint64 processId) const {
    if (processId <= 0) {
        return 0;
    }
    return static_cast<int>(std::count_if(m_tabs.cbegin(), m_tabs.cend(), [processId](const std::unique_ptr<Tab> &tab) {
        return tab->view && tab->view->rendererProcessId() == processId;
    }));
}

int TabModel::indexForTabId(quint64 tabId) const {
    for (int row = 0; row < rowCount(); ++row) {
        if (m_tabs.at(row)->id == tabId) {
            return row;
        }
    }
    return -1;
}

void TabModel::connectTab(Tab &tab) {
    engine::EngineView *view = tab.view.get();
    if (!view) {
        return;
    }
    connect(view, &engine::EngineView::titleChanged, this, [this, view] { notifyViewChanged(view, {TitleRole}); });
    connect(view, &engine::EngineView::urlChanged, this, [this, view] { notifyViewChanged(view, {UrlRole, TitleRole}); });
    connect(view, &engine::EngineView::faviconUrlChanged, this, [this, view] { notifyViewChanged(view, {FaviconRole}); });
    connect(view, &engine::EngineView::loadingChanged, this, [this, view] { notifyViewChanged(view, {LoadingRole}); });
    connect(view, &engine::EngineView::loadProgressChanged, this, [this, view] { notifyViewChanged(view, {ProgressRole}); });
    connect(view, &engine::EngineView::audibleChanged, this, [this, view] { notifyViewChanged(view, {AudibleRole}); });
    connect(view, &engine::EngineView::mutedChanged, this, [this, view] { notifyViewChanged(view, {MutedRole}); });
    connect(view, &engine::EngineView::newViewRequested, this, [this, view](engine::EngineNewViewRequest *request) {
        const int row = indexOf(view);
        if (row >= 0) {
            emit externalViewRequested(request, row, backendIdAt(row));
        }
    });
}

bool TabModel::discardTab(int row) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    Tab &tab = *m_tabs.at(row);
    if (!tab.view) {
        return true;
    }
    updateCachedState(tab);
    tab.view.reset();
    tab.profileLease.reset();
    emit dataChanged(index(row), index(row),
                     {TitleRole, UrlRole, FaviconRole, LoadingRole, ProgressRole, AudibleRole, MutedRole, EngineViewRole, DiscardedRole});
    return true;
}

bool TabModel::createView(int row) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    Tab &tab = *m_tabs.at(row);
    if (tab.view || !tab.internalPage.isEmpty()) {
        return true;
    }
    tab.profileLease = m_profileResolver ? m_profileResolver(tab.backend) : nullptr;
    tab.view = m_factory ? m_factory(tab.backend, tab.profileLease.get()) : nullptr;
    if (!tab.view) {
        tab.profileLease.reset();
        return false;
    }
    connectTab(tab);
    emit tabEngineCreated(row, tab.view.get());
    const QUrl storedUrl = tab.state.value("url").toUrl();
    if (storedUrl.scheme() == "eden") {
        const QUrl mapped = tab.view->internalUrlFor(storedUrl);
        if (!mapped.isEmpty()) {
            tab.state.insert("url", mapped);
        }
    }
    tab.view->restoreState(tab.state);
    emit dataChanged(index(row), index(row),
                     {TitleRole, UrlRole, FaviconRole, LoadingRole, ProgressRole, AudibleRole, MutedRole, EngineViewRole, DiscardedRole});
    return true;
}

engine::Backend TabModel::resolvedBackend(const QString &backendId) const {
    if (backendId.isEmpty()) {
        return m_defaultBackend;
    }
    return m_registry->backendForId(backendId).value_or(m_defaultBackend);
}

void TabModel::updateCachedState(Tab &tab) {
    if (!tab.view) {
        return;
    }
    const QUrl currentUrl = tab.view->url();
    if (!currentUrl.isEmpty()) {
        tab.url = currentUrl;
    }
    if (!tab.view->title().isEmpty()) {
        tab.title = tab.view->title();
    }
    if (!tab.view->faviconUrl().isEmpty()) {
        tab.favicon = tab.view->faviconUrl();
    }
    tab.state = tab.view->serializeState();
    tab.state.insert("url", tab.url);
    tab.state.insert("title", tab.title);
    tab.state.insert("faviconUrl", tab.favicon);
}

int TabModel::indexOf(const engine::EngineView *view) const {
    for (int row = 0; row < rowCount(); ++row) {
        if (m_tabs.at(row)->view.get() == view) {
            return row;
        }
    }
    return -1;
}

void TabModel::notifyViewChanged(engine::EngineView *view, const QList<int> &roles) {
    const int row = indexOf(view);
    if (row < 0) {
        return;
    }
    Tab &tab = *m_tabs.at(row);
    if (roles.contains(UrlRole) && !view->url().isEmpty()) {
        tab.url = view->url();
        tab.state.insert("url", tab.url);
    }
    if (roles.contains(TitleRole) && !view->title().isEmpty()) {
        tab.title = view->title();
        tab.state.insert("title", tab.title);
    }
    if (roles.contains(FaviconRole) && !view->faviconUrl().isEmpty()) {
        tab.favicon = view->faviconUrl();
        tab.state.insert("faviconUrl", tab.favicon);
    }
    if (roles.contains(MutedRole)) {
        tab.state.insert("muted", view->isMuted());
    }
    emit dataChanged(index(row), index(row), roles);
    emit operationOccurred();
}

}
