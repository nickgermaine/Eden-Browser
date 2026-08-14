#include "engine/engineplugin.h"
#include "engine/qtwebengine/qtwebengineprofile.h"
#include "engine/qtwebengine/qtwebengineview.h"

#include <QQuickWebEngineProfile>
#include <QString>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

namespace eden::engine {

    static QString &qtWebEngineUserAgent() {
        static QString userAgent;
        return userAgent;
    }

    static bool initializeQtWebEngine(int, char *[], const EngineIdentity *identity) {
        if (!identity || !identity->product || !identity->version) {
            return false;
        }
        QtWebEngineQuick::initialize();
        QString &userAgent = qtWebEngineUserAgent();
        if (userAgent.isEmpty()) {
            QQuickWebEngineProfile *defaultProfile = QQuickWebEngineProfile::defaultProfile();
            userAgent = defaultProfile->httpUserAgent();
            userAgent.append(' ');
            userAgent.append(QString::fromUtf8(identity->product));
            userAgent.append('/');
            userAgent.append(QString::fromUtf8(identity->version));
            defaultProfile->setHttpUserAgent(userAgent);
        }
        return true;
    }

    static EngineView *createQtWebEngineView(EngineProfile *profile) {
        return new QtWebEngineView(profile);
    }

    static EngineProfile *createQtWebEngineProfile(bool privateProfile, QQmlEngine *qmlEngine, QObject *parent) {
        return new QtWebEngineProfile(privateProfile, qtWebEngineUserAgent(), qmlEngine, parent);
    }

    static void shutdownQtWebEngine() {}

}

extern "C" const eden::engine::EnginePluginApi *eden_engine_plugin() {
    static const eden::engine::EnginePluginApi api{
        eden::engine::enginePluginAbiVersion,
        eden::engine::initializeQtWebEngine,
        eden::engine::createQtWebEngineView,
        eden::engine::createQtWebEngineProfile,
        eden::engine::shutdownQtWebEngine
    };
    return &api;
}
