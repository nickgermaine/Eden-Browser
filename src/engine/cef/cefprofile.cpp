#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/cef/cefuibridge.h"

#include "include/cef_cookie.h"

#include <QDateTime>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QUrl>

#include <atomic>
#include <memory>
#include <utility>

namespace eden::engine::cef {

    static constexpr qint64 windowsToUnixEpochMilliseconds = 11644473600000LL;

    static qint64 baseTimeToEpochMilliseconds(cef_basetime_t baseTime) {
        return baseTime.val / 1000 - windowsToUnixEpochMilliseconds;
    }

    static cef_basetime_t epochMillisecondsToBaseTime(qint64 epochMilliseconds) {
        cef_basetime_t baseTime{};
        baseTime.val = (epochMilliseconds + windowsToUnixEpochMilliseconds) * 1000;
        return baseTime;
    }

    static PortableCookie portableFromCef(const CefCookie &cookie) {
        PortableCookie portable;
        portable.name = QByteArray::fromStdString(CefString(&cookie.name).ToString());
        portable.value = QByteArray::fromStdString(CefString(&cookie.value).ToString());
        const QByteArray rawDomain = QByteArray::fromStdString(CefString(&cookie.domain).ToString());
        portable.hostOnly = !rawDomain.startsWith('.');
        portable.domain = rawDomain;
        portable.path = QByteArray::fromStdString(CefString(&cookie.path).ToString());
        portable.secure = cookie.secure != 0;
        portable.httpOnly = cookie.httponly != 0;
        if (cookie.has_expires != 0) {
            portable.expires = QDateTime::fromMSecsSinceEpoch(baseTimeToEpochMilliseconds(cookie.expires), Qt::UTC);
        }
        switch (cookie.same_site) {
        case CEF_COOKIE_SAME_SITE_NO_RESTRICTION:
            portable.sameSite = CookieSameSite::None;
            break;
        case CEF_COOKIE_SAME_SITE_LAX_MODE:
            portable.sameSite = CookieSameSite::Lax;
            break;
        case CEF_COOKIE_SAME_SITE_STRICT_MODE:
            portable.sameSite = CookieSameSite::Strict;
            break;
        default:
            portable.sameSite = CookieSameSite::Unspecified;
            break;
        }
        return portable;
    }

    static CefCookie cefFromPortable(const PortableCookie &portable) {
        CefCookie cookie;
        CefString(&cookie.name) = portable.name.toStdString();
        CefString(&cookie.value) = portable.value.toStdString();
        if (!portable.hostOnly) {
            CefString(&cookie.domain) = ("." + portable.domain).toStdString();
        }
        CefString(&cookie.path) = portable.path.toStdString();
        cookie.secure = portable.secure ? 1 : 0;
        cookie.httponly = portable.httpOnly ? 1 : 0;
        if (portable.expires) {
            cookie.has_expires = 1;
            cookie.expires = epochMillisecondsToBaseTime(portable.expires->toMSecsSinceEpoch());
        }
        switch (portable.sameSite) {
        case CookieSameSite::None:
            cookie.same_site = CEF_COOKIE_SAME_SITE_NO_RESTRICTION;
            break;
        case CookieSameSite::Lax:
            cookie.same_site = CEF_COOKIE_SAME_SITE_LAX_MODE;
            break;
        case CookieSameSite::Strict:
            cookie.same_site = CEF_COOKIE_SAME_SITE_STRICT_MODE;
            break;
        case CookieSameSite::Unspecified:
            cookie.same_site = CEF_COOKIE_SAME_SITE_UNSPECIFIED;
            break;
        }
        return cookie;
    }

    static CefString cookieOriginUrl(const PortableCookie &portable) {
        QUrl url;
        url.setScheme(portable.secure ? QStringLiteral("https") : QStringLiteral("http"));
        url.setHost(QString::fromLatin1(portable.domain));
        url.setPath(QString::fromUtf8(portable.path));
        return CefString(url.toEncoded().toStdString());
    }

    class CollectingCookieVisitor final : public CefCookieVisitor {
      public:
        CollectingCookieVisitor(
            bool deleteVisited,
            std::function<void(QList<PortableCookie>, qsizetype, bool)> completion
        )
            : m_deleteVisited(deleteVisited),
              m_completion(std::move(completion)) {}

        ~CollectingCookieVisitor() override {
            if (m_completion) {
                auto completion = std::move(m_completion);
                auto cookies = std::move(m_cookies);
                const qsizetype skipped = m_skipped;
                const bool failed = m_failed;
                CefUiBridge::runOnUiThread(
                    [completion = std::move(completion), cookies = std::move(cookies), skipped, failed]() mutable {
                        completion(std::move(cookies), skipped, failed);
                    }
                );
            }
        }

        void fail() {
            m_failed = true;
        }

        bool Visit(const CefCookie &cookie, int, int, bool &deleteCookie) override {
            PortableCookie portable = portableFromCef(cookie);
            if (canonicalizePortableCookie(portable, QDateTime::currentMSecsSinceEpoch())) {
                const QByteArray identity = portableCookieIdentity(portable);
                if (!m_identities.contains(identity)) {
                    m_identities.insert(identity);
                    m_cookies.append(std::move(portable));
                } else {
                    wipePortableCookie(portable);
                }
            } else {
                wipePortableCookie(portable);
                ++m_skipped;
            }
            deleteCookie = m_deleteVisited;
            return true;
        }

      private:
        std::atomic_bool m_failed = false;
        bool m_deleteVisited;
        std::function<void(QList<PortableCookie>, qsizetype, bool)> m_completion;
        QList<PortableCookie> m_cookies;
        QSet<QByteArray> m_identities;
        qsizetype m_skipped = 0;

        IMPLEMENT_REFCOUNTING(CollectingCookieVisitor);
    };

    static void visitOrdinaryCookies(CefRefPtr<CefCookieManager> manager, CefRefPtr<CollectingCookieVisitor> visitor) {
        CefRefPtr<CollectingCookieVisitor> inventory = new CollectingCookieVisitor(
            false,
            [manager, visitor](QList<PortableCookie> cookies, qsizetype, bool failed) {
                if (failed) {
                    visitor->fail();
                    wipePortableCookies(cookies);
                    return;
                }
                QSet<QString> urls;
                for (const PortableCookie &cookie : cookies) {
                    urls.insert(QString::fromStdString(cookieOriginUrl(cookie).ToString()));
                }
                wipePortableCookies(cookies);
                for (const QString &url : urls) {
                    if (!manager->VisitUrlCookies(url.toStdString(), true, visitor)) {
                        visitor->fail();
                    }
                }
            }
        );
        if (!manager->VisitAllCookies(inventory)) {
            inventory->fail();
        }
    }

    class CookieSetCompletion final : public CefSetCookieCallback {
      public:
        CookieSetCompletion(
            std::shared_ptr<qsizetype> remaining,
            std::shared_ptr<qsizetype> failures,
            std::function<void()> allDone
        )
            : m_remaining(std::move(remaining)),
              m_failures(std::move(failures)),
              m_allDone(std::move(allDone)) {}

        void OnComplete(bool success) override {
            CefUiBridge::runOnUiThread([remaining = m_remaining, failures = m_failures, allDone = m_allDone, success] {
                if (!success) {
                    ++*failures;
                }
                if (--*remaining == 0 && allDone) {
                    allDone();
                }
            });
        }

      private:
        std::shared_ptr<qsizetype> m_remaining;
        std::shared_ptr<qsizetype> m_failures;
        std::function<void()> m_allDone;

        IMPLEMENT_REFCOUNTING(CookieSetCompletion);
    };

    class StoreFlushCompletion final : public CefCompletionCallback {
      public:
        explicit StoreFlushCompletion(std::function<void()> completion)
            : m_completion(std::move(completion)) {}

        void OnComplete() override {
            if (m_completion) {
                CefUiBridge::runOnUiThread(std::move(m_completion));
            }
        }

      private:
        std::function<void()> m_completion;

        IMPLEMENT_REFCOUNTING(StoreFlushCompletion);
    };

    static void insertCookies(
        const CefRefPtr<CefCookieManager> &manager,
        const QList<PortableCookie> &cookies,
        std::function<void(qsizetype failures)> completion
    ) {
        if (cookies.isEmpty()) {
            completion(0);
            return;
        }
        auto remaining = std::make_shared<qsizetype>(cookies.size());
        auto failures = std::make_shared<qsizetype>(0);
        auto allDone = [failures, completion = std::move(completion)] {
            completion(*failures);
        };
        for (const PortableCookie &portable : cookies) {
            CefRefPtr<CookieSetCompletion> callback = new CookieSetCompletion(remaining, failures, allDone);
            if (!manager->SetCookie(cookieOriginUrl(portable), cefFromPortable(portable), callback)) {
                callback->OnComplete(false);
            }
        }
    }

    CefProfile::CefProfile(const EngineProfileParameters &parameters, QObject *parent)
        : EngineProfile(parameters, parent),
          m_requestContext(
              CefRuntime::instance().createRequestContext(
                  parameters.privateProfile,
                  parameters.privateProfile ? std::filesystem::path()
                                            : std::filesystem::path(parameters.dataPath.toStdString())
              )
          ) {}

    CefProfile::~CefProfile() = default;

    QObject *CefProfile::nativeProfile() const {
        return nullptr;
    }

    void CefProfile::clearData() {
        if (!m_requestContext) {
            return;
        }
        CefRefPtr<CefCookieManager> cookieManager = m_requestContext->GetCookieManager(nullptr);
        if (cookieManager) {
            cookieManager->DeleteCookies({}, {}, nullptr);
        }
        m_requestContext->ClearHttpCache(nullptr);
    }

    CefRefPtr<CefRequestContext> CefProfile::requestContext() const {
        return m_requestContext;
    }

    CefRefPtr<CefCookieManager> CefProfile::cookieManager() const {
        return m_requestContext ? m_requestContext->GetCookieManager(nullptr) : nullptr;
    }

    bool CefProfile::supportsPortableCookies() const {
        return !isPrivate() && m_requestContext;
    }

    void CefProfile::exportPortableCookies(CookieSnapshotCallback callback) {
        if (!supportsPortableCookies()) {
            EngineProfile::exportPortableCookies(std::move(callback));
            return;
        }
        CefRefPtr<CefCookieManager> manager = cookieManager();
        if (!manager) {
            CookieSnapshotResult result;
            result.errorCode = QStringLiteral("store_unavailable");
            CefUiBridge::runOnUiThread([callback = std::move(callback), result = std::move(result)]() mutable {
                callback(std::move(result));
            });
            return;
        }
        QPointer<CefProfile> guard(this);
        CefRefPtr<CollectingCookieVisitor> visitor = new CollectingCookieVisitor(
            false,
            [guard,
             callback = std::move(callback)](QList<PortableCookie> cookies, qsizetype skipped, bool failed) mutable {
                CookieSnapshotResult result;
                if (!guard) {
                    wipePortableCookies(cookies);
                    result.errorCode = QStringLiteral("profile_destroyed");
                    callback(std::move(result));
                    return;
                }
                if (failed) {
                    wipePortableCookies(cookies);
                    result.errorCode = QStringLiteral("cookie_read_failed");
                }
                result.cookies = std::move(cookies);
                result.skippedCookies = skipped;
                callback(std::move(result));
            }
        );
        visitOrdinaryCookies(manager, visitor);
    }

    void CefProfile::replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) {
        if (!supportsPortableCookies()) {
            EngineProfile::replacePortableCookies(cookies, std::move(callback));
            return;
        }
        CefRefPtr<CefCookieManager> manager = cookieManager();
        if (!manager) {
            CookieReplaceResult result;
            result.errorCode = QStringLiteral("store_unavailable");
            CefUiBridge::runOnUiThread([callback = std::move(callback), result = std::move(result)]() mutable {
                callback(std::move(result));
            });
            return;
        }
        if (m_replacingCookies) {
            callback({0, 0, "cookie_store_busy"});
            return;
        }
        auto imported = std::make_shared<QList<PortableCookie>>(cookies);
        for (PortableCookie &cookie : *imported) {
            if (!canonicalizePortableCookie(cookie, QDateTime::currentMSecsSinceEpoch())) {
                wipePortableCookies(*imported);
                callback({0, 0, "invalid_cookie"});
                return;
            }
        }
        m_replacingCookies = true;
        auto sharedCallback = std::make_shared<CookieReplaceCallback>(std::move(callback));
        QPointer<CefProfile> guard(this);
        const auto finish = [guard, sharedCallback, imported](qsizetype importedCount, const QString &error) {
            wipePortableCookies(*imported);
            if (guard) {
                guard->m_replacingCookies = false;
            }
            auto callback = std::exchange(*sharedCallback, {});
            if (callback) {
                callback({importedCount, 0, guard ? error : QStringLiteral("profile_destroyed")});
            }
        };
        const auto flush = [manager, finish](qsizetype count, const QString &error) {
            CefRefPtr<StoreFlushCompletion> completion =
                new StoreFlushCompletion([finish, count, error] { finish(count, error); });
            if (!manager->FlushStore(completion)) {
                finish(0, QStringLiteral("cookie_flush_failed"));
            }
        };
        CefRefPtr<CollectingCookieVisitor> snapshot = new CollectingCookieVisitor(
            false,
            [guard, manager, imported, finish, flush](QList<PortableCookie> before, qsizetype, bool failed) {
                if (!guard || failed) {
                    wipePortableCookies(before);
                    finish(0, QStringLiteral("cookie_read_failed"));
                    return;
                }
                auto rollback = std::make_shared<QList<PortableCookie>>(std::move(before));
                const auto restore = [manager, rollback, flush](bool alreadyFailed) {
                    insertCookies(manager, *rollback, [rollback, flush, alreadyFailed](qsizetype failures) {
                        wipePortableCookies(*rollback);
                        flush(
                            0,
                            failures == 0 && !alreadyFailed ? QStringLiteral("rolled_back")
                                                            : QStringLiteral("rollback_failed")
                        );
                    });
                };
                CefRefPtr<CollectingCookieVisitor> deletion = new CollectingCookieVisitor(
                    true,
                    [manager,
                     imported,
                     rollback,
                     restore,
                     flush](QList<PortableCookie> deleted, qsizetype, bool failed) {
                        wipePortableCookies(deleted);
                        if (failed) {
                            restore(false);
                            return;
                        }
                        insertCookies(
                            manager,
                            *imported,
                            [manager, imported, rollback, restore, flush](qsizetype failures) {
                                if (failures == 0) {
                                    wipePortableCookies(*rollback);
                                    flush(imported->size(), {});
                                    return;
                                }
                                CefRefPtr<CollectingCookieVisitor> undo = new CollectingCookieVisitor(
                                    true,
                                    [restore](QList<PortableCookie> undone, qsizetype, bool failed) {
                                        wipePortableCookies(undone);
                                        restore(failed);
                                    }
                                );
                                visitOrdinaryCookies(manager, undo);
                            }
                        );
                    }
                );
                visitOrdinaryCookies(manager, deletion);
            }
        );
        visitOrdinaryCookies(manager, snapshot);
    }

    void CefProfile::flushStorage(std::function<void()> completion) {
        CefRefPtr<CefCookieManager> manager = cookieManager();
        if (!manager) {
            if (completion) {
                CefUiBridge::runOnUiThread(std::move(completion));
            }
            return;
        }
        CefRefPtr<StoreFlushCompletion> callback = new StoreFlushCompletion(completion);
        if (!manager->FlushStore(callback) && completion) {
            CefUiBridge::runOnUiThread(std::move(completion));
        }
    }

}
