#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/cef/cefuibridge.h"

#include "include/cef_cookie.h"

#include <QDateTime>
#include <QPointer>

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
        const QByteArray scheme = portable.secure ? QByteArrayLiteral("https://") : QByteArrayLiteral("http://");
        return CefString((scheme + portable.domain + portable.path).toStdString());
    }

    class CollectingCookieVisitor final : public CefCookieVisitor {
      public:
        CollectingCookieVisitor(bool deleteVisited, std::function<void(QList<PortableCookie>, qsizetype)> completion)
            : m_deleteVisited(deleteVisited),
              m_completion(std::move(completion)) {}

        ~CollectingCookieVisitor() override {
            if (m_completion) {
                auto completion = std::move(m_completion);
                auto cookies = std::move(m_cookies);
                const qsizetype skipped = m_skipped;
                CefUiBridge::runOnUiThread([completion = std::move(completion),
                                            cookies = std::move(cookies),
                                            skipped]() mutable { completion(std::move(cookies), skipped); });
            }
        }

        bool Visit(const CefCookie &cookie, int, int, bool &deleteCookie) override {
            PortableCookie portable = portableFromCef(cookie);
            if (canonicalizePortableCookie(portable, QDateTime::currentMSecsSinceEpoch())) {
                m_cookies.append(std::move(portable));
            } else {
                wipePortableCookie(portable);
                ++m_skipped;
            }
            deleteCookie = m_deleteVisited;
            return true;
        }

      private:
        bool m_deleteVisited;
        std::function<void(QList<PortableCookie>, qsizetype)> m_completion;
        QList<PortableCookie> m_cookies;
        qsizetype m_skipped = 0;

        IMPLEMENT_REFCOUNTING(CollectingCookieVisitor);
    };

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
            [guard, callback = std::move(callback)](QList<PortableCookie> cookies, qsizetype skipped) mutable {
                CookieSnapshotResult result;
                if (!guard) {
                    wipePortableCookies(cookies);
                    result.errorCode = QStringLiteral("profile_destroyed");
                    callback(std::move(result));
                    return;
                }
                result.cookies = std::move(cookies);
                result.skippedCookies = skipped;
                callback(std::move(result));
            }
        );
        if (!manager->VisitAllCookies(visitor)) {
            visitor = nullptr;
        }
    }

    void CefProfile::replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) {
        CefRefPtr<CefCookieManager> manager = cookieManager();
        if (!manager) {
            CookieReplaceResult result;
            result.errorCode = QStringLiteral("store_unavailable");
            CefUiBridge::runOnUiThread([callback = std::move(callback), result = std::move(result)]() mutable {
                callback(std::move(result));
            });
            return;
        }
        auto imported = std::make_shared<QList<PortableCookie>>(cookies);
        auto sharedCallback = std::make_shared<CookieReplaceCallback>(std::move(callback));
        QPointer<CefProfile> guard(this);
        const auto finish = [sharedCallback,
                             imported](qsizetype importedCount, qsizetype skipped, const QString &errorCode) {
            wipePortableCookies(*imported);
            CookieReplaceResult result;
            result.importedCookies = importedCount;
            result.skippedCookies = skipped;
            result.errorCode = errorCode;
            if (*sharedCallback) {
                (*sharedCallback)(std::move(result));
            }
        };
        CefRefPtr<CollectingCookieVisitor> snapshotVisitor = new CollectingCookieVisitor(
            false,
            [guard, manager, imported, finish](QList<PortableCookie> rollbackSnapshot, qsizetype) {
                if (!guard) {
                    wipePortableCookies(rollbackSnapshot);
                    finish(0, 0, QStringLiteral("profile_destroyed"));
                    return;
                }
                auto rollback = std::make_shared<QList<PortableCookie>>(std::move(rollbackSnapshot));
                CefRefPtr<CollectingCookieVisitor> deletingVisitor = new CollectingCookieVisitor(
                    true,
                    [manager, imported, rollback, finish](QList<PortableCookie> deleted, qsizetype) {
                        wipePortableCookies(deleted);
                        insertCookies(manager, *imported, [manager, imported, rollback, finish](qsizetype failures) {
                            if (failures == 0) {
                                wipePortableCookies(*rollback);
                                const qsizetype importedCount = imported->size();
                                manager->FlushStore(new StoreFlushCompletion([finish, importedCount] {
                                    finish(importedCount, 0, QString());
                                }));
                                return;
                            }
                            CefRefPtr<CollectingCookieVisitor> undoVisitor = new CollectingCookieVisitor(
                                true,
                                [manager, rollback, finish](QList<PortableCookie> undone, qsizetype) {
                                    wipePortableCookies(undone);
                                    insertCookies(
                                        manager,
                                        *rollback,
                                        [manager, rollback, finish](qsizetype rollbackFailures) {
                                            wipePortableCookies(*rollback);
                                            manager->FlushStore(new StoreFlushCompletion([finish, rollbackFailures] {
                                                finish(
                                                    0,
                                                    0,
                                                    rollbackFailures == 0 ? QStringLiteral("rolled_back")
                                                                          : QStringLiteral("rollback_failed")
                                                );
                                            }));
                                        }
                                    );
                                }
                            );
                            if (!manager->VisitAllCookies(undoVisitor)) {
                                undoVisitor = nullptr;
                            }
                        });
                    }
                );
                if (!manager->VisitAllCookies(deletingVisitor)) {
                    deletingVisitor = nullptr;
                }
            }
        );
        if (!manager->VisitAllCookies(snapshotVisitor)) {
            snapshotVisitor = nullptr;
        }
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
