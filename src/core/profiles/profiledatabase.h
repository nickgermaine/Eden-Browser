#pragma once

#include <QByteArray>
#include <QString>

namespace eden::core {

    class ProfileDatabase final {
      public:
        explicit ProfileDatabase(QString databasePath);

        bool initialize(const QByteArray &key);
        bool isInitialized() const;
        const QString &databasePath() const;

      private:
        QString m_databasePath;
        bool m_initialized = false;
    };

}
