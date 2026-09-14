#include "engine/wpe/wpeprofile.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>

namespace eden::engine {

    static std::optional<PortableCookie> portableCookie(SoupCookie *native) {
        PortableCookie cookie;
        cookie.name = soup_cookie_get_name(native);
        cookie.value = soup_cookie_get_value(native);
        cookie.domain = soup_cookie_get_domain(native);
        cookie.path = soup_cookie_get_path(native);
        cookie.hostOnly = !cookie.domain.startsWith('.');
        cookie.secure = soup_cookie_get_secure(native);
        cookie.httpOnly = soup_cookie_get_http_only(native);
        if (auto *expires = soup_cookie_get_expires(native)) {
            cookie.expires = QDateTime::fromSecsSinceEpoch(g_date_time_to_unix(expires), QTimeZone::UTC);
        }
        switch (soup_cookie_get_same_site_policy(native)) {
        case SOUP_SAME_SITE_POLICY_NONE:
            cookie.sameSite = CookieSameSite::None;
            break;
        case SOUP_SAME_SITE_POLICY_LAX:
            cookie.sameSite = CookieSameSite::Lax;
            break;
        case SOUP_SAME_SITE_POLICY_STRICT:
            cookie.sameSite = CookieSameSite::Strict;
            break;
        }
        if (!canonicalizePortableCookie(cookie, QDateTime::currentMSecsSinceEpoch())) {
            wipePortableCookie(cookie);
            return std::nullopt;
        }
        return cookie;
    }

    static SoupCookie *nativeCookie(const PortableCookie &cookie) {
        if (cookie.name.contains('\0') || cookie.value.contains('\0')) {
            return nullptr;
        }
        const QByteArray domain = cookie.hostOnly ? cookie.domain : '.' + cookie.domain;
        SoupCookie *native = soup_cookie_new(
            cookie.name.constData(),
            cookie.value.constData(),
            domain.constData(),
            cookie.path.constData(),
            -1
        );
        if (!native) {
            return nullptr;
        }
        soup_cookie_set_secure(native, cookie.secure);
        soup_cookie_set_http_only(native, cookie.httpOnly);
        if (cookie.expires) {
            auto *expires = g_date_time_new_from_unix_utc(cookie.expires->toSecsSinceEpoch());
            soup_cookie_set_expires(native, expires);
            g_date_time_unref(expires);
        }
        const auto policy = cookie.sameSite == CookieSameSite::None     ? SOUP_SAME_SITE_POLICY_NONE
                            : cookie.sameSite == CookieSameSite::Strict ? SOUP_SAME_SITE_POLICY_STRICT
                                                                        : SOUP_SAME_SITE_POLICY_LAX;
        soup_cookie_set_same_site_policy(native, policy);
        return native;
    }

    static bool equivalentCookies(const QList<PortableCookie> &actual, const QList<PortableCookie> &expected) {
        if (actual.size() != expected.size()) {
            return false;
        }
        QHash<QByteArray, const PortableCookie *> indexed;
        for (const auto &cookie : actual) {
            indexed.insert(portableCookieIdentity(cookie), &cookie);
        }
        for (const auto &cookie : expected) {
            const auto *stored = indexed.value(portableCookieIdentity(cookie));
            const auto sameSite =
                cookie.sameSite == CookieSameSite::Unspecified ? CookieSameSite::Lax : cookie.sameSite;
            if (!stored || stored->value != cookie.value || stored->secure != cookie.secure ||
                stored->httpOnly != cookie.httpOnly || stored->sameSite != sameSite ||
                stored->expires.has_value() != cookie.expires.has_value()) {
                return false;
            }
            if (stored->expires && stored->expires->toSecsSinceEpoch() != cookie.expires->toSecsSinceEpoch()) {
                return false;
            }
        }
        return true;
    }

    static bool representableCookie(const PortableCookie &cookie) {
        const QByteArray separators("()<>@,;:\"/[]?={}");
        for (const auto byte : cookie.name) {
            if (static_cast<unsigned char>(byte) < 0x21 || static_cast<unsigned char>(byte) >= 0x7f ||
                byte == char(0x5c) || separators.contains(byte)) {
                return false;
            }
        }
        for (const auto &bytes : {cookie.value, cookie.path}) {
            for (const auto byte : bytes) {
                if (static_cast<unsigned char>(byte) < 0x20 || byte == 0x7f || byte == ';') {
                    return false;
                }
            }
        }
        if (!cookie.path.startsWith('/')) {
            return false;
        }
        if (cookie.sameSite == CookieSameSite::None && !cookie.secure) {
            return false;
        }
        if (cookie.name.startsWith("__Secure-") && !cookie.secure) {
            return false;
        }
        if (cookie.name.startsWith("__Host-") && (!cookie.secure || !cookie.hostOnly || cookie.path != "/")) {
            return false;
        }
        return true;
    }

    class WpeCookieOperation final : public std::enable_shared_from_this<WpeCookieOperation> {
      public:
        explicit WpeCookieOperation(WpeProfile *owner)
            : profile(owner),
              session(WEBKIT_NETWORK_SESSION(g_object_ref(owner->session()))),
              manager(webkit_network_session_get_cookie_manager(session)),
              cancellable(g_cancellable_new()) {}

        ~WpeCookieOperation() {
            wipePortableCookies(source);
            wipePortableCookies(backup);
            wipePortableCookies(removing);
            g_object_unref(cancellable);
            QTimer::singleShot(0, QCoreApplication::instance(), [retained = session] { g_object_unref(retained); });
        }

        void snapshot() {
            webkit_cookie_manager_get_all_cookies(
                manager,
                cancellable,
                [](GObject *object, GAsyncResult *result, gpointer data) {
                    std::unique_ptr<std::shared_ptr<WpeCookieOperation>> holder(
                        static_cast<std::shared_ptr<WpeCookieOperation> *>(data)
                    );
                    const auto operation = *holder;
                    GError *error = nullptr;
                    GList *cookies =
                        webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(object), result, &error);
                    if (operation->finished) {
                        g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
                        g_clear_error(&error);
                        return;
                    }
                    QList<PortableCookie> values;
                    for (auto *entry = cookies; entry; entry = entry->next) {
                        auto cookie = portableCookie(static_cast<SoupCookie *>(entry->data));
                        if (cookie) {
                            values.append(std::move(*cookie));
                        } else if (operation->snapshotCallback) {
                            ++operation->skipped;
                        }
                    }
                    g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
                    if (error) {
                        g_clear_error(&error);
                        wipePortableCookies(values);
                        operation->complete(operation->rollingBack ? "rollback-failed" : "snapshot-failed");
                        return;
                    }
                    if (operation->snapshotCallback) {
                        operation->finished = true;
                        auto callback = std::move(operation->snapshotCallback);
                        operation->retire();
                        callback({std::move(values), operation->skipped, {}});
                        return;
                    }
                    if (operation->verifying) {
                        operation->verifying = false;
                        const bool matches =
                            equivalentCookies(values, operation->rollingBack ? operation->backup : operation->source);
                        wipePortableCookies(values);
                        if (matches) {
                            operation->complete(operation->rollingBack ? "replace-failed" : QString());
                        } else {
                            operation->failed();
                        }
                        return;
                    }
                    if (!operation->rollingBack) {
                        operation->backup = values;
                    }
                    operation->removing = std::move(values);
                    operation->index = 0;
                    operation->deleting = true;
                    operation->advance();
                },
                new std::shared_ptr<WpeCookieOperation>(shared_from_this())
            );
        }

        void advance() {
            if (finished) {
                return;
            }
            const auto &inserting = rollingBack ? backup : source;
            if (deleting && index >= removing.size()) {
                deleting = false;
                index = 0;
                wipePortableCookies(removing);
            }
            if (!deleting && index >= inserting.size()) {
                verifying = true;
                snapshot();
                return;
            }
            SoupCookie *cookie = nativeCookie(deleting ? removing[index] : inserting[index]);
            if (!cookie) {
                failed();
                return;
            }
            const auto callback = [](GObject *object, GAsyncResult *result, gpointer data) {
                std::unique_ptr<std::shared_ptr<WpeCookieOperation>> holder(
                    static_cast<std::shared_ptr<WpeCookieOperation> *>(data)
                );
                const auto operation = *holder;
                GError *error = nullptr;
                const bool success =
                    operation->deleting
                        ? webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(object), result, &error)
                        : webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(object), result, &error);
                g_clear_error(&error);
                if (operation->finished) {
                    return;
                }
                if (!success) {
                    operation->failed();
                    return;
                }
                ++operation->index;
                operation->advance();
            };
            auto *holder = new std::shared_ptr<WpeCookieOperation>(shared_from_this());
            if (deleting) {
                webkit_cookie_manager_delete_cookie(manager, cookie, cancellable, callback, holder);
            } else {
                webkit_cookie_manager_add_cookie(manager, cookie, cancellable, callback, holder);
            }
            soup_cookie_free(cookie);
        }

        void failed() {
            if (rollingBack) {
                complete("rollback-failed");
                return;
            }
            rollingBack = true;
            wipePortableCookies(removing);
            snapshot();
        }

        void cancel() {
            g_cancellable_cancel(cancellable);
            complete("profile-destroyed");
        }

        void complete(const QString &error) {
            if (finished) {
                return;
            }
            finished = true;
            const qsizetype imported = error.isEmpty() ? source.size() : 0;
            auto snapshotDone = std::move(snapshotCallback);
            auto replaceDone = std::move(replaceCallback);
            retire();
            wipePortableCookies(source);
            wipePortableCookies(backup);
            wipePortableCookies(removing);
            if (snapshotDone) {
                snapshotDone({{}, skipped, error});
            }
            if (replaceDone) {
                replaceDone({imported, skipped, error});
            }
        }

        void retire() {
            if (profile) {
                profile->retireOperation(this);
            }
        }

        QPointer<WpeProfile> profile;
        WebKitNetworkSession *session;
        WebKitCookieManager *manager;
        GCancellable *cancellable;
        CookieSnapshotCallback snapshotCallback;
        CookieReplaceCallback replaceCallback;
        QList<PortableCookie> source;
        QList<PortableCookie> backup;
        QList<PortableCookie> removing;
        qsizetype skipped = 0;
        qsizetype index = 0;
        bool deleting = true;
        bool rollingBack = false;
        bool finished = false;
        bool verifying = false;
    };

    WpeProfile::WpeProfile(const EngineProfileParameters &parameters, QObject *parent)
        : EngineProfile(parameters, parent),
          m_session(
              parameters.privateProfile ? webkit_network_session_new_ephemeral()
                                        : webkit_network_session_new(
                                              parameters.dataPath.toUtf8().constData(),
                                              parameters.cachePath.toUtf8().constData()
                                          )
          ),
          m_context(webkit_web_context_new()),
          m_cancellable(g_cancellable_new()) {
        m_storageReady =
            parameters.privateProfile || (QDir().mkpath(parameters.dataPath) && QDir().mkpath(parameters.cachePath));
        if (!parameters.privateProfile && m_storageReady) {
            const auto cookiePath = QDir(parameters.dataPath).filePath("cookies.sqlite").toUtf8();
            webkit_cookie_manager_set_persistent_storage(
                webkit_network_session_get_cookie_manager(m_session),
                cookiePath.constData(),
                WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE
            );
        }
        webkit_network_session_set_persistent_credential_storage_enabled(m_session, FALSE);
        webkit_network_session_set_tls_errors_policy(m_session, WEBKIT_TLS_ERRORS_POLICY_FAIL);
        webkit_web_context_set_cache_model(m_context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
        g_signal_connect(
            m_session,
            "download-started",
            G_CALLBACK(+[](WebKitNetworkSession *, WebKitDownload *download, WpeProfile *profile) {
                profile->startDownload(download);
            }),
            this
        );
    }

    WpeProfile::~WpeProfile() {
        m_closing = true;
        const auto operations = std::move(m_operations);
        for (const auto &operation : operations) {
            operation->cancel();
        }
        g_cancellable_cancel(m_cancellable);
        g_signal_handlers_disconnect_by_data(m_session, this);
        for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
            g_signal_handlers_disconnect_by_data(it.key(), this);
            webkit_download_cancel(it.key());
            if (it->announced) {
                emit downloadUpdated(
                    it->id,
                    qint64(webkit_download_get_received_data_length(it.key())),
                    it->total,
                    "interrupted"
                );
            }
            g_object_unref(it.key());
        }
        g_object_unref(m_cancellable);
        g_object_unref(m_context);
        g_object_unref(m_session);
    }

    QObject *WpeProfile::nativeProfile() const {
        return const_cast<WpeProfile *>(this);
    }
    WebKitNetworkSession *WpeProfile::session() const {
        return m_session;
    }
    WebKitWebContext *WpeProfile::context() const {
        return m_context;
    }
    bool WpeProfile::supportsPortableCookies() const {
        return !isPrivate() && m_storageReady;
    }

    bool WpeProfile::storageReady() const {
        return m_storageReady;
    }

    void WpeProfile::clearData() {
        auto *manager = webkit_network_session_get_website_data_manager(m_session);
        webkit_website_data_manager_clear(
            manager,
            WEBKIT_WEBSITE_DATA_ALL,
            0,
            m_cancellable,
            [](GObject *object, GAsyncResult *result, gpointer) {
                GError *error = nullptr;
                if (!webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(object), result, &error) &&
                    !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    qWarning("WebKit could not clear website data");
                }
                g_clear_error(&error);
            },
            nullptr
        );
    }

    void WpeProfile::exportPortableCookies(CookieSnapshotCallback callback) {
        if (!callback) {
            return;
        }
        if (m_closing || !m_storageReady) {
            callback({{}, 0, m_closing ? "profile-destroyed" : "storage-unavailable"});
            return;
        }
        if (isPrivate()) {
            callback({{}, 0, "private-profile"});
            return;
        }
        auto operation = std::make_shared<WpeCookieOperation>(this);
        operation->snapshotCallback = std::move(callback);
        m_operations.append(operation);
        operation->snapshot();
    }

    void WpeProfile::replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) {
        if (!callback) {
            return;
        }
        if (m_closing || !m_storageReady) {
            callback({0, 0, m_closing ? "profile-destroyed" : "storage-unavailable"});
            return;
        }
        if (isPrivate()) {
            callback({0, 0, "private-profile"});
            return;
        }
        for (const auto &operation : m_operations) {
            if (operation->replaceCallback) {
                callback({0, 0, "operation-in-progress"});
                return;
            }
        }
        auto operation = std::make_shared<WpeCookieOperation>(this);
        operation->replaceCallback = std::move(callback);
        QSet<QByteArray> seen;
        for (auto cookie : cookies) {
            if (!canonicalizePortableCookie(cookie, QDateTime::currentMSecsSinceEpoch()) ||
                !representableCookie(cookie) || seen.contains(portableCookieIdentity(cookie))) {
                ++operation->skipped;
                wipePortableCookie(cookie);
                continue;
            }
            seen.insert(portableCookieIdentity(cookie));
            operation->source.append(std::move(cookie));
        }
        m_operations.append(operation);
        operation->snapshot();
    }

    void WpeProfile::retireOperation(WpeCookieOperation *operation) {
        m_operations.removeIf([operation](const auto &entry) { return entry.get() == operation; });
    }

    void WpeProfile::flushStorage(std::function<void()> completion) {
        if (!completion) {
            return;
        }
        if (m_closing) {
            completion();
            return;
        }
        using NativeFlush = void (*)(WebKitNetworkSession *, GAsyncReadyCallback, gpointer);
        const auto flush = reinterpret_cast<NativeFlush>(
            g_type_get_qdata(WEBKIT_TYPE_NETWORK_SESSION, g_quark_from_static_string("eden-flush-storage-v1"))
        );
        if (flush) {
            auto *callback = new std::function<void()>(std::move(completion));
            flush(
                m_session,
                [](GObject *, GAsyncResult *result, gpointer data) {
                    std::unique_ptr<std::function<void()>> callback(static_cast<std::function<void()> *>(data));
                    GError *error = nullptr;
                    g_task_propagate_boolean(G_TASK(result), &error);
                    g_clear_error(&error);
                    (*callback)();
                },
                callback
            );
            return;
        }
        auto operation = std::make_shared<WpeCookieOperation>(this);
        operation->snapshotCallback = [completion = std::move(completion)](CookieSnapshotResult result) {
            wipePortableCookies(result.cookies);
            completion();
        };
        m_operations.append(operation);
        operation->snapshot();
    }

    void WpeProfile::startDownload(WebKitDownload *download) {
        if (m_nextDownload == 0) {
            webkit_download_cancel(download);
            return;
        }
        DownloadState state;
        state.id = downloadIdentifier(m_nextDownload++);
        g_object_ref(download);
        m_downloads.insert(download, state);
        g_signal_connect(
            download,
            "decide-destination",
            G_CALLBACK(+[](WebKitDownload *download, const char *suggested, WpeProfile *profile) -> gboolean {
                auto found = profile->m_downloads.find(download);
                if (found == profile->m_downloads.end()) {
                    return FALSE;
                }
                const QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
                found->destination = reserveDownloadPath(directory, QString::fromUtf8(suggested));
                if (!found->destination) {
                    webkit_download_cancel(download);
                    return TRUE;
                }
                const auto *response = webkit_download_get_response(download);
                found->total =
                    response ? qint64(webkit_uri_response_get_content_length(const_cast<WebKitURIResponse *>(response)))
                             : -1;
                webkit_download_set_allow_overwrite(download, FALSE);
                webkit_download_set_destination(download, found->destination->toUtf8().constData());
                found->announced = true;
                emit profile->downloadStarted(
                    found->id,
                    QFileInfo(*found->destination).fileName(),
                    QUrl(QString::fromUtf8(webkit_uri_request_get_uri(webkit_download_get_request(download)))),
                    *found->destination,
                    found->total
                );
                return TRUE;
            }),
            this
        );
        g_signal_connect(
            download,
            "received-data",
            G_CALLBACK(+[](WebKitDownload *download, guint64, WpeProfile *profile) {
                const auto found = profile->m_downloads.constFind(download);
                if (found != profile->m_downloads.cend() && found->announced) {
                    emit profile->downloadUpdated(
                        found->id,
                        qint64(webkit_download_get_received_data_length(download)),
                        found->total,
                        "downloading"
                    );
                }
            }),
            this
        );
        g_signal_connect(
            download,
            "failed",
            G_CALLBACK(+[](WebKitDownload *download, GError *error, WpeProfile *profile) {
                profile->finishDownload(
                    download,
                    g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)
                        ? "cancelled"
                        : "interrupted"
                );
            }),
            this
        );
        g_signal_connect(
            download,
            "finished",
            G_CALLBACK(+[](WebKitDownload *download, WpeProfile *profile) {
                profile->finishDownload(download, "complete");
            }),
            this
        );
    }

    void WpeProfile::finishDownload(WebKitDownload *download, const QString &state) {
        const auto found = m_downloads.find(download);
        if (found == m_downloads.end()) {
            return;
        }
        const auto value = *found;
        m_downloads.erase(found);
        g_signal_handlers_disconnect_by_data(download, this);
        if (value.announced) {
            emit downloadUpdated(
                value.id,
                qint64(webkit_download_get_received_data_length(download)),
                value.total,
                state
            );
        }
        g_object_unref(download);
    }

}
