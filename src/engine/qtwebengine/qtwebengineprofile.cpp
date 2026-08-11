#include "engine/qtwebengine/qtwebengineprofile.h"

#include <QDir>
#include <QFile>
#include <QQuickWebEngineDownloadRequest>
#include <QQuickWebEngineProfile>
#include <QStandardPaths>

namespace eden::engine {

QtWebEngineProfile::QtWebEngineProfile(bool privateProfile, QObject *parent)
    : EngineProfile(privateProfile, parent),
      m_profile(new QQuickWebEngineProfile(this)) {
    if (privateProfile) {
        m_profile->setOffTheRecord(true);
        m_profile->setHttpCacheType(QQuickWebEngineProfile::MemoryHttpCache);
        m_profile->setPersistentCookiesPolicy(QQuickWebEngineProfile::NoPersistentCookies);
    } else {
        const QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/webengine";
        const QString cachePath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/webengine";
        QDir().mkpath(dataPath);
        QDir().mkpath(cachePath);
        QFile::setPermissions(dataPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QFile::setPermissions(cachePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        m_profile->setStorageName("Default");
        m_profile->setOffTheRecord(false);
        m_profile->setPersistentStoragePath(dataPath);
        m_profile->setCachePath(cachePath);
        m_profile->setPersistentCookiesPolicy(QQuickWebEngineProfile::AllowPersistentCookies);
        m_profile->setHttpCacheType(QQuickWebEngineProfile::DiskHttpCache);
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
