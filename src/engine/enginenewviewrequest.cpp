#include "engine/enginenewviewrequest.h"

namespace eden::engine {

EngineNewViewRequest::EngineNewViewRequest(const QUrl &requestedUrl, Disposition disposition, bool userInitiated, QObject *parent)
    : QObject(parent),
      m_requestedUrl(requestedUrl),
      m_disposition(disposition),
      m_userInitiated(userInitiated) {}

EngineNewViewRequest::~EngineNewViewRequest() = default;

QUrl EngineNewViewRequest::requestedUrl() const {
    return m_requestedUrl;
}

EngineNewViewRequest::Disposition EngineNewViewRequest::disposition() const {
    return m_disposition;
}

bool EngineNewViewRequest::isUserInitiated() const {
    return m_userInitiated;
}

}
