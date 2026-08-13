#pragma once

#include "engine/enginebackend.h"
#include "engine/enginefactory.h"
#include "engine/engineview.h"

#include <QAbstractListModel>
#include <QJsonArray>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

namespace eden::engine {
class EngineProfile;
class EngineRegistry;
}

namespace eden::core {

class TabModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int pinnedCount READ pinnedCount NOTIFY pinnedCountChanged)
    Q_PROPERTY(bool canUndoClose READ canUndoClose NOTIFY canUndoCloseChanged)

  public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        UrlRole,
        FaviconRole,
        LoadingRole,
        ProgressRole,
        PinnedRole,
        AudibleRole,
        MutedRole,
        PrivateRole,
        EngineViewRole,
        InternalPageRole,
        EngineNameRole,
        TabIdRole,
        DiscardedRole
    };

    using LegacyViewFactory = std::function<std::unique_ptr<engine::EngineView>()>;
    using ViewFactory = std::function<std::unique_ptr<engine::EngineView>(engine::Backend backend, engine::EngineProfile *profile)>;
    using ProfileResolver = std::function<std::shared_ptr<engine::EngineProfile>(engine::Backend backend)>;

    TabModel(LegacyViewFactory factory, bool privateMode, QObject *parent = nullptr,
             std::shared_ptr<engine::EngineProfile> profileLease = {});
    TabModel(ViewFactory factory, ProfileResolver profileResolver, const engine::EngineRegistry *registry, engine::Backend defaultBackend,
             bool privateMode, QObject *parent = nullptr);
    ~TabModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE int addTab(const QUrl &url = QUrl("about:blank"), bool background = false, const QString &backendId = {});
    Q_INVOKABLE bool closeTab(int index);
    Q_INVOKABLE bool undoClose();
    Q_INVOKABLE bool moveTab(int from, int to);
    Q_INVOKABLE void pinTab(int index, bool pinned);
    Q_INVOKABLE int duplicateTab(int index);
    Q_INVOKABLE void closeOthers(int index);
    Q_INVOKABLE void toggleMuted(int index);
    Q_INVOKABLE QObject *engineAt(int index) const;
    Q_INVOKABLE bool convertTab(int index, const QString &backendId, bool rehydrate = true);
    Q_INVOKABLE bool convertAllTabs(const QString &backendId, int activeIndex);
    Q_INVOKABLE bool ensureEngine(int index);
    Q_INVOKABLE bool setDefaultBackend(const QString &backendId);
    bool setInternalPage(int index, const QUrl &url);
    bool setInternalPageUrl(int index, const QUrl &url);
    static QString internalPageForUrl(const QUrl &url);
    bool transferTabTo(int index, TabModel *destination, int destinationIndex);
    int restoreTab(const QUrl &url, const QString &title, bool pinned, const QString &backendId);
    void restoreTabs(const QJsonArray &tabs);
    QJsonArray sessionTabs() const;
    int pinnedCount() const;
    bool canUndoClose() const;
    engine::EngineView *engineViewAt(int index) const;
    engine::EngineProfile *profileAt(int index) const;
    QString backendIdAt(int index) const;
    QString defaultBackendId() const;
    quint64 tabIdAt(int index) const;
    bool isDiscarded(int index) const;
    qint64 rendererProcessIdAt(int index) const;
    int rendererProcessUseCount(qint64 processId) const;
    int indexForTabId(quint64 tabId) const;

  signals:
    void countChanged();
    void pinnedCountChanged();
    void canUndoCloseChanged();
    void operationOccurred();
    void tabAdded(int index, bool background);
    void tabMoved(int from, int to);
    void tabTransferredOut(engine::EngineView *view);
    void tabEngineCreated(int index, engine::EngineView *view);
    void tabCloseRequestedForWindow();
    void externalViewRequested(engine::EngineNewViewRequest *request, int sourceIndex, const QString &backendId);

  private:
    struct Tab {
        std::unique_ptr<engine::EngineView> view;
        std::shared_ptr<engine::EngineProfile> profileLease;
        QVariantMap state;
        QUrl url;
        QString title;
        QUrl favicon;
        QString internalPage;
        quint64 id = 0;
        engine::Backend backend = engine::Backend::QtWebEngine;
        bool pinned = false;
        bool privateMode = false;
    };

    struct ClosedTab {
        std::unique_ptr<Tab> tab;
        int index = 0;
    };

    void connectTab(Tab &tab);
    bool discardTab(int index);
    bool createView(int index);
    engine::Backend resolvedBackend(const QString &backendId) const;
    void updateCachedState(Tab &tab);
    int indexOf(const engine::EngineView *view) const;
    void notifyViewChanged(engine::EngineView *view, const QList<int> &roles);

    ViewFactory m_factory;
    ProfileResolver m_profileResolver;
    const engine::EngineRegistry *m_registry;
    engine::Backend m_defaultBackend;
    bool m_privateMode = false;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::unique_ptr<ClosedTab> m_closedTab;
    QTimer m_undoTimer;
};

}
