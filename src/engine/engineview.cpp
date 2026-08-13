#include "engine/engineview.h"

namespace eden::engine {

EngineView::EngineView(QObject *parent)
    : QObject(parent) {}

EngineView::~EngineView() = default;

qint64 EngineView::rendererProcessId() const {
    return 0;
}

bool EngineView::devToolsOpen() const {
    return m_devToolsOpen;
}

EngineView::DevToolsPlacement EngineView::devToolsPlacement() const {
    return m_devToolsPlacement;
}

void EngineView::setDevToolsOpen(bool open) {
    if (m_devToolsOpen == open) {
        return;
    }
    m_devToolsOpen = open;
    emit devToolsOpenChanged();
}

void EngineView::setDevToolsPlacement(DevToolsPlacement placement) {
    if (m_devToolsPlacement == placement) {
        return;
    }
    m_devToolsPlacement = placement;
    emit devToolsPlacementChanged();
}

void EngineView::closeDevTools() {
    setDevToolsOpen(false);
}

void EngineView::attachDevTools(QQuickItem *) {}

void EngineView::detachDevTools(QQuickItem *) {}

void EngineView::toggleDevToolsOrientation() {
    if (m_devToolsPlacement == DevToolsSeparate) {
        return;
    }
    setDevToolsPlacement(m_devToolsPlacement == DevToolsRight ? DevToolsBottom : DevToolsRight);
}

void EngineView::toggleDevToolsSeparate() {
    if (m_devToolsPlacement == DevToolsSeparate) {
        setDevToolsPlacement(m_lastDockedPlacement);
        return;
    }
    m_lastDockedPlacement = m_devToolsPlacement;
    setDevToolsPlacement(DevToolsSeparate);
}

bool EngineView::containsPageScenePoint(const QPointF &) const {
    return false;
}

#if EDEN_ENABLE_AUTOMATION
bool EngineView::automationWheel(int) {
    return false;
}
#endif

void EngineView::executeContextMenuCommand(const QString &) {}

void EngineView::dismissContextMenu() {}

void EngineView::resolveJavaScriptDialog(quint64, bool, const QString &) {}

void EngineView::resolvePermissionRequest(quint64, bool) {}

void EngineView::resolveFileDialog(quint64, bool, const QList<QUrl> &) {}

void EngineView::releaseFocus() {}

QUrl EngineView::internalUrlFor(const QUrl &) const {
    return {};
}

void EngineView::requestThumbnail(const QSize &, ThumbnailCallback callback) {
    if (callback) {
        callback({});
    }
}

QVariantMap EngineView::serializeState() const {
    QVariantMap state;
    state.insert("url", url());
    state.insert("title", title());
    state.insert("faviconUrl", faviconUrl());
    state.insert("muted", isMuted());
    return state;
}

void EngineView::restoreState(const QVariantMap &state) {
    setMuted(state.value("muted").toBool());
    const QUrl restoredUrl = state.value("url").toUrl();
    load(restoredUrl.isEmpty() ? QUrl("about:blank") : restoredUrl);
}

}
