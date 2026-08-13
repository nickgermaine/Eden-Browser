#pragma once

#include "engine/engineview.h"

#include <memory>

namespace eden::engine {
class EngineProfile;
}

namespace eden::engine::cef {

class CefEngineClient;
class CefDevToolsClient;
class CefFileChooserObserver;
class CefDevToolsProtocolSession;
class CefPopupNewViewRequest;
struct CefPopupTransfer;

class CefEngineView final : public EngineView {
    Q_OBJECT

  public:
    explicit CefEngineView(EngineProfile *profile, QObject *parent = nullptr);
    ~CefEngineView() override;

    QUrl url() const override;
    QString title() const override;
    QUrl faviconUrl() const override;
    int loadProgress() const override;
    bool isLoading() const override;
    bool canGoBack() const override;
    bool canGoForward() const override;
    bool isAudible() const override;
    bool isMuted() const override;
    QString securityState() const override;
    QString backendName() const override;
    Capabilities capabilities() const override;
    qint64 rendererProcessId() const override;
    bool containsPageScenePoint(const QPointF &windowScenePoint) const override;
#if EDEN_ENABLE_AUTOMATION
    bool automationWheel(int delta) override;
#endif

    QUrl internalUrlFor(const QUrl &url) const override;
    void load(const QUrl &url) override;
    void back() override;
    void forward() override;
    QVariantList navigationHistory(int direction, int maximumItems = 20) const override;
    void goToHistoryOffset(int offset) override;
    void reload() override;
    void stop() override;
    void openDevTools() override;
    void closeDevTools() override;
    void attachDevTools(QQuickItem *viewport) override;
    void detachDevTools(QQuickItem *viewport) override;
    void findInPage(const QString &text, FindFlags flags) override;
    void attach(QQuickItem *viewport) override;
    void releaseFocus() override;
    void setMuted(bool muted) override;
    void executeContextMenuCommand(const QString &command) override;
    void dismissContextMenu() override;
    void resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text) override;
    void resolvePermissionRequest(quint64 id, bool allowed) override;
    void resolveFileDialog(quint64 id, bool accepted, const QList<QUrl> &files) override;
    void requestThumbnail(const QSize &size, ThumbnailCallback callback) override;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    friend class CefEngineClient;
    friend class CefDevToolsClient;
    friend class CefFileChooserObserver;
    friend class CefDevToolsProtocolSession;
    friend class CefPopupNewViewRequest;

    bool adoptPopup(const std::shared_ptr<CefPopupTransfer> &transfer);

    class Private;
    std::unique_ptr<Private> d;
};

}
