#include "engine/qtwebengine/qtwebengineprofile.h"

#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWebEngineDownloadRequest>
#include <QQuickWebEngineProfile>
#include <QStandardPaths>

namespace eden::engine {

QtWebEngineProfile::QtWebEngineProfile(bool privateProfile, QQmlEngine *engine, QObject *parent)
    : EngineProfile(privateProfile, parent),
      m_profilePrototype(nullptr),
      m_profile(nullptr) {
    if (!engine) {
        qFatal("A QML engine is required to create a web profile");
    }
    QQmlComponent component(engine);
    component.setData("import QtWebEngine\nWebEngineProfilePrototype {}", QUrl());
    if (component.isError()) {
        qFatal("The Qt WebEngine profile prototype is unavailable: %s", qPrintable(component.errorString()));
    }
    QVariantMap properties;
    if (privateProfile) {
        properties.insert("storageName", QString());
        properties.insert("httpCacheType", QQuickWebEngineProfile::MemoryHttpCache);
        properties.insert("persistentCookiesPolicy", QQuickWebEngineProfile::NoPersistentCookies);
    } else {
        const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/webengine";
        const QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/webengine";
        QDir().mkpath(dataPath);
        QDir().mkpath(cachePath);
        QFile::setPermissions(dataPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(cachePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        properties.insert("storageName", "Default");
        properties.insert("persistentStoragePath", dataPath);
        properties.insert("cachePath", cachePath);
        properties.insert("persistentCookiesPolicy", QQuickWebEngineProfile::AllowPersistentCookies);
        properties.insert("httpCacheType", QQuickWebEngineProfile::DiskHttpCache);
    }
    m_profilePrototype = component.createWithInitialProperties(properties);
    if (!m_profilePrototype) {
        qFatal("The Qt WebEngine profile prototype could not be created: %s", qPrintable(component.errorString()));
    }
    m_profilePrototype->setParent(this);
    if (!QMetaObject::invokeMethod(m_profilePrototype, "instance", Q_RETURN_ARG(QQuickWebEngineProfile *, m_profile)) || !m_profile) {
        qFatal("The Qt WebEngine profile could not be created");
    }
    connect(m_profile, &QQuickWebEngineProfile::downloadRequested, this, [this](QQuickWebEngineDownloadRequest *download) {
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        download->setDownloadDirectory(directory);
        const int id = static_cast<int>(download->id());
        emit downloadStarted(id, download->suggestedFileName(), download->url(), directory + "/" + download->suggestedFileName(),
                             download->totalBytes());
        auto update = [this, download, id] {
            QString state = "downloading";
            if (download->state() == QWebEngineDownloadRequest::DownloadCompleted) {
                state = "completed";
            } else if (download->state() == QWebEngineDownloadRequest::DownloadCancelled) {
                state = "cancelled";
            } else if (download->state() == QWebEngineDownloadRequest::DownloadInterrupted) {
                state = "failed";
            }
            emit downloadUpdated(id, download->receivedBytes(), download->totalBytes(), state);
        };
        connect(download, &QWebEngineDownloadRequest::receivedBytesChanged, this, update);
        connect(download, &QWebEngineDownloadRequest::totalBytesChanged, this, update);
        connect(download, &QWebEngineDownloadRequest::stateChanged, this, update);
        download->accept();
    });
}

QtWebEngineProfile::~QtWebEngineProfile() = default;

QObject *QtWebEngineProfile::nativeProfile() const {
    return m_profile;
}

}
