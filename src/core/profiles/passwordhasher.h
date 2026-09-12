#pragma once

#include <QObject>
#include <QString>
#include <QThread>

#include <functional>

namespace eden::core {

    class PasswordHasherWorker;

    class PasswordHasher final : public QObject {
        Q_OBJECT

      public:
        using HashCallback = std::function<void(bool success, QString verifier)>;
        using VerifyCallback = std::function<void(bool matches, bool needsRehash)>;

        explicit PasswordHasher(QObject *parent = nullptr);
        ~PasswordHasher() override;

        void hashPassword(const QString &password, HashCallback callback);
        void verifyPassword(const QString &verifier, const QString &password, VerifyCallback callback);

        static bool hashPasswordBlocking(const QString &password, QString &verifier);
        static bool verifyPasswordBlocking(const QString &verifier, const QString &password, bool &needsRehash);

      private:
        QThread m_thread;
        PasswordHasherWorker *m_worker = nullptr;
    };

}
