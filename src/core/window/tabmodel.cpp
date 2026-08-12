#include "core/window/tabmodel.h"
#include "core/urlsanitizer.h"
#include "engine/engineprofile.h"
#include "engine/engineview.h"

#include <algorithm>

namespace eden::core {

TabModel::TabModel(ViewFactory factory, bool privateMode, QObject *parent, std::shared_ptr<engine::EngineProfile> profileLease)
    : QAbstractListModel(parent),
      m_factory(std::move(factory)),
      m_profileLease(std::move(profileLease)),
      m_privateMode(privateMode) {
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
        if (!view) {
            return tab.title;
        }
        return view->title().isEmpty() ? view->url().host() : view->title();
    }
    if (role == UrlRole) {
        return urlWithoutCredentials(view ? view->url() : tab.url);
    }
    if (role == FaviconRole) {
        return view ? view->faviconUrl() : QUrl();
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
        return view && view->isMuted();
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
            {InternalPageRole, "internalPage"}};
}

int TabModel::addTab(const QUrl &url, bool background) {
    std::unique_ptr<Tab> tab = std::make_unique<Tab>();
    tab->privateMode = m_privateMode;
    tab->profileLease = m_profileLease;
    const QUrl destination = url.isEmpty() ? QUrl("about:blank") : url;
    if (destination.scheme() == "eden") {
        tab->url = destination;
        tab->internalPage = destination.host();
        if (tab->internalPage == "settings") {
            tab->title = "Settings";
        } else if (tab->internalPage == "theme-editor") {
            tab->title = "Theme Editor";
        } else {
            tab->title = "Eden";
        }
    } else {
        tab->view = m_factory();
        connectTab(*tab);
    }
    const int row = rowCount();
    beginInsertRows({}, row, row);
    m_tabs.push_back(std::move(tab));
    endInsertRows();
    if (m_tabs.back()->view) {
        m_tabs.back()->view->load(destination);
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
    beginRemoveRows({}, row, row);
    std::unique_ptr<Tab> removed = std::move(m_tabs.at(row));
    m_tabs.erase(m_tabs.begin() + row);
    endRemoveRows();
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
    const int added = addTab(data(index(row), UrlRole).toUrl(), false);
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

bool TabModel::transferTabTo(int row, TabModel *destination, int destinationRow) {
    if (!destination || row < 0 || row >= rowCount() || m_privateMode != destination->m_privateMode) {
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
    if (m_tabs.empty()) {
        emit tabCloseRequestedForWindow();
    }
    return true;
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
    return m_tabs.at(row)->profileLease ? m_tabs.at(row)->profileLease.get() : nullptr;
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
    connect(view, &engine::EngineView::newViewRequested, this,
            [this](engine::EngineNewViewRequest *request) { emit externalViewRequested(request); });
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
    if (row >= 0) {
        emit dataChanged(index(row), index(row), roles);
        emit operationOccurred();
    }
}

}
