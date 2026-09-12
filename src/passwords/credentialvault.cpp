#include "passwords/credentialvault.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>

#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")
#include <sodium.h>
#include <sqlcipher/sqlite3.h>

namespace eden::passwords {

    static const SecretSchema vaultSchema = {
        "browser.eden.vault",
        SECRET_SCHEMA_NONE,
        {
            {"profile", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
    };

    static QByteArray columnBlob(sqlite3_stmt *statement, int column) {
        const void *data = sqlite3_column_blob(statement, column);
        const int size = sqlite3_column_bytes(statement, column);
        return data && size > 0 ? QByteArray(static_cast<const char *>(data), size) : QByteArray();
    }

    CredentialVault::CredentialVault(QString databasePath, QString profileId, QObject *parent)
        : QAbstractListModel(parent),
          m_databasePath(std::move(databasePath)),
          m_profileId(std::move(profileId)) {}

    CredentialVault::~CredentialVault() {
        close();
        if (!m_key.isEmpty()) {
            sodium_munlock(m_key.data(), static_cast<size_t>(m_key.size()));
        }
    }

    bool CredentialVault::initialize() {
        if (m_available) {
            return true;
        }
        if (sodium_init() < 0) {
            setError(QStringLiteral("Encryption initialization failed"));
            return false;
        }
        m_key = loadOrCreateKey();
        if (m_key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
            setError(QStringLiteral("The profile key is unavailable"));
            return false;
        }
        if (sodium_mlock(m_key.data(), static_cast<size_t>(m_key.size())) != 0) {
            sodium_memzero(m_key.data(), static_cast<size_t>(m_key.size()));
            m_key.clear();
            setError(QStringLiteral("The profile key could not be locked in memory"));
            return false;
        }
        if (!QDir().mkpath(QFileInfo(m_databasePath).absolutePath()) ||
            sqlite3_open_v2(
                QFile::encodeName(m_databasePath).constData(),
                &m_database,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                nullptr
            ) != SQLITE_OK) {
            setError(QStringLiteral("The encrypted vault could not be opened"));
            close();
            return false;
        }
        const QByteArray keyStatement =
            QByteArrayLiteral("PRAGMA key = \"x'") + m_key.toHex() + QByteArrayLiteral("'\"");
        if (!execute(keyStatement) || !execute("PRAGMA cipher_memory_security = ON") ||
            !execute("PRAGMA secure_delete = ON") || !execute("PRAGMA journal_mode = WAL") ||
            !execute(
                "CREATE TABLE IF NOT EXISTS credentials("
                "id INTEGER PRIMARY KEY, site_hash BLOB NOT NULL, site BLOB NOT NULL, username BLOB NOT NULL, "
                "password BLOB NOT NULL, updated_at INTEGER NOT NULL)"
            ) ||
            !execute("CREATE INDEX IF NOT EXISTS credentials_site ON credentials(site_hash)") ||
            !execute(
                "CREATE TABLE IF NOT EXISTS form_profiles("
                "id INTEGER PRIMARY KEY, payload BLOB NOT NULL, updated_at INTEGER NOT NULL)"
            )) {
            setError(QStringLiteral("The encrypted vault is invalid or locked"));
            close();
            return false;
        }
        restrictFiles();
        setError({});
        m_available = true;
        emit availableChanged();
        reload();
        return true;
    }

    int CredentialVault::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_entries.size();
    }

    QVariant CredentialVault::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
            return {};
        }
        const Entry &entry = m_entries.at(index.row());
        if (role == IdRole) {
            return entry.id;
        }
        if (role == SiteRole) {
            return entry.site;
        }
        if (role == UsernameRole) {
            return entry.username;
        }
        if (role == UpdatedAtRole) {
            return QDateTime::fromMSecsSinceEpoch(entry.updatedAt);
        }
        return {};
    }

    QHash<int, QByteArray> CredentialVault::roleNames() const {
        return {{IdRole, "credentialId"}, {SiteRole, "site"}, {UsernameRole, "username"}, {UpdatedAtRole, "updatedAt"}};
    }

    QString CredentialVault::filter() const {
        return m_filter;
    }

    void CredentialVault::setFilter(const QString &filter) {
        const QString normalized = filter.trimmed();
        if (m_filter == normalized) {
            return;
        }
        m_filter = normalized;
        beginResetModel();
        m_entries.clear();
        for (const Entry &entry : m_allEntries) {
            if (m_filter.isEmpty() || entry.site.contains(m_filter, Qt::CaseInsensitive) ||
                entry.username.contains(m_filter, Qt::CaseInsensitive)) {
                m_entries.append(entry);
            }
        }
        endResetModel();
        emit filterChanged();
    }

    bool CredentialVault::available() const {
        return m_available;
    }

    QString CredentialVault::error() const {
        return m_error;
    }

    qint64
    CredentialVault::saveCredential(qint64 id, const QString &site, const QString &username, const QString &password) {
        const QString normalized = normalizedSite(QUrl::fromUserInput(site));
        if (!m_available || normalized.isEmpty() || password.isEmpty()) {
            return 0;
        }
        sqlite3_stmt *statement = nullptr;
        const QByteArray sql = id > 0
                                   ? QByteArrayLiteral(
                                         "UPDATE credentials SET site_hash=?,site=?,username=?,password=?,updated_at=? "
                                         "WHERE id=?"
                                     )
                                   : QByteArrayLiteral(
                                         "INSERT INTO credentials(site_hash,site,username,password,updated_at) "
                                         "VALUES(?,?,?,?,?)"
                                     );
        if (sqlite3_prepare_v2(m_database, sql.constData(), -1, &statement, nullptr) != SQLITE_OK) {
            return 0;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        const QByteArray hash = siteHash(normalized);
        const QByteArray sealedSite = seal(normalized.toUtf8(), QByteArrayLiteral("site"));
        const QByteArray sealedUsername = seal(username.toUtf8(), QByteArrayLiteral("username"));
        QByteArray passwordBytes = password.toUtf8();
        const QByteArray sealedPassword = seal(passwordBytes, QByteArrayLiteral("password"));
        sodium_memzero(passwordBytes.data(), static_cast<size_t>(passwordBytes.capacity()));
        sqlite3_bind_blob(statement, 1, hash.constData(), hash.size(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(statement, 2, sealedSite.constData(), sealedSite.size(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(statement, 3, sealedUsername.constData(), sealedUsername.size(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(statement, 4, sealedPassword.constData(), sealedPassword.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 5, QDateTime::currentMSecsSinceEpoch());
        if (id > 0) {
            sqlite3_bind_int64(statement, 6, id);
        }
        if (sqlite3_step(statement) != SQLITE_DONE) {
            return 0;
        }
        const qint64 savedId = id > 0 ? id : sqlite3_last_insert_rowid(m_database);
        restrictFiles();
        reload();
        return savedId;
    }

    bool CredentialVault::removeCredential(qint64 id) {
        if (!m_available || id <= 0) {
            return false;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "DELETE FROM credentials WHERE id=?", -1, &statement, nullptr) !=
            SQLITE_OK) {
            return false;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        sqlite3_bind_int64(statement, 1, id);
        const bool removed = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(m_database) == 1;
        if (removed) {
            restrictFiles();
            reload();
        }
        return removed;
    }

    QString CredentialVault::revealPassword(qint64 id) const {
        if (!m_available || id <= 0) {
            return {};
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "SELECT password FROM credentials WHERE id=?", -1, &statement, nullptr) !=
            SQLITE_OK) {
            return {};
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        sqlite3_bind_int64(statement, 1, id);
        if (sqlite3_step(statement) != SQLITE_ROW) {
            return {};
        }
        QByteArray plain = open(columnBlob(statement, 0), QByteArrayLiteral("password"));
        const QString password = QString::fromUtf8(plain);
        sodium_memzero(plain.data(), static_cast<size_t>(plain.capacity()));
        return password;
    }

    QString CredentialVault::usernameForCredential(qint64 id) const {
        if (id <= 0) {
            return {};
        }
        for (const Entry &entry : m_allEntries) {
            if (entry.id == id) {
                return entry.username;
            }
        }
        return {};
    }

    QVariantList CredentialVault::credentialsForUrl(const QUrl &url) const {
        QVariantList matches;
        const QString site = normalizedSite(url);
        if (!m_available || site.isEmpty()) {
            return matches;
        }
        const QByteArray hash = siteHash(site);
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT id,username FROM credentials WHERE site_hash=? ORDER BY updated_at DESC",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return matches;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        sqlite3_bind_blob(statement, 1, hash.constData(), hash.size(), SQLITE_TRANSIENT);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const QByteArray username = open(columnBlob(statement, 1), QByteArrayLiteral("username"));
            QVariantMap match;
            match.insert("id", sqlite3_column_int64(statement, 0));
            match.insert("username", QString::fromUtf8(username));
            matches.append(match);
        }
        return matches;
    }

    QVariantList CredentialVault::credentialSites(const QString &filter) const {
        QVariantList result;
        QMap<QString, int> counts;
        const QString query = filter.trimmed();
        for (const Entry &entry : m_allEntries) {
            if (!query.isEmpty() && !entry.site.contains(query, Qt::CaseInsensitive) &&
                !entry.username.contains(query, Qt::CaseInsensitive)) {
                continue;
            }
            counts[entry.site] += 1;
        }
        for (auto iterator = counts.cbegin(); iterator != counts.cend(); ++iterator) {
            const QUrl url(iterator.key());
            result.append(
                QVariantMap{
                    {"site", iterator.key()},
                    {"host", url.host().isEmpty() ? iterator.key() : url.host()},
                    {"credentialCount", iterator.value()},
                }
            );
        }
        return result;
    }

    QVariantList CredentialVault::credentialsForSite(const QString &site) const {
        QVariantList result;
        const QString normalized = normalizedSite(QUrl::fromUserInput(site));
        if (normalized.isEmpty()) {
            return result;
        }
        for (const Entry &entry : m_allEntries) {
            if (entry.site != normalized) {
                continue;
            }
            result.append(
                QVariantMap{
                    {"id", entry.id},
                    {"site", entry.site},
                    {"username", entry.username},
                    {"updatedAt", QDateTime::fromMSecsSinceEpoch(entry.updatedAt)},
                }
            );
        }
        return result;
    }

    QVariantList CredentialVault::formProfiles() const {
        QVariantList profiles;
        if (!m_available) {
            return profiles;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT id,payload FROM form_profiles ORDER BY updated_at DESC",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return profiles;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const qint64 id = sqlite3_column_int64(statement, 0);
            QByteArray payload = open(columnBlob(statement, 1), QByteArrayLiteral("form-profile"));
            QVariantMap fields = QJsonDocument::fromJson(payload).object().toVariantMap();
            sodium_memzero(payload.data(), static_cast<size_t>(payload.capacity()));
            fields.insert("id", id);
            profiles.append(fields);
        }
        return profiles;
    }

    qint64 CredentialVault::saveFormProfile(qint64 id, const QVariantMap &fields) {
        if (!m_available || fields.value("label").toString().trimmed().isEmpty()) {
            return 0;
        }
        QVariantMap stored = fields;
        stored.remove("id");
        QByteArray plain = QJsonDocument(QJsonObject::fromVariantMap(stored)).toJson(QJsonDocument::Compact);
        const QByteArray payload = seal(plain, QByteArrayLiteral("form-profile"));
        sodium_memzero(plain.data(), static_cast<size_t>(plain.capacity()));
        sqlite3_stmt *statement = nullptr;
        const char *sql = id > 0 ? "UPDATE form_profiles SET payload=?,updated_at=? WHERE id=?"
                                 : "INSERT INTO form_profiles(payload,updated_at) VALUES(?,?)";
        if (sqlite3_prepare_v2(m_database, sql, -1, &statement, nullptr) != SQLITE_OK) {
            return 0;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        sqlite3_bind_blob(statement, 1, payload.constData(), payload.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, QDateTime::currentMSecsSinceEpoch());
        if (id > 0) {
            sqlite3_bind_int64(statement, 3, id);
        }
        if (sqlite3_step(statement) != SQLITE_DONE) {
            return 0;
        }
        restrictFiles();
        emit formProfilesChanged();
        return id > 0 ? id : sqlite3_last_insert_rowid(m_database);
    }

    bool CredentialVault::removeFormProfile(qint64 id) {
        if (!m_available || id <= 0) {
            return false;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_database, "DELETE FROM form_profiles WHERE id=?", -1, &statement, nullptr) !=
            SQLITE_OK) {
            return false;
        }
        const auto finalize = qScopeGuard([statement] { sqlite3_finalize(statement); });
        sqlite3_bind_int64(statement, 1, id);
        const bool removed = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(m_database) == 1;
        if (removed) {
            restrictFiles();
            emit formProfilesChanged();
        }
        return removed;
    }

    QByteArray CredentialVault::sealData(const QByteArray &plain, const QByteArray &purpose) const {
        return m_available ? seal(plain, purpose) : QByteArray();
    }

    QByteArray CredentialVault::openData(const QByteArray &sealed, const QByteArray &purpose) const {
        return m_available ? open(sealed, purpose) : QByteArray();
    }

    QByteArray CredentialVault::derivedKey(const QByteArray &purpose) const {
        if (!m_available || purpose.isEmpty()) {
            return {};
        }
        QByteArray key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES, Qt::Uninitialized);
        crypto_generichash(
            reinterpret_cast<unsigned char *>(key.data()),
            static_cast<size_t>(key.size()),
            reinterpret_cast<const unsigned char *>(purpose.constData()),
            static_cast<unsigned long long>(purpose.size()),
            reinterpret_cast<const unsigned char *>(m_key.constData()),
            static_cast<size_t>(m_key.size())
        );
        return key;
    }

    QByteArray CredentialVault::loadOrCreateKey() {
        GError *error = nullptr;
        const QByteArray profile = m_profileId.toUtf8();
        gchar *stored =
            secret_password_lookup_sync(&vaultSchema, nullptr, &error, "profile", profile.constData(), nullptr);
        if (error) {
            g_error_free(error);
            if (stored) {
                secret_password_free(stored);
            }
            return {};
        }
        if (stored) {
            QByteArray encoded(stored);
            secret_password_free(stored);
            auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
            sodium_memzero(encoded.data(), static_cast<size_t>(encoded.capacity()));
            if (!decoded || decoded.decoded.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) {
                sodium_memzero(decoded.decoded.data(), static_cast<size_t>(decoded.decoded.capacity()));
                return {};
            }
            return std::move(decoded.decoded);
        }
        if (QFileInfo::exists(m_databasePath)) {
            return {};
        }
        QByteArray key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES, Qt::Uninitialized);
        randombytes_buf(key.data(), static_cast<size_t>(key.size()));
        QByteArray encoded = key.toBase64();
        const QByteArray label = QStringLiteral("Eden profile vault %1").arg(m_profileId).toUtf8();
        if (!secret_password_store_sync(
                &vaultSchema,
                SECRET_COLLECTION_DEFAULT,
                label.constData(),
                encoded.constData(),
                nullptr,
                &error,
                "profile",
                profile.constData(),
                nullptr
            )) {
            if (error) {
                g_error_free(error);
            }
            sodium_memzero(key.data(), static_cast<size_t>(key.capacity()));
            sodium_memzero(encoded.data(), static_cast<size_t>(encoded.capacity()));
            return {};
        }
        sodium_memzero(encoded.data(), static_cast<size_t>(encoded.capacity()));
        return key;
    }

    QByteArray CredentialVault::seal(const QByteArray &plain, const QByteArray &purpose) const {
        QByteArray result(
            crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES,
            Qt::Uninitialized
        );
        unsigned char *nonce = reinterpret_cast<unsigned char *>(result.data());
        randombytes_buf(nonce, crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
        unsigned long long encryptedSize = 0;
        crypto_aead_xchacha20poly1305_ietf_encrypt(
            nonce + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
            &encryptedSize,
            reinterpret_cast<const unsigned char *>(plain.constData()),
            static_cast<unsigned long long>(plain.size()),
            reinterpret_cast<const unsigned char *>(purpose.constData()),
            static_cast<unsigned long long>(purpose.size()),
            nullptr,
            nonce,
            reinterpret_cast<const unsigned char *>(m_key.constData())
        );
        result.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + static_cast<qsizetype>(encryptedSize));
        return result;
    }

    QByteArray CredentialVault::open(const QByteArray &sealed, const QByteArray &purpose) const {
        if (sealed.size() < crypto_aead_xchacha20poly1305_ietf_NPUBBYTES + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
            return {};
        }
        QByteArray plain(sealed.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, Qt::Uninitialized);
        unsigned long long plainSize = 0;
        const unsigned char *nonce = reinterpret_cast<const unsigned char *>(sealed.constData());
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(
                reinterpret_cast<unsigned char *>(plain.data()),
                &plainSize,
                nullptr,
                nonce + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
                static_cast<unsigned long long>(sealed.size() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES),
                reinterpret_cast<const unsigned char *>(purpose.constData()),
                static_cast<unsigned long long>(purpose.size()),
                nonce,
                reinterpret_cast<const unsigned char *>(m_key.constData())
            ) != 0) {
            sodium_memzero(plain.data(), static_cast<size_t>(plain.capacity()));
            return {};
        }
        plain.resize(static_cast<qsizetype>(plainSize));
        return plain;
    }

    QByteArray CredentialVault::siteHash(const QString &site) const {
        QByteArray hash(crypto_generichash_BYTES, Qt::Uninitialized);
        const QByteArray value = site.toUtf8();
        crypto_generichash(
            reinterpret_cast<unsigned char *>(hash.data()),
            static_cast<size_t>(hash.size()),
            reinterpret_cast<const unsigned char *>(value.constData()),
            static_cast<unsigned long long>(value.size()),
            reinterpret_cast<const unsigned char *>(m_key.constData()),
            static_cast<size_t>(m_key.size())
        );
        return hash;
    }

    QString CredentialVault::normalizedSite(const QUrl &url) const {
        if (!url.isValid() || url.host().isEmpty()) {
            return {};
        }
        QUrl origin;
        origin.setScheme(url.scheme().toLower());
        origin.setHost(url.host().toLower());
        if (url.port() > 0) {
            origin.setPort(url.port());
        }
        return origin.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::StripTrailingSlash);
    }

    bool CredentialVault::execute(const QByteArray &sql) const {
        char *message = nullptr;
        const int result = sqlite3_exec(m_database, sql.constData(), nullptr, nullptr, &message);
        if (message) {
            sqlite3_free(message);
        }
        return result == SQLITE_OK;
    }

    void CredentialVault::restrictFiles() const {
        const QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        QFile::setPermissions(m_databasePath, permissions);
        QFile::setPermissions(m_databasePath + QStringLiteral("-wal"), permissions);
        QFile::setPermissions(m_databasePath + QStringLiteral("-shm"), permissions);
    }

    void CredentialVault::reload() {
        QList<Entry> entries;
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT id,site,username,updated_at FROM credentials ORDER BY updated_at DESC",
                -1,
                &statement,
                nullptr
            ) == SQLITE_OK) {
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const QByteArray site = open(columnBlob(statement, 1), QByteArrayLiteral("site"));
                const QByteArray username = open(columnBlob(statement, 2), QByteArrayLiteral("username"));
                entries.append(
                    {sqlite3_column_int64(statement, 0),
                     QString::fromUtf8(site),
                     QString::fromUtf8(username),
                     sqlite3_column_int64(statement, 3)}
                );
            }
        }
        sqlite3_finalize(statement);
        beginResetModel();
        m_allEntries = std::move(entries);
        m_entries.clear();
        for (const Entry &entry : m_allEntries) {
            if (m_filter.isEmpty() || entry.site.contains(m_filter, Qt::CaseInsensitive) ||
                entry.username.contains(m_filter, Qt::CaseInsensitive)) {
                m_entries.append(entry);
            }
        }
        endResetModel();
    }

    void CredentialVault::setError(const QString &error) {
        if (m_error == error) {
            return;
        }
        m_error = error;
        emit errorChanged();
    }

    void CredentialVault::close() {
        if (m_database) {
            sqlite3_close_v2(m_database);
            m_database = nullptr;
        }
        if (m_available) {
            m_available = false;
            emit availableChanged();
        }
    }

}
