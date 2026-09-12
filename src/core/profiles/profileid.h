#pragma once

#include <QHash>
#include <QString>

#include <optional>

namespace eden::core {

    class ProfileId {
      public:
        ProfileId() = default;

        static ProfileId generate();
        static std::optional<ProfileId> parse(const QString &text);
        static bool isCanonical(const QString &text);

        const QString &toString() const;
        bool isValid() const;

        friend bool operator==(const ProfileId &left, const ProfileId &right) = default;

      private:
        explicit ProfileId(QString canonical);

        QString m_value;
    };

    inline size_t qHash(const ProfileId &id, size_t seed = 0) {
        return ::qHash(id.toString(), seed);
    }

}
