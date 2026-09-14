#pragma once

#include "engine/engineprofile.h"
#include "engine/wpe/wpeapi.h"
#include <QHash>
#include <memory>

namespace eden::engine {

    class WpeCookieOperation;

    class WpeProfile final : public EngineProfile {
        Q_OBJECT

      public:
        explicit WpeProfile(const EngineProfileParameters &parameters, QObject *parent = nullptr);
        ~WpeProfile() override;
        QObject *nativeProfile() const override;
        void clearData() override;
        bool supportsPortableCookies() const override;
        void exportPortableCookies(CookieSnapshotCallback callback) override;
        void replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) override;
        void flushStorage(std::function<void()> completion) override;
        WebKitNetworkSession *session() const;
        WebKitWebContext *context() const;
        bool storageReady() const;
        void retireOperation(WpeCookieOperation *operation);

      private:
        void startDownload(WebKitDownload *download);
        void finishDownload(WebKitDownload *download, const QString &state);
        struct DownloadState {
            quint64 id = 0;
            qint64 total = -1;
            std::shared_ptr<const QString> destination;
            bool announced = false;
        };
        WebKitNetworkSession *m_session;
        WebKitWebContext *m_context;
        GCancellable *m_cancellable;
        QHash<WebKitDownload *, DownloadState> m_downloads;
        QList<std::shared_ptr<WpeCookieOperation>> m_operations;
        quint32 m_nextDownload = 1;
        bool m_storageReady = false;
        bool m_closing = false;
    };

}
