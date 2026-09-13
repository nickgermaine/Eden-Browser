#pragma once

#include <QList>
#include <QObject>
#include <QPointer>

class QQuickWindow;

namespace eden::core {

    class WindowController;

    class WindowRegistry final : public QObject {
        Q_OBJECT

      public:
        explicit WindowRegistry(QObject *parent = nullptr);
        ~WindowRegistry() override;

        static WindowRegistry *instance();

        void registerWindow(
            QQuickWindow *window,
            WindowController *controller,
            const QString &profileId,
            bool privateWindow
        );
        void unregisterWindow(WindowController *controller);
        void touchActivation(WindowController *controller);

        QList<WindowController *> allControllers() const;
        QList<WindowController *>
        profileControllers(const QString &profileId, bool includeNormal, bool includePrivate) const;
        WindowController *mostRecentProfileController(const QString &profileId, bool privateOnly) const;
        QQuickWindow *windowFor(WindowController *controller) const;
        QString profileIdFor(const WindowController *controller) const;
        bool isPrivateWindow(const WindowController *controller) const;
        int normalWindowCount() const;
        int normalWindowCountForProfile(const QString &profileId) const;
        QString mostRecentActiveProfileId(bool includePrivate = false) const;

      signals:
        void windowsChanged();

      private:
        struct Entry {
            QPointer<QQuickWindow> window;
            WindowController *controller = nullptr;
            QString profileId;
            bool privateWindow = false;
            quint64 activationOrdinal = 0;
        };

        const Entry *entryFor(const WindowController *controller) const;

        QList<Entry> m_entries;
        quint64 m_activationCounter = 0;
    };

}
