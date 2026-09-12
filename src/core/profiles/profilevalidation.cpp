#include "core/profiles/profilevalidation.h"

namespace eden::core {

    static qsizetype scalarValueCount(const QString &text) {
        qsizetype count = 0;
        for (qsizetype index = 0; index < text.size(); ++index) {
            if (text.at(index).isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
                ++index;
            }
            ++count;
        }
        return count;
    }

    std::optional<QString> normalizedDisplayName(const QString &raw) {
        const QString trimmed = raw.trimmed();
        const qsizetype scalars = scalarValueCount(trimmed);
        if (scalars < 1 || scalars > 64) {
            return std::nullopt;
        }
        for (const QChar character : trimmed) {
            const QChar::Category category = character.category();
            if (category == QChar::Other_Control || category == QChar::Separator_Line ||
                category == QChar::Separator_Paragraph) {
                return std::nullopt;
            }
            if (character == QLatin1Char('/') || character == QLatin1Char('\\')) {
                return std::nullopt;
            }
        }
        return trimmed;
    }

    ProfileError validateProfilePassword(const QString &password, const QString &confirmation) {
        if (password != confirmation) {
            return ProfileError::InvalidPassword;
        }
        const qsizetype scalars = scalarValueCount(password);
        if (scalars < 8 || scalars > 256) {
            return ProfileError::InvalidPassword;
        }
        if (password.toUtf8().size() > 1024) {
            return ProfileError::InvalidPassword;
        }
        return ProfileError::None;
    }

}
