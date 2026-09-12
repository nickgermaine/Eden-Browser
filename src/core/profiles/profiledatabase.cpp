#include "core/profiles/profiledatabase.h"

#include "core/profiles/sqlcipherdatabase.h"

namespace eden::core {

    ProfileDatabase::ProfileDatabase(QString databasePath)
        : m_databasePath(std::move(databasePath)) {}

    bool ProfileDatabase::initialize(const QByteArray &key) {
        if (m_initialized) {
            return true;
        }
        m_initialized = SqlCipherDatabase::initialize(m_databasePath, key);
        return m_initialized;
    }

    bool ProfileDatabase::isInitialized() const {
        return m_initialized;
    }

    const QString &ProfileDatabase::databasePath() const {
        return m_databasePath;
    }

}
