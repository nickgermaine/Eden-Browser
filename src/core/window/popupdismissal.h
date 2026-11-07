#pragma once

#include <QList>
#include <QObject>
#include <QPointF>

class QQuickWindow;

namespace eden::core {

    class PopupDismissal final : public QObject {
        Q_OBJECT

      public:
        enum class Interaction {
            Press,
            Wheel,
        };

        explicit PopupDismissal(QObject *parent = nullptr);
        ~PopupDismissal() override;

        static bool dismissOutside(QQuickWindow *window, const QList<QPointF> &scenePoints, Interaction interaction);
        static void dismissTransient(QQuickWindow *window);

      protected:
        bool eventFilter(QObject *watched, QEvent *event) override;

      private:
        static void dismissOtherWindows(QQuickWindow *window);
    };

}
