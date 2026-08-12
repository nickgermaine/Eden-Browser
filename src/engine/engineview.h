#pragma once

#include "engine/enginenewviewrequest.h"

#include <QObject>
#include <QPoint>
#include <QUrl>
#include <QVariantList>

class QQuickItem;

namespace eden::engine {

class ContextMenuInfo {
    Q_GADGET
    Q_PROPERTY(QPoint position MEMBER position)
    Q_PROPERTY(QUrl linkUrl MEMBER linkUrl)
    Q_PROPERTY(QString selectedText MEMBER selectedText)
    Q_PROPERTY(bool editable MEMBER editable)

  public:
    QPoint position;
    QUrl linkUrl;
    QString selectedText;
    bool editable = false;
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

  public:
    using Disposition = EngineNewViewRequest::Disposition;

    enum FindFlag { FindBackward = 0x1, FindCaseSensitive = 0x2 };
    Q_DECLARE_FLAGS(FindFlags, FindFlag)
    Q_FLAG(FindFlags)

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

    Q_INVOKABLE virtual void load(const QUrl &url) = 0;
    Q_INVOKABLE virtual void back() = 0;
    Q_INVOKABLE virtual void forward() = 0;
    Q_INVOKABLE virtual QVariantList navigationHistory(int direction, int maximumItems = 20) const = 0;
    Q_INVOKABLE virtual void goToHistoryOffset(int offset) = 0;
    Q_INVOKABLE virtual void reload() = 0;
    Q_INVOKABLE virtual void stop() = 0;
    Q_INVOKABLE virtual void openDevTools() = 0;
    Q_INVOKABLE virtual void findInPage(const QString &text, FindFlags flags = {}) = 0;
    Q_INVOKABLE virtual void attach(QQuickItem *viewport) = 0;
    Q_INVOKABLE virtual void setMuted(bool muted) = 0;
    Q_INVOKABLE virtual void executeContextMenuCommand(const QString &command);

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
    void newViewRequested(EngineNewViewRequest *request);
    void contextMenuRequested(const ContextMenuInfo &info);
    void fullscreenRequested(bool fullscreen);
};

}

Q_DECLARE_OPERATORS_FOR_FLAGS(eden::engine::EngineView::FindFlags)
Q_DECLARE_METATYPE(eden::engine::ContextMenuInfo)
