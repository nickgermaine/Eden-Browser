#include "engine/portablecookie.h"

#include <sodium.h>

namespace eden::engine {

    static bool isLowerAsciiDomain(const QByteArray &domain) {
        if (domain.isEmpty()) {
            return false;
        }
        for (const char character : domain) {
            const bool lower = character >= 'a' && character <= 'z';
            const bool digit = character >= '0' && character <= '9';
            if (!lower && !digit && character != '-' && character != '.' && character != '_' && character != '[' &&
                character != ']' && character != ':') {
                return false;
            }
        }
        return true;
    }

    bool canonicalizePortableCookie(PortableCookie &cookie, qint64 nowUtcMilliseconds) {
        if (cookie.name.isEmpty() && cookie.value.isEmpty()) {
            return false;
        }
        QByteArray domain = cookie.domain.toLower();
        while (domain.startsWith('.')) {
            domain.remove(0, 1);
        }
        if (!isLowerAsciiDomain(domain)) {
            return false;
        }
        cookie.domain = domain;
        if (cookie.path.isEmpty()) {
            cookie.path = QByteArrayLiteral("/");
        }
        if (cookie.expires) {
            if (!cookie.expires->isValid()) {
                return false;
            }
            if (cookie.expires->toMSecsSinceEpoch() <= nowUtcMilliseconds) {
                return false;
            }
            cookie.expires = cookie.expires->toUTC();
        }
        return true;
    }

    QByteArray portableCookieIdentity(const PortableCookie &cookie) {
        QByteArray identity;
        identity.reserve(cookie.name.size() + cookie.domain.size() + cookie.path.size() + 4);
        identity.append(cookie.name);
        identity.append('\x1f');
        identity.append(cookie.domain);
        identity.append('\x1f');
        identity.append(cookie.path);
        identity.append('\x1f');
        identity.append(cookie.hostOnly ? '1' : '0');
        return identity;
    }

    void wipePortableCookie(PortableCookie &cookie) {
        if (!cookie.value.isEmpty()) {
            sodium_memzero(cookie.value.data(), static_cast<size_t>(cookie.value.capacity()));
        }
        cookie.value.clear();
        cookie.name.clear();
        cookie.domain.clear();
        cookie.path.clear();
        cookie.expires.reset();
    }

    void wipePortableCookies(QList<PortableCookie> &cookies) {
        for (PortableCookie &cookie : cookies) {
            wipePortableCookie(cookie);
        }
        cookies.clear();
    }

}
