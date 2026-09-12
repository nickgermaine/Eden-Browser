#pragma once

#include "engine/engineprofileparameters.h"
#include "engine/portablecookie.h"

#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

namespace eden::engine {

    class EngineProfile : public QObject {
        Q_OBJECT
        Q_PROPERTY(bool privateProfile READ isPrivate CONSTANT)

      public:
        explicit EngineProfile(const EngineProfileParameters &parameters, QObject *parent = nullptr);
        ~EngineProfile() override;

        bool isPrivate() const;
        const QString &profileId() const;
        Backend backend() const;
        const EngineProfileParameters &parameters() const;
        quint64 downloadIdentifier(quint32 nativeId) const;
        static std::shared_ptr<const QString> reserveDownloadPath(const QString &directory, const QString &fileName);

        virtual QObject *nativeProfile() const = 0;
        virtual void clearData() = 0;

        virtual bool supportsPortableCookies() const;
        virtual void exportPortableCookies(CookieSnapshotCallback callback);
        virtual void replacePortableCookies(const QList<PortableCookie> &cookies, CookieReplaceCallback callback);
        virtual void flushStorage(std::function<void()> completion);

      signals:
        void downloadStarted(
            quint64 id,
            const QString &fileName,
            const QUrl &sourceUrl,
            const QString &targetPath,
            qint64 totalBytes
        );
        void downloadUpdated(quint64 id, qint64 receivedBytes, qint64 totalBytes, const QString &state);

      private:
        EngineProfileParameters m_parameters;
        const quint64 m_downloadPrefix;
    };

}
