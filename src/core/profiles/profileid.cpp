#include "core/profiles/profileid.h"

#include <QUuid>

namespace eden::core {

    ProfileId::ProfileId(QString canonical)
        : m_value(std::move(canonical)) {}

    ProfileId ProfileId::generate() {
        return ProfileId(QUuid::createUuid().toString(QUuid::WithoutBraces).toLower());
    }

    bool ProfileId::isCanonical(const QString &text) {
        if (text.size() != 36) {
            return false;
        }
        for (qsizetype index = 0; index < 36; ++index) {
            const QChar character = text.at(index);
            if (index == 8 || index == 13 || index == 18 || index == 23) {
                if (character != QLatin1Char('-')) {
                    return false;
                }
                continue;
            }
            const bool digit = character >= QLatin1Char('0') && character <= QLatin1Char('9');
            const bool lowerHex = character >= QLatin1Char('a') && character <= QLatin1Char('f');
            if (!digit && !lowerHex) {
                return false;
            }
        }
        return true;
    }

    std::optional<ProfileId> ProfileId::parse(const QString &text) {
        if (!isCanonical(text)) {
            return std::nullopt;
        }
        return ProfileId(text);
    }

    const QString &ProfileId::toString() const {
        return m_value;
    }

    bool ProfileId::isValid() const {
        return !m_value.isEmpty();
    }

}
