#pragma once

#include <QUrl>

namespace eden::core {

inline QUrl urlWithoutCredentials(const QUrl &url) {
    QUrl sanitized = url;
    sanitized.setUserInfo({});
    return sanitized;
}

}
