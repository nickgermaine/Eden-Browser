#pragma once

#include <QHash>
#include <QUrl>
#include <limits>

namespace eden::engine {

    class WpeConnectionWarmup final {
      public:
        enum class Kind { Dns, Connection };

        static QUrl origin(const QUrl &url) {
            if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
                (url.scheme() != "https" && url.scheme() != "http") || url.port() == 0) {
                return {};
            }
            QUrl result;
            result.setScheme(url.scheme());
            result.setHost(url.host().toLower());
            result.setPort(url.port());
            result.setPath("/");
            return result;
        }

        bool reserve(const QUrl &url, Kind kind, qint64 now) {
            const QUrl target = origin(url);
            if (target.isEmpty() || now < 0 || now > std::numeric_limits<qint64>::max() - cooldown ||
                (kind == Kind::Connection && target.scheme() != "https")) {
                return false;
            }
            if (kind == Kind::Connection && now < m_connectionWindowEnd &&
                m_connectionCount >= maximumConnectionsPerMinute) {
                return false;
            }
            QString host = target.host();
            while (host.endsWith('.')) {
                host.chop(1);
            }
            if (host.isEmpty()) {
                return false;
            }
            auto found = m_hosts.find(host);
            if (found == m_hosts.end()) {
                if (m_hosts.size() >= maximumHosts) {
                    for (auto entry = m_hosts.begin(); entry != m_hosts.end();) {
                        if (entry->dns <= now && entry->connection <= now) {
                            entry = m_hosts.erase(entry);
                        } else {
                            ++entry;
                        }
                    }
                }
                if (m_hosts.size() >= maximumHosts) {
                    return false;
                }
                found = m_hosts.insert(host, {});
            }
            qint64 &deadline = kind == Kind::Dns ? found->dns : found->connection;
            if (now < deadline) {
                return false;
            }
            deadline = now + cooldown;
            if (kind == Kind::Connection) {
                if (now >= m_connectionWindowEnd) {
                    m_connectionWindowEnd = now + cooldown;
                    m_connectionCount = 0;
                }
                ++m_connectionCount;
            }
            return true;
        }

        qsizetype hostCount() const {
            return m_hosts.size();
        }

      private:
        struct Deadlines {
            qint64 dns = 0;
            qint64 connection = 0;
        };

        static constexpr qint64 cooldown = 60000;
        static constexpr qsizetype maximumHosts = 128;
        static constexpr unsigned maximumConnectionsPerMinute = 16;
        QHash<QString, Deadlines> m_hosts;
        qint64 m_connectionWindowEnd = 0;
        unsigned m_connectionCount = 0;
    };

}
