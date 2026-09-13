#pragma once

#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QRegion>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace eden::core {

    class WindowInputRegion : public QObject {
        Q_OBJECT
        QML_ELEMENT
        Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged)
        Q_PROPERTY(QRectF rect READ rect WRITE setRect NOTIFY rectChanged)
        Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)

      public:
        explicit WindowInputRegion(QObject *parent = nullptr);
        ~WindowInputRegion() override;

        QWindow *window() const;
        void setWindow(QWindow *window);
        QRectF rect() const;
        void setRect(const QRectF &rect);
        qreal radius() const;
        void setRadius(qreal radius);
        QRegion inputRegion(qreal scale = 1) const;

      signals:
        void windowChanged();
        void rectChanged();
        void radiusChanged();

      protected:
        bool eventFilter(QObject *watched, QEvent *event) override;

      private:
        void updateRegion();
        void clearRegion();
        bool applyX11Region(const QRegion &region, bool reset);

        QPointer<QWindow> m_window;
        QRectF m_rect;
        qreal m_radius = 0;
    };

}
