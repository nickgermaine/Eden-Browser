#pragma once

#include "engine/engineview.h"

#include <QHash>
#include <QPointer>

class QQmlComponent;
class QQmlEngine;
class QWebEngineNewWindowRequest;

namespace eden::engine {

    class EngineProfile;
    class QtWebEngineNewViewRequest;

    class QtWebEngineView final : public EngineView {
        Q_OBJECT

      public:
        explicit QtWebEngineView(EngineProfile *profile, QObject *parent = nullptr);
        ~QtWebEngineView() override;

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
        void exitFullscreen() override;
        void setMuted(bool muted) override;
        void executeContextMenuCommand(const QString &command) override;
        void requestThumbnail(const QSize &size, ThumbnailCallback callback) override;
        void requestAutofillTarget(AutofillTargetCallback callback) override;
        void fillCredential(const AutofillTarget &target, const QString &username, const QString &password) override;
        void fillForm(const AutofillTarget &target, const QVariantMap &fields) override;
        void resolvePermissionRequest(quint64 id, bool allowed) override;
        void dismissPermissionRequest(quint64 id) override;

        Q_INVOKABLE void handleMediaActivity(const QString &message);
        Q_INVOKABLE void clearMediaActivity();
        Q_INVOKABLE void handleNewWindow(QObject *requestObject);
        Q_INVOKABLE void handleFullScreen(bool fullscreen);
        Q_INVOKABLE void handleContextMenu(
            const QPoint &position,
            const QUrl &linkUrl,
            const QUrl &mediaUrl,
            const QString &selectedText,
            bool editable
        );
        Q_INVOKABLE void handleCertificateError();
        Q_INVOKABLE void handlePermission(const QVariant &permissionValue);
        Q_INVOKABLE void handleAutofillTarget(const QString &requestId, const QVariant &result);

      private slots:
        void syncState();

      private:
        friend class QtWebEngineNewViewRequest;
        friend class QtFormReportBridge;

        QUrl committedMainFrameUrl() const;
        QVariantMap registerFormDocument(const QString &origin, const QString &documentId);
        void handleFormReport(
            const QString &token,
            const QString &documentId,
            const QString &origin,
            const QString &kind,
            const QVariantList &values
        );

        bool ensureView(QQmlEngine *engine);
        bool acceptNewWindowRequest(QWebEngineNewWindowRequest *request, QQmlEngine *engine);
        void triggerWebAction(const QByteArray &actionName);
        void installFormHooks();
        void runJavaScript(const QString &script);
        QVariant value(const char *name) const;
        void invoke(const char *method);

        EngineProfile *m_profile;
        QPointer<QObject> m_view;
        QPointer<QObject> m_devToolsView;
        QPointer<QQuickItem> m_devToolsHost;
        bool m_pendingInspect = false;
        QUrl m_pendingUrl;
        QUrl m_url;
        QString m_title;
        QUrl m_faviconUrl;
        int m_loadProgress = 0;
        bool m_loading = false;
        bool m_canGoBack = false;
        bool m_canGoForward = false;
        bool m_audible = false;
        bool m_muted = false;
        bool m_certificateError = false;
        quint64 m_nextPermissionId = 1;
        QHash<quint64, QVariant> m_permissions;
        QHash<QString, AutofillTargetCallback> m_autofillRequests;
        QString m_mediaReportToken;
        QString m_formReportToken;
        QString m_formDocumentId;
        QUrl m_formOrigin;
    };

}
