#include "engine/engineview.h"

namespace eden::engine {

    QUrl autofillOrigin(const QUrl &url) {
        if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http")) {
            return {};
        }
        QUrl origin = url;
        origin.setUserInfo({});
        origin.setPath({});
        origin.setQuery({});
        origin.setFragment({});
        if (origin.port() == (origin.scheme() == "https" ? 443 : 80)) {
            origin.setPort(-1);
        }
        return origin;
    }

    bool AutofillTarget::isValid() const {
        return !documentId.isEmpty() && !origin.isEmpty() && origin == autofillOrigin(origin) &&
               origin == autofillOrigin(url);
    }

    EngineView::EngineView(QObject *parent)
        : QObject(parent) {
        connect(this, &EngineView::captureChanged, this, &EngineView::activityIndicatorsChanged);
        connect(this, &EngineView::audibleChanged, this, &EngineView::activityIndicatorsChanged);
        connect(this, &EngineView::mutedChanged, this, &EngineView::activityIndicatorsChanged);
    }

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

    QVariantMap EngineView::certificateDetails() const {
        return {};
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

    void EngineView::dismissPermissionRequest(quint64 id) {
        resolvePermissionRequest(id, false);
    }

    void EngineView::resolveDisplayCaptureRequest(quint64, const QString &) {}

    void EngineView::resolveFileDialog(quint64, bool, const QList<QUrl> &) {}

    void EngineView::requestAutofillTarget(AutofillTargetCallback callback) {
        if (callback) {
            callback({});
        }
    }

    void EngineView::fillCredential(const AutofillTarget &, const QString &, const QString &) {}

    void EngineView::fillForm(const AutofillTarget &, const QVariantMap &) {}

    bool EngineView::isCapturing() const {
        return m_capturingVideo || m_capturingAudio || !m_captureDetails.isEmpty();
    }

    QVariantList EngineView::activityIndicators() const {
        int activities = 0;
        for (int value : m_captureDetails) {
            activities |= value;
        }
        QVariantList indicators;
        const auto append = [&indicators](const QString &icon, const QString &description, bool capture) {
            indicators.append(QVariantMap{{"icon", icon}, {"description", description}, {"capture", capture}});
        };
        if (activities & 1) {
            append("camera", "Camera in use", true);
        }
        if (activities & 2) {
            append("microphone", "Microphone in use", true);
        }
        if (activities & 4) {
            append("monitor", "Sharing your screen or an application", true);
        }
        if (activities & 8) {
            append("volume", "Sharing audio", true);
        }
        if (m_capturingVideo && !(activities & 5)) {
            append("camera", "Camera or screen capture in use", true);
        }
        if (m_capturingAudio && !(activities & 10)) {
            append("microphone", "Microphone or shared audio in use", true);
        }
        if (isMuted()) {
            append("muted", "Tab audio muted", false);
        } else if (isAudible()) {
            append("volume", "Playing audio", false);
        }
        QStringList descriptions;
        for (const QVariant &indicator : indicators) {
            descriptions.append(indicator.toMap().value("description").toString());
        }
        for (QVariant &indicator : indicators) {
            QVariantMap value = indicator.toMap();
            value.insert("summary", descriptions.join(QStringLiteral(". ")));
            indicator = value;
        }
        return indicators;
    }

    QString EngineView::captureDescription() const {
        QStringList descriptions;
        for (const QVariant &indicator : activityIndicators()) {
            const QVariantMap item = indicator.toMap();
            if (item.value("capture").toBool()) {
                descriptions.append(item.value("description").toString());
            }
        }
        return descriptions.join(QStringLiteral(". "));
    }

    void EngineView::setMediaCapture(bool video, bool audio) {
        if (m_capturingVideo == video && m_capturingAudio == audio) {
            return;
        }
        m_capturingVideo = video;
        m_capturingAudio = audio;
        emit captureChanged();
    }

    void EngineView::setCaptureDetails(const QString &document, int activities) {
        activities &= 15;
        if (document.isEmpty() || m_captureDetails.value(document) == activities) {
            return;
        }
        if (activities) {
            m_captureDetails.insert(document, activities);
        } else {
            m_captureDetails.remove(document);
        }
        emit captureChanged();
    }

    void EngineView::clearCaptureDetails() {
        if (!m_captureDetails.isEmpty()) {
            m_captureDetails.clear();
            emit captureChanged();
        }
    }

    void EngineView::releaseFocus() {}

    void EngineView::exitFullscreen() {}

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
