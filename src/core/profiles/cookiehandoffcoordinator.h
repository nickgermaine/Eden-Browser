#pragma once

#include "core/profiles/profileerror.h"
#include "engine/portablecookie.h"

#include <QObject>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>

namespace eden::engine {
    class EngineProfile;
}

namespace eden::core {

    class CookieHandoffCoordinator final : public QObject {
        Q_OBJECT

      public:
        static constexpr int deadlineMilliseconds = 5000;

        using Completion = std::function<void(ProfileError error, const QString &diagnosticCode)>;

        explicit CookieHandoffCoordinator(const QString &profileId, QObject *parent = nullptr);
        ~CookieHandoffCoordinator() override;

        void setBlocked(bool blocked);
        bool isBlocked() const;
        bool active() const;
        qsizetype pendingCount() const;

        void requestHandoff(
            std::shared_ptr<engine::EngineProfile> source,
            std::shared_ptr<engine::EngineProfile> destination,
            Completion completion
        );

      signals:
        void handoffFinished(
            const QString &sourceBackend,
            const QString &destinationBackend,
            qsizetype cookieCount,
            qsizetype skippedCount,
            qint64 durationMilliseconds,
            const QString &errorCode
        );

      private:
        struct Operation {
            std::shared_ptr<engine::EngineProfile> source;
            std::shared_ptr<engine::EngineProfile> destination;
            Completion completion;
            qint64 startedAtMilliseconds = 0;
            qsizetype cookieCount = 0;
            qsizetype skippedCount = 0;
        };

        void startNext();
        void beginExport();
        void handleSnapshot(quint64 generation, engine::CookieSnapshotResult result);
        void handleReplace(quint64 generation, engine::CookieReplaceResult result);
        void finishCurrent(ProfileError error, const QString &diagnosticCode);
        static QString backendIdOf(const std::shared_ptr<engine::EngineProfile> &profile);

        QString m_profileId;
        std::deque<Operation> m_queue;
        std::unique_ptr<Operation> m_current;
        QList<engine::PortableCookie> m_snapshot;
        QTimer m_deadline;
        quint64 m_generation = 0;
        bool m_blocked = false;
    };

}
