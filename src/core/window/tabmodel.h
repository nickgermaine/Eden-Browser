#pragma once

#include "engine/enginefactory.h"
#include "engine/engineview.h"

#include <QAbstractListModel>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

namespace eden::engine {
class EngineProfile;
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
        InternalPageRole
    };

    using ViewFactory = std::function<std::unique_ptr<engine::EngineView>()>;

    TabModel(ViewFactory factory, bool privateMode, QObject *parent = nullptr, std::shared_ptr<engine::EngineProfile> profileLease = {});
    ~TabModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE int addTab(const QUrl &url = QUrl("about:blank"), bool background = false);
    Q_INVOKABLE bool closeTab(int index);
    Q_INVOKABLE bool undoClose();
    Q_INVOKABLE bool moveTab(int from, int to);
    Q_INVOKABLE void pinTab(int index, bool pinned);
    Q_INVOKABLE int duplicateTab(int index);
    Q_INVOKABLE void closeOthers(int index);
    Q_INVOKABLE void toggleMuted(int index);
    Q_INVOKABLE QObject *engineAt(int index) const;
    bool transferTabTo(int index, TabModel *destination, int destinationIndex);
    int pinnedCount() const;
    bool canUndoClose() const;
    engine::EngineView *engineViewAt(int index) const;
    engine::EngineProfile *profileAt(int index) const;

  signals:
    void countChanged();
    void pinnedCountChanged();
    void canUndoCloseChanged();
    void operationOccurred();
    void tabAdded(int index, bool background);
    void tabMoved(int from, int to);
    void tabTransferredOut(engine::EngineView *view);
    void tabCloseRequestedForWindow();
    void externalViewRequested(engine::EngineNewViewRequest *request);

  private:
    struct Tab {
        std::unique_ptr<engine::EngineView> view;
        std::shared_ptr<engine::EngineProfile> profileLease;
        QUrl url;
        QString title;
        QString internalPage;
        bool pinned = false;
        bool privateMode = false;
    };

    struct ClosedTab {
        std::unique_ptr<Tab> tab;
        int index = 0;
    };

    void connectTab(Tab &tab);
    int indexOf(const engine::EngineView *view) const;
    void notifyViewChanged(engine::EngineView *view, const QList<int> &roles);

    ViewFactory m_factory;
    std::shared_ptr<engine::EngineProfile> m_profileLease;
    bool m_privateMode = false;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::unique_ptr<ClosedTab> m_closedTab;
    QTimer m_undoTimer;
};

}
