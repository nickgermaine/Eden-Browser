#pragma once

#include "engine/wpe/wpeapi.h"
#include <QPointer>
#include <QQuickItem>
#include <QRect>
#include <QSet>
#include <atomic>
#include <memory>

namespace eden::engine {

    struct WpeFrame {
        WpeFrame(WPEView *view, WPEBuffer *buffer);
        ~WpeFrame();
        void rendered();
        static int heldPeak();
        WPEView *view;
        WPEBuffer *buffer;
        QSize size;
        double scale;
        std::atomic<bool> acknowledged = false;
        std::atomic<int> releaseFence = -1;
        std::atomic<int> renderingFence = -1;
        GMainContext *mainContext;
    };

    enum class FrameAcknowledgement { Arrival, Import, Swap };

    FrameAcknowledgement frameAcknowledgementPolicy();
    QString frameAcknowledgementName(FrameAcknowledgement policy);

    class WpeRenderingNotice;

    class WpeSurface final : public QQuickItem {
        Q_OBJECT

      public:
        explicit WpeSurface(WPEView *view);
        ~WpeSurface() override;
        void attach(QQuickItem *viewport);
        bool submit(WPEBuffer *buffer);
        void buffersChanged(WPEBuffer **buffers, guint count);
        void synchronizeVisibility();
        void setPointerCursor(const QString &name);
        void setPointerCursor(const QImage &image, const QPoint &hotspot);
        void setInputContext(WPEInputMethodContext *context);
        void setInputFocus(bool focused);
        void setInputCursor(const QRect &rectangle);
        void setInputSurrounding(const char *text, unsigned length, unsigned cursor, unsigned anchor);
        void setInputPreedit(const QString &text, int cursor);
        QString inputPreedit() const;
        int inputPreeditCursor() const;
        void resetInput();
        QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
        WPEView *nativeView() const;
        QPoint pointerPosition() const;
        bool accelerated() const;
        QString renderingDiagnostic() const;
        quint64 textureImportCount() const;
        quint64 frameImportCount() const;

      signals:
        void renderingFailed(const QString &message);
        void shortcutRequested(const QString &command);
        void nativeClosed();
        void inspectorCommand(const QString &command);

      protected:
        QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
        void geometryChange(const QRectF &next, const QRectF &previous) override;
        void itemChange(ItemChange change, const ItemChangeData &data) override;
        void mousePressEvent(QMouseEvent *event) override;
        void mouseReleaseEvent(QMouseEvent *event) override;
        void mouseDoubleClickEvent(QMouseEvent *event) override;
        void mouseMoveEvent(QMouseEvent *event) override;
        void hoverMoveEvent(QHoverEvent *event) override;
        void hoverEnterEvent(QHoverEvent *event) override;
        void hoverLeaveEvent(QHoverEvent *event) override;
        void wheelEvent(QWheelEvent *event) override;
        void keyPressEvent(QKeyEvent *event) override;
        void keyReleaseEvent(QKeyEvent *event) override;
        void focusInEvent(QFocusEvent *event) override;
        void focusOutEvent(QFocusEvent *event) override;
        void inputMethodEvent(QInputMethodEvent *event) override;
        void touchEvent(QTouchEvent *event) override;

      private:
        void dispatch(WPEEvent *event);
        void pointerButton(QMouseEvent *event, bool pressed);
        void
        pointerMotion(const QPointF &position, unsigned long time, Qt::KeyboardModifiers modifiers, WPEEventType type);
        void keyEvent(QKeyEvent *event, bool pressed);
        void synchronizeGeometry();
        void synchronizeActivation();
        void reportRenderingFailure(const QString &message);
        WPEView *m_view;
        QPointer<QQuickItem> m_viewport;
        QPointer<QQuickWindow> m_window;
        QList<QMetaObject::Connection> m_viewportConnections;
        QList<QMetaObject::Connection> m_windowConnections;
        std::shared_ptr<WpeFrame> m_frame;
        QSet<const void *> m_liveBuffers;
        quint64 m_liveBuffersGeneration = 0;
        WPEInputMethodContext *m_input = nullptr;
        QPointF m_pointer;
        QRect m_cursor;
        QString m_surrounding;
        QByteArray m_surroundingBytes;
        unsigned m_surroundingCursorByte = 0;
        unsigned m_surroundingAnchorByte = 0;
        bool m_surroundingValid = false;
        QString m_preedit;
        int m_cursorPosition = 0;
        int m_anchorPosition = 0;
        int m_preeditCursor = 0;
        bool m_inputFocused = false;
        bool m_composing = false;
        bool m_accelerated = true;
        const FrameAcknowledgement m_acknowledgement;
        std::atomic<quint64> m_textureImports = 0;
        std::atomic<quint64> m_frameImports = 0;
        QString m_renderingDiagnostic;
        WpeRenderingNotice *m_renderingNotice = nullptr;
    };

}
