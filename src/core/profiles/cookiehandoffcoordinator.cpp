#include "core/profiles/cookiehandoffcoordinator.h"

#include "engine/engineprofile.h"
#include "engine/engineregistry.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>

namespace eden::core {

    CookieHandoffCoordinator::CookieHandoffCoordinator(const QString &profileId, QObject *parent)
        : QObject(parent),
          m_profileId(profileId) {
        m_deadline.setSingleShot(true);
        m_deadline.setInterval(deadlineMilliseconds);
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            finishCurrent(ProfileError::HandoffFailed, QStringLiteral("deadline"));
        });
    }

    CookieHandoffCoordinator::~CookieHandoffCoordinator() {
        engine::wipePortableCookies(m_snapshot);
        ++m_generation;
        if (m_current && m_current->completion) {
            m_current->completion(ProfileError::Cancelled, QStringLiteral("destroyed"));
        }
        for (Operation &operation : m_queue) {
            if (operation.completion) {
                operation.completion(ProfileError::Cancelled, QStringLiteral("destroyed"));
            }
        }
        m_queue.clear();
    }

    void CookieHandoffCoordinator::setBlocked(bool blocked) {
        m_blocked = blocked;
        if (!blocked) {
            return;
        }
        std::deque<Operation> rejected;
        rejected.swap(m_queue);
        for (Operation &operation : rejected) {
            if (operation.completion) {
                operation.completion(ProfileError::SignOutBlocked, QStringLiteral("blocked"));
            }
        }
        if (m_current) {
            finishCurrent(ProfileError::Cancelled, QStringLiteral("blocked"));
        }
    }

    bool CookieHandoffCoordinator::isBlocked() const {
        return m_blocked;
    }

    bool CookieHandoffCoordinator::active() const {
        return static_cast<bool>(m_current);
    }

    qsizetype CookieHandoffCoordinator::pendingCount() const {
        return static_cast<qsizetype>(m_queue.size()) + (m_current ? 1 : 0);
    }

    QString CookieHandoffCoordinator::backendIdOf(const std::shared_ptr<engine::EngineProfile> &profile) {
        return profile ? engine::EngineRegistry::instance()->idForBackend(profile->backend()) : QString();
    }

    void CookieHandoffCoordinator::requestHandoff(
        std::shared_ptr<engine::EngineProfile> source,
        std::shared_ptr<engine::EngineProfile> destination,
        Completion completion
    ) {
        const auto reject = [&completion](ProfileError error, const QString &code) {
            if (completion) {
                completion(error, code);
            }
        };
        if (m_blocked) {
            reject(ProfileError::SignOutBlocked, QStringLiteral("blocked"));
            return;
        }
        if (!source || !destination || source == destination) {
            reject(ProfileError::HandoffUnsupported, QStringLiteral("invalid_endpoints"));
            return;
        }
        if (source->isPrivate() || destination->isPrivate()) {
            reject(ProfileError::HandoffUnsupported, QStringLiteral("private_mode"));
            return;
        }
        if (source->backend() == destination->backend()) {
            reject(ProfileError::HandoffUnsupported, QStringLiteral("equal_backend"));
            return;
        }
        if (source->profileId() != m_profileId || destination->profileId() != m_profileId) {
            reject(ProfileError::HandoffUnsupported, QStringLiteral("cross_profile"));
            return;
        }
        if (!source->supportsPortableCookies() || !destination->supportsPortableCookies()) {
            reject(ProfileError::HandoffUnsupported, QStringLiteral("unsupported_adapter"));
            return;
        }
        Operation operation;
        operation.source = std::move(source);
        operation.destination = std::move(destination);
        operation.completion = std::move(completion);
        m_queue.push_back(std::move(operation));
        if (!m_current) {
            startNext();
        }
    }

    void CookieHandoffCoordinator::startNext() {
        if (m_current || m_queue.empty()) {
            return;
        }
        m_current = std::make_unique<Operation>(std::move(m_queue.front()));
        m_queue.pop_front();
        m_current->startedAtMilliseconds = QDateTime::currentMSecsSinceEpoch();
        m_deadline.start();
        beginExport();
    }

    void CookieHandoffCoordinator::beginExport() {
        const quint64 generation = ++m_generation;
        QPointer<CookieHandoffCoordinator> guard(this);
        m_current->source->exportPortableCookies([guard, generation](engine::CookieSnapshotResult result) {
            if (!guard) {
                engine::wipePortableCookies(result.cookies);
                return;
            }
            guard->handleSnapshot(generation, std::move(result));
        });
    }

    void CookieHandoffCoordinator::handleSnapshot(quint64 generation, engine::CookieSnapshotResult result) {
        if (generation != m_generation || !m_current) {
            engine::wipePortableCookies(result.cookies);
            return;
        }
        if (!result.errorCode.isEmpty()) {
            engine::wipePortableCookies(result.cookies);
            finishCurrent(ProfileError::HandoffFailed, QStringLiteral("export_") + result.errorCode);
            return;
        }
        if (m_current->source->profileId() != m_profileId || m_current->destination->profileId() != m_profileId) {
            engine::wipePortableCookies(result.cookies);
            finishCurrent(ProfileError::HandoffUnsupported, QStringLiteral("cross_profile"));
            return;
        }
        m_snapshot = std::move(result.cookies);
        m_current->cookieCount = m_snapshot.size();
        m_current->skippedCount = result.skippedCookies;
        const quint64 replaceGeneration = m_generation;
        QPointer<CookieHandoffCoordinator> guard(this);
        m_current->destination->replacePortableCookies(
            m_snapshot,
            [guard, replaceGeneration](engine::CookieReplaceResult result) {
                if (!guard) {
                    return;
                }
                guard->handleReplace(replaceGeneration, std::move(result));
            }
        );
    }

    void CookieHandoffCoordinator::handleReplace(quint64 generation, engine::CookieReplaceResult result) {
        if (generation != m_generation || !m_current) {
            return;
        }
        m_current->skippedCount += result.skippedCookies;
        if (!result.errorCode.isEmpty()) {
            finishCurrent(ProfileError::HandoffFailed, QStringLiteral("replace_") + result.errorCode);
            return;
        }
        finishCurrent(ProfileError::None, QStringLiteral("ok"));
    }

    void CookieHandoffCoordinator::finishCurrent(ProfileError error, const QString &diagnosticCode) {
        if (!m_current) {
            return;
        }
        m_deadline.stop();
        ++m_generation;
        engine::wipePortableCookies(m_snapshot);
        const std::unique_ptr<Operation> finished = std::move(m_current);
        const qint64 duration = QDateTime::currentMSecsSinceEpoch() - finished->startedAtMilliseconds;
        emit handoffFinished(
            backendIdOf(finished->source),
            backendIdOf(finished->destination),
            finished->cookieCount,
            finished->skippedCount,
            duration,
            diagnosticCode
        );
        if (finished->completion) {
            finished->completion(error, diagnosticCode);
        }
        startNext();
    }

}
