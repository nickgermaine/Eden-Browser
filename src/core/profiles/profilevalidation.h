#pragma once

#include "core/profiles/profileerror.h"

#include <QString>

#include <optional>

namespace eden::core {

    std::optional<QString> normalizedDisplayName(const QString &raw);
    ProfileError validateProfilePassword(const QString &password, const QString &confirmation);

}
