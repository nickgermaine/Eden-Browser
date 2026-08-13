#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"

#include "include/cef_cookie.h"

namespace eden::engine::cef {

CefProfile::CefProfile(bool privateProfile, QObject *parent)
    : EngineProfile(privateProfile, parent),
      m_requestContext(CefRuntime::instance().createRequestContext(privateProfile)) {}

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

}
