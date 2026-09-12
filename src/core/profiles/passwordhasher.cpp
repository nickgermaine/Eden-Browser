#include "core/profiles/passwordhasher.h"

#include <sodium.h>

namespace eden::core {

    static bool ensureSodium() {
        static const bool initialized = sodium_init() >= 0;
        return initialized;
    }

    class PasswordHasherWorker final : public QObject {
      public:
        explicit PasswordHasherWorker(PasswordHasher *facade)
            : m_facade(facade) {}

        void hash(QString password, PasswordHasher::HashCallback callback) {
            QString verifier;
            const bool success = PasswordHasher::hashPasswordBlocking(password, verifier);
            password.fill(QChar(0));
            deliver([callback = std::move(callback), success, verifier] { callback(success, verifier); });
        }

        void verify(QString verifier, QString password, PasswordHasher::VerifyCallback callback) {
            bool needsRehash = false;
            const bool matches = PasswordHasher::verifyPasswordBlocking(verifier, password, needsRehash);
            password.fill(QChar(0));
            deliver([callback = std::move(callback), matches, needsRehash] { callback(matches, needsRehash); });
        }

      private:
        void deliver(std::function<void()> notification) {
            QMetaObject::invokeMethod(m_facade, std::move(notification), Qt::QueuedConnection);
        }

        PasswordHasher *m_facade;
    };

    PasswordHasher::PasswordHasher(QObject *parent)
        : QObject(parent) {
        ensureSodium();
        m_thread.setObjectName(QStringLiteral("eden-password-hasher"));
        m_worker = new PasswordHasherWorker(this);
        m_worker->moveToThread(&m_thread);
        connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
    }

    PasswordHasher::~PasswordHasher() {
        m_thread.quit();
        m_thread.wait();
    }

    void PasswordHasher::hashPassword(const QString &password, HashCallback callback) {
        QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, password, callback = std::move(callback)]() mutable {
                worker->hash(std::move(password), std::move(callback));
            },
            Qt::QueuedConnection
        );
    }

    void PasswordHasher::verifyPassword(const QString &verifier, const QString &password, VerifyCallback callback) {
        QMetaObject::invokeMethod(
            m_worker,
            [worker = m_worker, verifier, password, callback = std::move(callback)]() mutable {
                worker->verify(std::move(verifier), std::move(password), std::move(callback));
            },
            Qt::QueuedConnection
        );
    }

    bool PasswordHasher::hashPasswordBlocking(const QString &password, QString &verifier) {
        if (!ensureSodium()) {
            return false;
        }
        QByteArray utf8 = password.toUtf8();
        if (utf8.isEmpty() || utf8.size() > 1024) {
            sodium_memzero(utf8.data(), static_cast<size_t>(utf8.capacity()));
            return false;
        }
        char encoded[crypto_pwhash_STRBYTES];
        const int result = crypto_pwhash_str(
            encoded,
            utf8.constData(),
            static_cast<unsigned long long>(utf8.size()),
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE
        );
        sodium_memzero(utf8.data(), static_cast<size_t>(utf8.capacity()));
        if (result != 0) {
            return false;
        }
        verifier = QString::fromLatin1(encoded);
        sodium_memzero(encoded, sizeof(encoded));
        return true;
    }

    bool PasswordHasher::verifyPasswordBlocking(const QString &verifier, const QString &password, bool &needsRehash) {
        needsRehash = false;
        if (!ensureSodium()) {
            return false;
        }
        const QByteArray encoded = verifier.toLatin1();
        if (encoded.isEmpty() || encoded.size() >= static_cast<qsizetype>(crypto_pwhash_STRBYTES)) {
            return false;
        }
        char bounded[crypto_pwhash_STRBYTES] = {};
        memcpy(bounded, encoded.constData(), static_cast<size_t>(encoded.size()));
        QByteArray utf8 = password.toUtf8();
        const bool matches =
            crypto_pwhash_str_verify(bounded, utf8.constData(), static_cast<unsigned long long>(utf8.size())) == 0;
        sodium_memzero(utf8.data(), static_cast<size_t>(utf8.capacity()));
        if (matches) {
            needsRehash = crypto_pwhash_str_needs_rehash(
                              bounded,
                              crypto_pwhash_OPSLIMIT_MODERATE,
                              crypto_pwhash_MEMLIMIT_MODERATE
                          ) != 0;
        }
        sodium_memzero(bounded, sizeof(bounded));
        return matches;
    }

}
