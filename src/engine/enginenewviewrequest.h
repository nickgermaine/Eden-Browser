#pragma once

#include <QObject>
#include <QUrl>

namespace eden::engine {

    class EngineView;

    class EngineNewViewRequest : public QObject {
        Q_OBJECT
        Q_PROPERTY(QUrl requestedUrl READ requestedUrl CONSTANT)
        Q_PROPERTY(Disposition disposition READ disposition CONSTANT)
        Q_PROPERTY(bool userInitiated READ isUserInitiated CONSTANT)

      public:
        enum class Disposition { CurrentTab, NewForegroundTab, NewBackgroundTab, NewWindow };
        Q_ENUM(Disposition)

        EngineNewViewRequest(
            const QUrl &requestedUrl,
            Disposition disposition,
            bool userInitiated,
            QObject *parent = nullptr
        );
        ~EngineNewViewRequest() override;

        QUrl requestedUrl() const;
        Disposition disposition() const;
        bool isUserInitiated() const;
        virtual bool openIn(EngineView *target) = 0;

      private:
        QUrl m_requestedUrl;
        Disposition m_disposition;
        bool m_userInitiated;
    };

}

Q_DECLARE_METATYPE(eden::engine::EngineNewViewRequest *)
