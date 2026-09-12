#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>

#include <functional>
#include <optional>

namespace eden::engine {

    enum class CookieSameSite {
        Unspecified,
        None,
        Lax,
        Strict,
    };

    struct PortableCookie {
        QByteArray name;
        QByteArray value;
        QByteArray domain;
        QByteArray path;
        std::optional<QDateTime> expires;
        CookieSameSite sameSite = CookieSameSite::Unspecified;
        bool secure = false;
        bool httpOnly = false;
        bool hostOnly = false;
    };

    struct CookieSnapshotResult {
        QList<PortableCookie> cookies;
        qsizetype skippedCookies = 0;
        QString errorCode;
    };

    struct CookieReplaceResult {
        qsizetype importedCookies = 0;
        qsizetype skippedCookies = 0;
        QString errorCode;
    };

    using CookieSnapshotCallback = std::function<void(CookieSnapshotResult)>;
    using CookieReplaceCallback = std::function<void(CookieReplaceResult)>;

    bool canonicalizePortableCookie(PortableCookie &cookie, qint64 nowUtcMilliseconds);
    QByteArray portableCookieIdentity(const PortableCookie &cookie);
    void wipePortableCookie(PortableCookie &cookie);
    void wipePortableCookies(QList<PortableCookie> &cookies);

}
