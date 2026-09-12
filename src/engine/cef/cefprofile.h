#pragma once

#include "engine/engineprofile.h"

#include "include/cef_request_context.h"

namespace eden::engine::cef {

    class CefProfile final : public EngineProfile {
        Q_OBJECT

      public:
        explicit CefProfile(const EngineProfileParameters &parameters, QObject *parent = nullptr);
        ~CefProfile() override;

        QObject *nativeProfile() const override;
        void clearData() override;
        CefRefPtr<CefRequestContext> requestContext() const;

        bool supportsPortableCookies() const override;
        void exportPortableCookies(CookieSnapshotCallback callback) override;
        void replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback) override;

        void flushStorage(std::function<void()> completion) override;

      private:
        CefRefPtr<CefCookieManager> cookieManager() const;

        CefRefPtr<CefRequestContext> m_requestContext;
        bool m_replacingCookies = false;
    };

}
