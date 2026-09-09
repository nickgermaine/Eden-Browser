#pragma once

#include "engine/enginenewviewrequest.h"

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class QQuickItem;

namespace eden::engine {

    class ContextMenuInfo {
        Q_GADGET
        Q_PROPERTY(QPoint position MEMBER position)
        Q_PROPERTY(QUrl linkUrl MEMBER linkUrl)
        Q_PROPERTY(QUrl mediaUrl MEMBER mediaUrl)
        Q_PROPERTY(QString selectedText MEMBER selectedText)
        Q_PROPERTY(bool editable MEMBER editable)
        Q_PROPERTY(QVariantList actions MEMBER actions)
        Q_PROPERTY(QString surface MEMBER surface)

      public:
        QPoint position;
        QUrl linkUrl;
        QUrl mediaUrl;
        QString selectedText;
        bool editable = false;
        QVariantList actions;
        QString surface = "page";
    };

    class JavaScriptDialogInfo {
        Q_GADGET
        Q_PROPERTY(quint64 id MEMBER id)
        Q_PROPERTY(QUrl origin MEMBER origin)
        Q_PROPERTY(QString kind MEMBER kind)
        Q_PROPERTY(QString message MEMBER message)
        Q_PROPERTY(QString defaultText MEMBER defaultText)

      public:
        quint64 id = 0;
        QUrl origin;
        QString kind;
        QString message;
        QString defaultText;
    };

    class PermissionRequestInfo {
        Q_GADGET
        Q_PROPERTY(quint64 id MEMBER id)
        Q_PROPERTY(QUrl origin MEMBER origin)
        Q_PROPERTY(QStringList permissions MEMBER permissions)

      public:
        quint64 id = 0;
        QUrl origin;
        QStringList permissions;
    };

    class DisplayCaptureRequestInfo {
        Q_GADGET
        Q_PROPERTY(quint64 id MEMBER id)
        Q_PROPERTY(QUrl origin MEMBER origin)
        Q_PROPERTY(bool audioRequested MEMBER audioRequested)

      public:
        quint64 id = 0;
        QUrl origin;
        bool audioRequested = false;
    };

    class FileDialogInfo {
        Q_GADGET
        Q_PROPERTY(quint64 id MEMBER id)
        Q_PROPERTY(QString title MEMBER title)
        Q_PROPERTY(QString mode MEMBER mode)
        Q_PROPERTY(QString defaultPath MEMBER defaultPath)
        Q_PROPERTY(QStringList nameFilters MEMBER nameFilters)

      public:
        quint64 id = 0;
        QString title;
        QString mode;
        QString defaultPath;
        QStringList nameFilters;
    };

    class CredentialSubmissionInfo {
        Q_GADGET
        Q_PROPERTY(QUrl origin MEMBER origin)
        Q_PROPERTY(QString username MEMBER username)
        Q_PROPERTY(QString password MEMBER password)

      public:
        QUrl origin;
        QString username;
        QString password;
    };

    class EngineView : public QObject {
        Q_OBJECT
        Q_PROPERTY(QUrl url READ url NOTIFY urlChanged)
        Q_PROPERTY(QString title READ title NOTIFY titleChanged)
        Q_PROPERTY(QUrl faviconUrl READ faviconUrl NOTIFY faviconUrlChanged)
        Q_PROPERTY(int loadProgress READ loadProgress NOTIFY loadProgressChanged)
        Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
        Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY canGoBackChanged)
        Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY canGoForwardChanged)
        Q_PROPERTY(bool audible READ isAudible NOTIFY audibleChanged)
        Q_PROPERTY(bool muted READ isMuted WRITE setMuted NOTIFY mutedChanged)
        Q_PROPERTY(QString securityState READ securityState NOTIFY securityStateChanged)
        Q_PROPERTY(QVariantMap certificateDetails READ certificateDetails NOTIFY certificateDetailsChanged)
        Q_PROPERTY(QString backendName READ backendName CONSTANT)
        Q_PROPERTY(Capabilities capabilities READ capabilities CONSTANT)
        Q_PROPERTY(bool devToolsOpen READ devToolsOpen NOTIFY devToolsOpenChanged)
        Q_PROPERTY(
            DevToolsPlacement devToolsPlacement READ devToolsPlacement WRITE setDevToolsPlacement NOTIFY
                devToolsPlacementChanged
        )

      public:
        using Disposition = EngineNewViewRequest::Disposition;

        enum FindFlag { FindBackward = 0x1, FindCaseSensitive = 0x2 };
        Q_DECLARE_FLAGS(FindFlags, FindFlag)
        Q_FLAG(FindFlags)

        enum Capability {
            DockedDevtools = 0x1,
            HistorySerialization = 0x2,
            ThumbnailCapture = 0x4,
            OsrCompositing = 0x8,
            PerTabMute = 0x10
        };
        Q_DECLARE_FLAGS(Capabilities, Capability)
        Q_FLAG(Capabilities)

        enum DevToolsPlacement { DevToolsRight, DevToolsBottom, DevToolsSeparate };
        Q_ENUM(DevToolsPlacement)

        using ThumbnailCallback = std::function<void(const QImage &image)>;

        explicit EngineView(QObject *parent = nullptr);
        ~EngineView() override;

        virtual QUrl url() const = 0;
        virtual QString title() const = 0;
        virtual QUrl faviconUrl() const = 0;
        virtual int loadProgress() const = 0;
        virtual bool isLoading() const = 0;
        virtual bool canGoBack() const = 0;
        virtual bool canGoForward() const = 0;
        virtual bool isAudible() const = 0;
        virtual bool isMuted() const = 0;
        virtual QString securityState() const = 0;
        virtual QVariantMap certificateDetails() const;
        virtual QString backendName() const = 0;
        virtual Capabilities capabilities() const = 0;
        virtual qint64 rendererProcessId() const;
        bool devToolsOpen() const;
        DevToolsPlacement devToolsPlacement() const;

        Q_INVOKABLE virtual void load(const QUrl &url) = 0;
        Q_INVOKABLE virtual void back() = 0;
        Q_INVOKABLE virtual void forward() = 0;
        Q_INVOKABLE virtual QVariantList navigationHistory(int direction, int maximumItems = 20) const = 0;
        Q_INVOKABLE virtual void goToHistoryOffset(int offset) = 0;
        Q_INVOKABLE virtual void reload() = 0;
        Q_INVOKABLE virtual void stop() = 0;
        Q_INVOKABLE virtual void openDevTools() = 0;
        Q_INVOKABLE virtual void closeDevTools();
        Q_INVOKABLE virtual void attachDevTools(QQuickItem *viewport);
        Q_INVOKABLE virtual void detachDevTools(QQuickItem *viewport);
        Q_INVOKABLE void toggleDevToolsOrientation();
        Q_INVOKABLE void toggleDevToolsSeparate();
        Q_INVOKABLE virtual void findInPage(const QString &text, FindFlags flags = {}) = 0;
        Q_INVOKABLE virtual void attach(QQuickItem *viewport) = 0;
        Q_INVOKABLE virtual void releaseFocus();
        Q_INVOKABLE virtual void setMuted(bool muted) = 0;
        Q_INVOKABLE virtual void executeContextMenuCommand(const QString &command);
        Q_INVOKABLE virtual void dismissContextMenu();
        Q_INVOKABLE virtual void resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text);
        Q_INVOKABLE virtual void resolvePermissionRequest(quint64 id, bool allowed);
        Q_INVOKABLE virtual void resolveDisplayCaptureRequest(quint64 id, const QString &source);
        Q_INVOKABLE virtual void resolveFileDialog(quint64 id, bool accepted, const QList<QUrl> &files);
        Q_INVOKABLE virtual void fillCredential(const QString &username, const QString &password);
        Q_INVOKABLE virtual void fillForm(const QVariantMap &fields);
        void setDevToolsPlacement(DevToolsPlacement placement);
        virtual QUrl internalUrlFor(const QUrl &url) const;
        virtual void requestThumbnail(const QSize &size, ThumbnailCallback callback);
        virtual QVariantMap serializeState() const;
        virtual void restoreState(const QVariantMap &state);
        virtual bool containsPageScenePoint(const QPointF &windowScenePoint) const;
#if EDEN_ENABLE_AUTOMATION
        virtual bool automationWheel(int delta);
#endif

      signals:
        void urlChanged();
        void titleChanged();
        void faviconUrlChanged();
        void loadProgressChanged();
        void loadingChanged();
        void canGoBackChanged();
        void canGoForwardChanged();
        void audibleChanged();
        void mutedChanged();
        void securityStateChanged();
        void certificateDetailsChanged();
        void newViewRequested(EngineNewViewRequest *request);
        void contextMenuRequested(const ContextMenuInfo &info);
        void javaScriptDialogRequested(const JavaScriptDialogInfo &info);
        void javaScriptDialogClosed(quint64 id);
        void permissionRequested(const PermissionRequestInfo &info);
        void permissionRequestClosed(quint64 id);
        void displayCaptureRequested(const DisplayCaptureRequestInfo &info);
        void displayCaptureRequestClosed(quint64 id);
        void fileDialogRequested(const FileDialogInfo &info);
        void fileDialogClosed(quint64 id);
        void credentialSubmitted(const CredentialSubmissionInfo &info);
        void formFieldFocused(const QVariantMap &field);
        void fullscreenRequested(bool fullscreen);
        void shortcutRequested(const QString &command);
        void focusTraversalRequested(bool next);
        void devToolsOpenChanged();
        void devToolsPlacementChanged();

      protected:
        void setDevToolsOpen(bool open);

      private:
        bool m_devToolsOpen = false;
        DevToolsPlacement m_devToolsPlacement = DevToolsRight;
        DevToolsPlacement m_lastDockedPlacement = DevToolsRight;
    };

}

Q_DECLARE_OPERATORS_FOR_FLAGS(eden::engine::EngineView::FindFlags)
Q_DECLARE_OPERATORS_FOR_FLAGS(eden::engine::EngineView::Capabilities)
Q_DECLARE_METATYPE(eden::engine::ContextMenuInfo)
Q_DECLARE_METATYPE(eden::engine::JavaScriptDialogInfo)
Q_DECLARE_METATYPE(eden::engine::PermissionRequestInfo)
Q_DECLARE_METATYPE(eden::engine::DisplayCaptureRequestInfo)
Q_DECLARE_METATYPE(eden::engine::FileDialogInfo)
Q_DECLARE_METATYPE(eden::engine::CredentialSubmissionInfo)
Q_DECLARE_METATYPE(eden::engine::EngineView::Capabilities)
