#pragma once

#include "engine/engineview.h"
#include "engine/wpe/wpeapi.h"
#include <QPointer>

namespace eden::engine {

    class WpeProfile;
    class WpeSurface;

    class WpeEngineView final : public EngineView {
        Q_OBJECT

      public:
        explicit WpeEngineView(WpeProfile *profile, QObject *parent = nullptr);
        ~WpeEngineView() override;
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
        QVariantMap certificateDetails() const override;
        QString backendName() const override;
        Capabilities capabilities() const override;
        qint64 rendererProcessId() const override;
        void load(const QUrl &url) override;
        void back() override;
        void forward() override;
        QVariantList navigationHistory(int direction, int maximumItems = 20) const override;
        void goToHistoryOffset(int offset) override;
        void reload() override;
        void reloadBypassingCache() override;
        void stop() override;
        void openDevTools() override;
        bool devToolsSelfFramed() const override;
        void closeDevTools() override;
        void attachDevTools(QQuickItem *viewport) override;
        void detachDevTools(QQuickItem *viewport) override;
        void findInPage(const QString &text, FindFlags flags = {}) override;
        void attach(QQuickItem *viewport) override;
        void releaseFocus() override;
        void exitFullscreen() override;
        void setMuted(bool muted) override;
        void executeContextMenuCommand(const QString &command) override;
        void dismissContextMenu() override;
        void resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text) override;
        void resolvePermissionRequest(quint64 id, bool allowed) override;
        void dismissPermissionRequest(quint64 id) override;
        void resolveDisplayCaptureRequest(quint64 id, const QString &source) override;
        void resolveFileDialog(quint64 id, bool accepted, const QList<QUrl> &files) override;
        void requestAutofillTarget(AutofillTargetCallback callback) override;
        void fillCredential(const AutofillTarget &target, const QString &username, const QString &password) override;
        void fillForm(const AutofillTarget &target, const QVariantMap &fields) override;
        void requestThumbnail(const QSize &size, ThumbnailCallback callback) override;
        QVariantMap serializeState() const override;
        void restoreState(const QVariantMap &state) override;
        QUrl internalUrlFor(const QUrl &url) const override;
        bool containsPageScenePoint(const QPointF &position) const override;
#if EDEN_ENABLE_AUTOMATION
        bool automationWheel(int delta) override;
#endif
        WebKitWebView *nativeView() const;
        WpeSurface *surface() const;
        bool adoptPopup(WebKitWebView *source, WpeProfile *profile);
        void
        evaluate(const QString &script, std::function<void(QVariant)> callback = {}, const char *world = "eden.forms");

      protected:
        void applyZoomFactor(qreal factor) override;

      private:
        void createNative(WebKitWebView *related = nullptr);
        void destroyNative();
        void synchronizeState();
        void navigationChanged(WebKitLoadEvent event);
        void invalidateFormTarget();
        void cancelRequests();
        void installFormHooks();
        void handleFormMessage(JSCValue *message);
        void showError(const QUrl &url, const QString &message);
        QVariantList menuActions(WebKitContextMenu *menu);
        void showContextMenu(WebKitContextMenu *menu, WebKitHitTestResult *hit, bool inspector);
        void showOptionMenu(WebKitOptionMenu *menu, const QPoint &position, bool inspector);
        void applyInspectorTheme();
        void updateFavicon(GBytes *bytes);
        bool permissionRequest(WebKitPermissionRequest *request);
        QPointer<WpeProfile> m_profile;
        WebKitWebView *m_view = nullptr;
        WebKitUserContentManager *m_content = nullptr;
        GCancellable *m_cancellable = nullptr;
        QPointer<WpeSurface> m_surface;
        QPointer<WpeSurface> m_inspector;
        QPointer<QQuickItem> m_inspectorViewport;
        QHash<quint64, WebKitScriptDialog *> m_dialogs;
        QHash<quint64, WebKitFileChooserRequest *> m_files;
        QHash<quint64, WebKitPermissionRequest *> m_permissions;
        QHash<quint64, WebKitPermissionRequest *> m_displayRequests;
        QHash<QString, WebKitContextMenuItem *> m_menuActions;
        WebKitOptionMenu *m_optionMenu = nullptr;
        QUrl m_url;
        QString m_title;
        QUrl m_favicon;
        int m_faviconResolution = 0;
        bool m_inspectorMenu = false;
        bool m_inspectorSelfFramed = false;
        QMetaObject::Connection m_inspectorThemeConnection;
        QString m_formToken;
        QString m_documentId;
        QUrl m_formOrigin;
        quint64 m_generation = 0;
        quint64 m_nextRequest = 1;
        int m_progress = 0;
        bool m_loading = false;
        bool m_back = false;
        bool m_forward = false;
        bool m_audible = false;
        bool m_muted = false;
        bool m_insecureContent = false;
        bool m_errorPage = false;
        bool m_committed = false;
    };

}
