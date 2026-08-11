#pragma once

#include <QObject>
#include <QUrl>
#include <memory>

namespace eden::engine {

class EngineProfile : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool privateProfile READ isPrivate CONSTANT)

  public:
    explicit EngineProfile(bool privateProfile, QObject *parent = nullptr);
    ~EngineProfile() override;

    bool isPrivate() const;
    virtual QObject *nativeProfile() const = 0;

    static EngineProfile *defaultProfile();
    static std::unique_ptr<EngineProfile> createPrivateProfile(QObject *parent = nullptr);

  signals:
    void downloadStarted(int id, const QString &fileName, const QUrl &sourceUrl, const QString &targetPath, qint64 totalBytes);
    void downloadUpdated(int id, qint64 receivedBytes, qint64 totalBytes, const QString &state);

  private:
    bool m_privateProfile;
};

}
