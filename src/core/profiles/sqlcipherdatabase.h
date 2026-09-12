#pragma once

#include <QByteArray>
#include <QString>

struct sqlite3;

namespace eden::core {

    class SqlCipherDatabase final {
      public:
        static bool initialize(const QString &path, const QByteArray &key);
        static sqlite3 *open(const QString &path, const QByteArray &key, bool create);
        static bool execute(sqlite3 *database, const QByteArray &sql);
        static void restrictFiles(const QString &path);

      private:
        static bool migratePlaintext(const QString &path, const QByteArray &key);
        static bool readable(sqlite3 *database);
        static void discard(const QString &path);
    };

}
