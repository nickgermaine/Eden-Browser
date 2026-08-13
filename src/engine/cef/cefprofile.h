#pragma once

#include "engine/engineprofile.h"

#include "include/cef_request_context.h"

namespace eden::engine::cef {

class CefProfile final : public EngineProfile {
    Q_OBJECT

  public:
    explicit CefProfile(bool privateProfile, QObject *parent = nullptr);
    ~CefProfile() override;

    QObject *nativeProfile() const override;
    void clearData() override;
    CefRefPtr<CefRequestContext> requestContext() const;

  private:
    CefRefPtr<CefRequestContext> m_requestContext;
};

}
