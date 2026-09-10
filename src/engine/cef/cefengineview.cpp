#include "engine/cef/cefengineview.h"
#if EDEN_ENABLE_AUTOMATION
#include "core/automation/performancemetrics.h"
#endif
#include "engine/cef/cefbrowsersettings.h"
#include "engine/cef/cefprofile.h"
#include "engine/cef/cefruntime.h"
#include "engine/cef/cefuibridge.h"
#include "engine/cef/devtoolssocketserver.h"

#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_dialog_handler.h"
#include "include/cef_display_handler.h"
#include "include/cef_download_handler.h"
#include "include/cef_focus_handler.h"
#include "include/cef_frame_handler.h"
#include "include/cef_image.h"
#include "include/cef_jsdialog_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_navigation_entry.h"
#include "include/cef_permission_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_request.h"
#include "include/cef_request_handler.h"
#include "include/cef_task.h"
#include "include/cef_x509_certificate.h"
#include "include/internal/cef_string_wrappers.h"
#include "include/internal/cef_types_wrappers.h"

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegion>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSslCertificate>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QWheelEvent>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>

#include <X11/Xlib.h>

#undef FocusIn
#undef FocusOut
#undef KeyPress
#undef KeyRelease

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace eden::engine::cef {

    class CefEngineClient;

    static QString cefString(const CefString &value) {
        return QString::fromStdString(value.ToString());
    }

    static bool isCurrentMainFrame(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) {
        if (!browser || !frame || !frame->IsMain()) {
            return false;
        }
        const CefRefPtr<CefFrame> mainFrame = browser->GetMainFrame();
        return mainFrame && mainFrame->GetIdentifier() == frame->GetIdentifier();
    }

    static QString certificateSerial(CefRefPtr<CefBinaryValue> value) {
        if (!value || value->GetSize() == 0) {
            return {};
        }
        QByteArray bytes(static_cast<qsizetype>(value->GetSize()), Qt::Uninitialized);
        const size_t copied = value->GetData(bytes.data(), value->GetSize(), 0);
        bytes.resize(static_cast<qsizetype>(copied));
        return QString::fromLatin1(bytes.toHex(':').toUpper());
    }

    static QByteArray binaryValueBytes(CefRefPtr<CefBinaryValue> value) {
        if (!value || value->GetSize() == 0) {
            return {};
        }
        QByteArray bytes(static_cast<qsizetype>(value->GetSize()), Qt::Uninitialized);
        bytes.resize(static_cast<qsizetype>(value->GetData(bytes.data(), value->GetSize(), 0)));
        return bytes;
    }

    static QVariantMap certificateChainEntry(const QByteArray &der) {
        const QSslCertificate certificate(der, QSsl::Der);
        if (certificate.isNull()) {
            return {};
        }
        const QString subject = certificate.subjectInfo(QSslCertificate::CommonName).join(", ");
        const QString issuer = certificate.issuerInfo(QSslCertificate::CommonName).join(", ");
        return {
            {"subject", subject},
            {"issuer", issuer},
            {"serialNumber", QString::fromLatin1(certificate.serialNumber())},
            {"validFrom", certificate.effectiveDate()},
            {"validUntil", certificate.expiryDate()},
            {"sha256", QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':').toUpper())},
        };
    }

    static QString sslVersionName(cef_ssl_version_t version) {
        switch (version) {
        case SSL_CONNECTION_VERSION_SSL2:
            return "SSL 2";
        case SSL_CONNECTION_VERSION_SSL3:
            return "SSL 3";
        case SSL_CONNECTION_VERSION_TLS1:
            return "TLS 1.0";
        case SSL_CONNECTION_VERSION_TLS1_1:
            return "TLS 1.1";
        case SSL_CONNECTION_VERSION_TLS1_2:
            return "TLS 1.2";
        case SSL_CONNECTION_VERSION_TLS1_3:
            return "TLS 1.3";
        case SSL_CONNECTION_VERSION_QUIC:
            return "QUIC";
        default:
            return "Unknown";
        }
    }

    static QDateTime cefBaseTimeDate(CefBaseTime value) {
        constexpr qint64 windowsToUnixSeconds = 11644473600;
        if (value.val == 0) {
            return {};
        }
        return QDateTime::fromSecsSinceEpoch(value.val / 1000000 - windowsToUnixSeconds);
    }

    static QVariantMap certificateDetails(CefRefPtr<CefBrowser> browser) {
        const CefRefPtr<CefNavigationEntry> entry = browser ? browser->GetHost()->GetVisibleNavigationEntry() : nullptr;
        const CefRefPtr<CefSSLStatus> status = entry ? entry->GetSSLStatus() : nullptr;
        const CefRefPtr<CefX509Certificate> certificate = status ? status->GetX509Certificate() : nullptr;
        if (!status || !certificate) {
            return {};
        }
        const CefRefPtr<CefX509CertPrincipal> subject = certificate->GetSubject();
        const CefRefPtr<CefX509CertPrincipal> issuer = certificate->GetIssuer();
        QVariantMap details;
        details.insert("secure", status->IsSecureConnection());
        details.insert("subject", subject ? cefString(subject->GetDisplayName()) : QString());
        details.insert("commonName", subject ? cefString(subject->GetCommonName()) : QString());
        details.insert("issuer", issuer ? cefString(issuer->GetDisplayName()) : QString());
        details.insert("serialNumber", certificateSerial(certificate->GetSerialNumber()));
        details.insert("validFrom", cefBaseTimeDate(certificate->GetValidStart()));
        details.insert("validUntil", cefBaseTimeDate(certificate->GetValidExpiry()));
        details.insert("protocol", sslVersionName(status->GetSSLVersion()));
        details.insert("certificateStatus", static_cast<qulonglong>(status->GetCertStatus()));
        details.insert("chainLength", static_cast<qulonglong>(certificate->GetIssuerChainSize() + 1));
        QVariantList chain;
        const QVariantMap leaf = certificateChainEntry(binaryValueBytes(certificate->GetDEREncoded()));
        if (!leaf.isEmpty()) {
            chain.append(leaf);
        }
        CefX509Certificate::IssuerChainBinaryList issuers;
        certificate->GetDEREncodedIssuerChain(issuers);
        for (const CefRefPtr<CefBinaryValue> &issuerCertificate : issuers) {
            const QVariantMap issuerEntry = certificateChainEntry(binaryValueBytes(issuerCertificate));
            if (!issuerEntry.isEmpty()) {
                chain.append(issuerEntry);
            }
        }
        details.insert("chain", chain);
        return details;
    }

    struct CefOsrFrame {
        CefOsrFrame() = default;
        CefOsrFrame(QImage image, CefRefPtr<CefEngineClient> owner, int stagingIndex);
        CefOsrFrame(const CefOsrFrame &) = delete;
        CefOsrFrame &operator=(const CefOsrFrame &) = delete;
        CefOsrFrame(CefOsrFrame &&other) noexcept;
        CefOsrFrame &operator=(CefOsrFrame &&other) noexcept;
        ~CefOsrFrame();

        bool isNull() const;
        void reset();

        QImage image;
        CefRefPtr<CefEngineClient> owner;
        int stagingIndex = -1;
    };

    class CefFunctionTask final : public CefTask {
      public:
        explicit CefFunctionTask(std::function<void()> function)
            : m_function(std::move(function)) {}

        void Execute() override {
            m_function();
        }

      private:
        std::function<void()> m_function;

        IMPLEMENT_REFCOUNTING(CefFunctionTask);
    };

    static int nextDevToolsMessageId() {
        static std::atomic_int counter{1000000};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    static QString &mirroredClipboardText() {
        static QString text;
        return text;
    }

    static void setMirroredClipboardText(const QString &text) {
        mirroredClipboardText() = text;
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(text);
        }
    }

    static QString contextMenuLabel(const CefString &label) {
        const QString source = QString::fromStdString(label.ToString());
        QString result;
        result.reserve(source.size());
        for (qsizetype index = 0; index < source.size(); ++index) {
            if (source.at(index) != '&') {
                result.append(source.at(index));
                continue;
            }
            if (index + 1 < source.size() && source.at(index + 1) == '&') {
                result.append('&');
                ++index;
            }
        }
        return result.trimmed();
    }

    static bool validCefFrameDimensions(int width, int height) {
        constexpr int maximumDimension = 32768;
        if (width <= 0 || height <= 0 || width > maximumDimension || height > maximumDimension) {
            return false;
        }
        const qsizetype stride = static_cast<qsizetype>(width) * 4;
        return stride > 0 && static_cast<qsizetype>(height) <= std::numeric_limits<qsizetype>::max() / stride;
    }

    static QImage copyCefFrame(const void *buffer, int width, int height) {
        if (!buffer || !validCefFrameDimensions(width, height)) {
            return {};
        }
        QImage frame(width, height, QImage::Format_ARGB32);
        const qsizetype rowBytes = static_cast<qsizetype>(width) * 4;
        if (frame.isNull() || frame.bytesPerLine() < rowBytes) {
            return {};
        }
        const auto *source = static_cast<const uchar *>(buffer);
        for (int row = 0; row < height; ++row) {
            std::memcpy(
                frame.scanLine(row),
                source + static_cast<qsizetype>(row) * rowBytes,
                static_cast<std::size_t>(rowBytes)
            );
        }
        return frame;
    }

    static void updateCefFrame(QImage &target, const void *buffer, const QRegion &dirtyRegion) {
        const auto *source = static_cast<const uchar *>(buffer);
        const qsizetype sourceStride = static_cast<qsizetype>(target.width()) * 4;
        uchar *targetBits = target.bits();
        const qsizetype targetStride = target.bytesPerLine();
        for (const QRect &rect : dirtyRegion.rects()) {
            const qsizetype rowBytes = static_cast<qsizetype>(rect.width()) * 4;
            const qsizetype columnOffset = static_cast<qsizetype>(rect.x()) * 4;
            for (int row = rect.top(); row <= rect.bottom(); ++row) {
                std::memcpy(
                    targetBits + static_cast<qsizetype>(row) * targetStride + columnOffset,
                    source + static_cast<qsizetype>(row) * sourceStride + columnOffset,
                    static_cast<std::size_t>(rowBytes)
                );
            }
        }
    }

    static QString contextMenuIcon(int commandId, const QString &title) {
        switch (commandId) {
        case MENU_ID_UNDO:
            return "undo";
        case MENU_ID_REDO:
            return "redo";
        case MENU_ID_CUT:
            return "cut";
        case MENU_ID_COPY:
            return "copy";
        case MENU_ID_PASTE:
            return "clipboard";
        case MENU_ID_PASTE_MATCH_STYLE:
            return "clipboard-text";
        case MENU_ID_DELETE:
            return "trash";
        case MENU_ID_SELECT_ALL:
            return "text-selection";
        default:
            break;
        }
        const QString value = title.toLower();
        if (value.contains("copy object")) {
            return "object-scan";
        }
        if (value.contains("copy value") || value.contains("plain text")) {
            return "clipboard-text";
        }
        if (value.contains("copy property") || value.contains("path") || value.contains("link")) {
            return "link";
        }
        if (value.contains("copy")) {
            return "copy";
        }
        if (value.contains("paste")) {
            return "clipboard";
        }
        if (value.contains("expand") || value.contains("maximize")) {
            return "expand";
        }
        if (value.contains("collapse") || value.contains("minimize")) {
            return "collapse";
        }
        if (value.contains("capture") || value.contains("screenshot")) {
            return "camera";
        }
        if (value.contains("focus") || value.contains("select")) {
            return "text-focus";
        }
        if (value.contains("sort")) {
            return "sort";
        }
        if (value.contains("filter")) {
            return "filter";
        }
        if (value.contains("global variable") || value.contains("global")) {
            return "globe";
        }
        if (value.contains("watch") || value.startsWith("show ") || value.startsWith("hide ")) {
            return "eye";
        }
        if (value.contains("property") || value.contains("attribute") || value.contains("settings")) {
            return "tuning";
        }
        if (value.contains("source") || value.contains("code") || value.contains("script")) {
            return "code";
        }
        if (value.contains("delete") || value.contains("remove") || value.contains("clear")) {
            return "trash";
        }
        if (value.contains("edit")) {
            return "gallery-edit";
        }
        if (value.contains("add")) {
            return "add";
        }
        if (value.contains("list") || value.contains("header")) {
            return "list";
        }
        if (value.contains("document") || value.contains("save")) {
            return "document-text";
        }
        return "menu-dots";
    }

    static bool postToCefUi(std::function<void()> function) {
        if (!function) {
            return false;
        }
        if (CefCurrentlyOn(TID_UI)) {
            function();
            return true;
        }
        return CefPostTask(TID_UI, new CefFunctionTask(std::move(function)));
    }

    static bool windowContains(Display *display, Window ancestor, Window candidate) {
        if (!display || !ancestor || !candidate) {
            return false;
        }
        Window current = candidate;
        while (current) {
            if (current == ancestor) {
                return true;
            }
            Window root = 0;
            Window parent = 0;
            Window *children = nullptr;
            unsigned int childCount = 0;
            if (!XQueryTree(display, current, &root, &parent, &children, &childCount)) {
                return false;
            }
            if (children) {
                XFree(children);
            }
            if (!parent || parent == current) {
                return false;
            }
            current = parent;
        }
        return false;
    }

    static uint32_t cefEventModifiers(Qt::KeyboardModifiers modifiers) {
        uint32_t result = 0;
        if (modifiers & Qt::ShiftModifier) {
            result |= EVENTFLAG_SHIFT_DOWN;
        }
        if (modifiers & Qt::ControlModifier) {
            result |= EVENTFLAG_CONTROL_DOWN;
        }
        if (modifiers & Qt::AltModifier) {
            result |= EVENTFLAG_ALT_DOWN;
        }
        if (modifiers & Qt::MetaModifier) {
            result |= EVENTFLAG_COMMAND_DOWN;
        }
        return result;
    }

    static uint32_t cefMouseModifiers(Qt::KeyboardModifiers keyboardModifiers, Qt::MouseButtons buttons) {
        uint32_t result = cefEventModifiers(keyboardModifiers);
        if (buttons & Qt::LeftButton) {
            result |= EVENTFLAG_LEFT_MOUSE_BUTTON;
        }
        if (buttons & Qt::MiddleButton) {
            result |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
        }
        if (buttons & Qt::RightButton) {
            result |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
        }
        return result;
    }

    static int cefWindowsKeyCode(int key) {
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            return 'A' + key - Qt::Key_A;
        }
        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            return '0' + key - Qt::Key_0;
        }
        if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
            return 112 + key - Qt::Key_F1;
        }
        switch (key) {
        case Qt::Key_Exclam:
            return '1';
        case Qt::Key_At:
            return '2';
        case Qt::Key_NumberSign:
            return '3';
        case Qt::Key_Dollar:
            return '4';
        case Qt::Key_Percent:
            return '5';
        case Qt::Key_AsciiCircum:
            return '6';
        case Qt::Key_Ampersand:
            return '7';
        case Qt::Key_Asterisk:
            return '8';
        case Qt::Key_ParenLeft:
            return '9';
        case Qt::Key_ParenRight:
            return '0';
        case Qt::Key_Underscore:
            return 189;
        case Qt::Key_Plus:
            return 187;
        case Qt::Key_BraceLeft:
            return 219;
        case Qt::Key_BraceRight:
            return 221;
        case Qt::Key_Bar:
            return 220;
        case Qt::Key_Colon:
            return 186;
        case Qt::Key_QuoteDbl:
            return 222;
        case Qt::Key_Less:
            return 188;
        case Qt::Key_Greater:
            return 190;
        case Qt::Key_Question:
            return 191;
        case Qt::Key_AsciiTilde:
            return 192;
        case Qt::Key_Backspace:
            return 8;
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return 9;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return 13;
        case Qt::Key_Escape:
            return 27;
        case Qt::Key_Space:
            return 32;
        case Qt::Key_PageUp:
            return 33;
        case Qt::Key_PageDown:
            return 34;
        case Qt::Key_End:
            return 35;
        case Qt::Key_Home:
            return 36;
        case Qt::Key_Left:
            return 37;
        case Qt::Key_Up:
            return 38;
        case Qt::Key_Right:
            return 39;
        case Qt::Key_Down:
            return 40;
        case Qt::Key_Insert:
            return 45;
        case Qt::Key_Delete:
            return 46;
        case Qt::Key_Semicolon:
            return 186;
        case Qt::Key_Equal:
            return 187;
        case Qt::Key_Comma:
            return 188;
        case Qt::Key_Minus:
            return 189;
        case Qt::Key_Period:
            return 190;
        case Qt::Key_Slash:
            return 191;
        case Qt::Key_QuoteLeft:
            return 192;
        case Qt::Key_BracketLeft:
            return 219;
        case Qt::Key_Backslash:
            return 220;
        case Qt::Key_BracketRight:
            return 221;
        case Qt::Key_Apostrophe:
            return 222;
        default:
            return key;
        }
    }

    struct CefViewLifetime {
        void bind(void *nextState) {
            std::deque<std::function<void(void *)>> queued;
            {
                const std::lock_guard lock(mutex);
                if (closed) {
                    return;
                }
                state = nextState;
                queued.swap(pending);
            }
            for (auto &function : queued) {
                function(nextState);
            }
        }

        void close() {
            const std::lock_guard lock(mutex);
            state = nullptr;
            closed = true;
            pending.clear();
        }

        void runOrQueue(std::function<void(void *)> function) {
            void *currentState = nullptr;
            {
                const std::lock_guard lock(mutex);
                if (closed) {
                    return;
                }
                currentState = state;
                if (!currentState) {
                    pending.push_back(std::move(function));
                    return;
                }
            }
            function(currentState);
        }

        std::mutex mutex;
        void *state = nullptr;
        bool closed = false;
        std::deque<std::function<void(void *)>> pending;
    };

    class CefPopupNewViewRequest final : public EngineNewViewRequest {
      public:
        CefPopupNewViewRequest(
            const QUrl &url,
            Disposition disposition,
            bool userInitiated,
            std::shared_ptr<CefPopupTransfer> transfer
        )
            : EngineNewViewRequest(url, disposition, userInitiated),
              m_transfer(std::move(transfer)) {}

        ~CefPopupNewViewRequest() override;

        bool openIn(EngineView *target) override;

      private:
        std::shared_ptr<CefPopupTransfer> m_transfer;
        bool m_opened = false;
    };

    class CefOsrTextureNode final : public QSGSimpleTextureNode {
      public:
        void replaceTexture(QSGTexture *nextTexture) {
            QSGTexture *previousTexture = texture();
            setOwnsTexture(false);
            setTexture(nextTexture);
            setOwnsTexture(true);
            delete previousTexture;
        }
    };

    struct CefOsrPopupFrame {
        QRect bounds;
        QImage image;
        bool visible = false;
    };

    class CefOsrPopupQueue final {
      public:
        bool setVisible(bool visible) {
            const std::lock_guard lock(m_mutex);
            if (m_frame.visible == visible) {
                return false;
            }
            m_frame.visible = visible;
            if (!visible) {
                m_frame.bounds = {};
                m_frame.image = {};
            }
            return schedule();
        }

        bool setBounds(const CefRect &bounds) {
            const QRect next(bounds.x, bounds.y, bounds.width, bounds.height);
            const std::lock_guard lock(m_mutex);
            if (m_frame.bounds == next) {
                return false;
            }
            if (m_frame.bounds.size() != next.size()) {
                m_frame.image = {};
            }
            m_frame.bounds = next;
            return schedule();
        }

        bool paint(const void *buffer, int width, int height) {
            {
                const std::lock_guard lock(m_mutex);
                if (!m_frame.visible) {
                    return false;
                }
            }
            QImage image = copyCefFrame(buffer, width, height);
            if (image.isNull()) {
                return false;
            }
            image.reinterpretAsFormat(QImage::Format_ARGB32_Premultiplied);
            const std::lock_guard lock(m_mutex);
            m_frame.image = std::move(image);
            return schedule();
        }

        CefOsrPopupFrame take() {
            const std::lock_guard lock(m_mutex);
            m_queued = false;
            return m_frame;
        }

      private:
        bool schedule() {
            return !std::exchange(m_queued, true);
        }

        std::mutex m_mutex;
        CefOsrPopupFrame m_frame;
        bool m_queued = false;
    };

    class CefOsrItem final : public QQuickItem {
      public:
        explicit CefOsrItem(QQuickItem *parent)
            : QQuickItem(parent) {
            setFlag(ItemHasContents, true);
            setAcceptedMouseButtons(Qt::AllButtons);
            setAcceptHoverEvents(true);
            setActiveFocusOnTab(true);
            setFocusPolicy(Qt::StrongFocus);
        }

        void setEventHandler(std::function<bool(QEvent *)> handler) {
            m_eventHandler = std::move(handler);
        }

        void presentFrame(CefOsrFrame frame) {
            m_pendingFrame = std::move(frame);
            update();
        }

        void presentFrame(QImage frame) {
            m_pendingFrame = CefOsrFrame(std::move(frame), nullptr, -1);
            update();
        }

        void presentPopup(CefOsrPopupFrame frame) {
            m_popupFrame = std::move(frame);
            m_popupChanged = true;
            update();
        }

        QPoint browserPosition(const QPointF &position) const {
            const QRectF displayedBounds = popupBounds();
            if (m_popupFrame.visible && displayedBounds.contains(position)) {
                return (position + m_popupFrame.bounds.topLeft() - displayedBounds.topLeft()).toPoint();
            }
            return position.toPoint();
        }

      protected:
        void focusInEvent(QFocusEvent *event) override {
            m_eventHandler(event);
            QQuickItem::focusInEvent(event);
        }

        void focusOutEvent(QFocusEvent *event) override {
            m_eventHandler(event);
            QQuickItem::focusOutEvent(event);
        }

        void mouseMoveEvent(QMouseEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        void mousePressEvent(QMouseEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        void mouseReleaseEvent(QMouseEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        void mouseDoubleClickEvent(QMouseEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        void hoverMoveEvent(QHoverEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        void hoverLeaveEvent(QHoverEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        void wheelEvent(QWheelEvent *event) override {
            event->setAccepted(m_eventHandler(event));
        }

        QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override {
            if (!window() || (m_pendingFrame.isNull() && m_displayedFrame.isNull())) {
                delete oldNode;
                m_displayedFrame.reset();
                return nullptr;
            }
            auto *node = static_cast<CefOsrTextureNode *>(oldNode);
            if (!node) {
                node = new CefOsrTextureNode;
                node->setFiltering(QSGTexture::Linear);
            }
            node->setRect(boundingRect());
            if (!m_pendingFrame.isNull()) {
                node->replaceTexture(window()->createTextureFromImage(m_pendingFrame.image));
                m_displayedFrame = std::move(m_pendingFrame);
            } else if (!node->texture()) {
                node->replaceTexture(window()->createTextureFromImage(m_displayedFrame.image));
            }
            auto *popup = static_cast<CefOsrTextureNode *>(node->firstChild());
            if (!m_popupFrame.visible || m_popupFrame.image.isNull() || m_popupFrame.bounds.isEmpty()) {
                delete popup;
            } else {
                if (!popup) {
                    popup = new CefOsrTextureNode;
                    popup->setFiltering(QSGTexture::Linear);
                    node->appendChildNode(popup);
                    m_popupChanged = true;
                }
                if (m_popupChanged) {
                    popup->replaceTexture(window()->createTextureFromImage(m_popupFrame.image));
                }
                const QRectF bounds = popupBounds();
                const QRectF visibleBounds = bounds.intersected(boundingRect());
                popup->setRect(visibleBounds);
                popup->setSourceRect(QRectF(
                    0,
                    0,
                    m_popupFrame.image.width() * visibleBounds.width() / bounds.width(),
                    m_popupFrame.image.height() * visibleBounds.height() / bounds.height()
                ));
            }
            m_popupChanged = false;
            return node;
        }

      private:
        QRectF popupBounds() const {
            QRectF bounds(m_popupFrame.bounds);
            bounds.moveLeft(std::clamp(bounds.left(), 0.0, std::max(0.0, width() - bounds.width())));
            bounds.moveTop(std::clamp(bounds.top(), 0.0, std::max(0.0, height() - bounds.height())));
            return bounds;
        }

        std::function<bool(QEvent *)> m_eventHandler;
        CefOsrFrame m_pendingFrame;
        CefOsrFrame m_displayedFrame;
        CefOsrPopupFrame m_popupFrame;
        bool m_popupChanged = false;
    };

    class CefDevToolsProtocolSession;

    class CefThumbnailObserver final : public CefDevToolsMessageObserver {
      public:
        CefThumbnailObserver(QSize size, EngineView::ThumbnailCallback callback)
            : m_size(std::move(size)),
              m_callback(std::move(callback)) {}

        static void capture(CefRefPtr<CefBrowser> browser, const QSize &size, EngineView::ThumbnailCallback callback) {
            if (!browser || !callback || !size.isValid()) {
                CefUiBridge::runOnUiThread([callback = std::move(callback)]() mutable {
                    if (callback) {
                        callback({});
                    }
                });
                return;
            }
            CefRefPtr<CefThumbnailObserver> observer = new CefThumbnailObserver(size, std::move(callback));
            observer->start(browser);
        }

        void OnDevToolsMethodResult(
            CefRefPtr<CefBrowser>,
            int messageId,
            bool success,
            const void *result,
            size_t resultSize
        ) override {
            if (messageId != m_messageId) {
                return;
            }
            QByteArray payload;
            if (success && result && resultSize > 0) {
                payload = QByteArray(static_cast<const char *>(result), static_cast<qsizetype>(resultSize));
            }
            m_registration = nullptr;
            EngineView::ThumbnailCallback callback = std::move(m_callback);
            const QSize size = m_size;
            QThreadPool::globalInstance()->start(
                [payload = std::move(payload), size, callback = std::move(callback)]() mutable {
                    QImage image;
                    if (!payload.isEmpty()) {
                        const QByteArray encoded =
                            QJsonDocument::fromJson(payload).object().value("data").toString().toLatin1();
                        image.loadFromData(QByteArray::fromBase64(encoded));
                        if (!image.isNull() && image.size() != size) {
                            image = image.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                            if (image.width() > size.width() || image.height() > size.height()) {
                                image = image.copy(
                                    (image.width() - size.width()) / 2,
                                    (image.height() - size.height()) / 2,
                                    size.width(),
                                    size.height()
                                );
                            }
                        }
                    }
                    CefUiBridge::runOnUiThread([callback = std::move(callback), image = std::move(image)]() mutable {
                        if (callback) {
                            callback(image);
                        }
                    });
                }
            );
        }

      private:
        void start(CefRefPtr<CefBrowser> browser) {
            m_messageId = nextDevToolsMessageId();
            m_registration = browser->GetHost()->AddDevToolsMessageObserver(this);
            CefRefPtr<CefDictionaryValue> parameters = CefDictionaryValue::Create();
            parameters->SetString("format", "jpeg");
            parameters->SetInt("quality", 70);
            parameters->SetBool("fromSurface", true);
            parameters->SetBool("captureBeyondViewport", false);
            if (!m_registration ||
                browser->GetHost()->ExecuteDevToolsMethod(m_messageId, "Page.captureScreenshot", parameters) == 0) {
                finish({});
            }
        }

        void finish(QImage image) {
            m_registration = nullptr;
            EngineView::ThumbnailCallback callback = std::move(m_callback);
            CefUiBridge::runOnUiThread([callback = std::move(callback), image = std::move(image)]() mutable {
                if (callback) {
                    callback(image);
                }
            });
        }

        QSize m_size;
        EngineView::ThumbnailCallback m_callback;
        CefRefPtr<CefRegistration> m_registration;
        int m_messageId = 0;

        IMPLEMENT_REFCOUNTING(CefThumbnailObserver);
    };

    class CefNavigationHistoryObserver final : public CefDevToolsMessageObserver {
      public:
        using HistoryCallback = std::function<void(int currentIndex, QVariantList entries)>;

        static void capture(CefRefPtr<CefBrowser> browser, HistoryCallback callback) {
            if (!browser || !callback) {
                return;
            }
            CefRefPtr<CefNavigationHistoryObserver> observer = new CefNavigationHistoryObserver(std::move(callback));
            observer->start(browser);
        }

        void OnDevToolsMethodResult(
            CefRefPtr<CefBrowser>,
            int messageId,
            bool success,
            const void *result,
            size_t resultSize
        ) override {
            if (messageId != m_messageId) {
                return;
            }
            m_registration = nullptr;
            if (!success || !result || resultSize == 0) {
                return;
            }
            const QByteArray payload(static_cast<const char *>(result), static_cast<qsizetype>(resultSize));
            const QJsonObject history = QJsonDocument::fromJson(payload).object();
            const int currentIndex = history.value("currentIndex").toInt(-1);
            QVariantList entries;
            const QJsonArray items = history.value("entries").toArray();
            for (const QJsonValue &item : items) {
                const QJsonObject entry = item.toObject();
                QVariantMap value;
                value.insert("entryId", entry.value("id").toVariant().toLongLong());
                value.insert("url", entry.value("url").toString());
                value.insert("title", entry.value("title").toString());
                entries.append(value);
            }
            HistoryCallback callback = std::move(m_callback);
            CefUiBridge::runOnUiThread(
                [callback = std::move(callback), currentIndex, entries = std::move(entries)]() mutable {
                    if (callback) {
                        callback(currentIndex, std::move(entries));
                    }
                }
            );
        }

      private:
        explicit CefNavigationHistoryObserver(HistoryCallback callback)
            : m_callback(std::move(callback)) {}

        void start(CefRefPtr<CefBrowser> browser) {
            m_messageId = nextDevToolsMessageId();
            m_registration = browser->GetHost()->AddDevToolsMessageObserver(this);
            if (!m_registration || browser->GetHost()->ExecuteDevToolsMethod(
                                       m_messageId,
                                       "Page.getNavigationHistory",
                                       CefDictionaryValue::Create()
                                   ) == 0) {
                m_registration = nullptr;
            }
        }

        HistoryCallback m_callback;
        CefRefPtr<CefRegistration> m_registration;
        int m_messageId = 0;

        IMPLEMENT_REFCOUNTING(CefNavigationHistoryObserver);
    };

    class CefFileChooserObserver final : public CefDevToolsMessageObserver {
      public:
        explicit CefFileChooserObserver(std::shared_ptr<CefViewLifetime> lifetime)
            : m_lifetime(std::move(lifetime)) {}

        void OnDevToolsEvent(
            CefRefPtr<CefBrowser> browser,
            const CefString &method,
            const void *parameters,
            size_t parametersSize
        ) override;

      private:
        std::shared_ptr<CefViewLifetime> m_lifetime;

        IMPLEMENT_REFCOUNTING(CefFileChooserObserver);
    };

    class CefDevToolsMessageForwarder final : public CefDevToolsMessageObserver {
      public:
        explicit CefDevToolsMessageForwarder(std::weak_ptr<CefDevToolsProtocolSession> session)
            : m_session(std::move(session)) {}

        bool OnDevToolsMessage(CefRefPtr<CefBrowser> browser, const void *message, size_t messageSize) override;

      private:
        std::weak_ptr<CefDevToolsProtocolSession> m_session;

        IMPLEMENT_REFCOUNTING(CefDevToolsMessageForwarder);
    };

    class CefDevToolsProtocolSession final : public std::enable_shared_from_this<CefDevToolsProtocolSession> {
      public:
        explicit CefDevToolsProtocolSession(CefRefPtr<CefBrowser> browser)
            : m_browser(std::move(browser)) {}

        bool start(const QString &token, std::shared_ptr<CefViewLifetime> lifetime);
        void sendToInspected(QByteArray message);
        void deliverToFrontend(QByteArray message);
        void close();

      private:
        CefRefPtr<CefBrowser> m_browser;
        CefRefPtr<CefDevToolsMessageForwarder> m_observer;
        CefRefPtr<CefRegistration> m_registration;
        QString m_token;
        std::atomic_bool m_closed = false;
    };

    class CefEngineClient;

    class CefDevToolsClient final : public CefClient,
                                    public CefContextMenuHandler,
                                    public CefDialogHandler,
                                    public CefDisplayHandler,
                                    public CefDownloadHandler,
                                    public CefFocusHandler,
                                    public CefKeyboardHandler,
                                    public CefLifeSpanHandler,
                                    public CefLoadHandler,
                                    public CefRenderHandler {
      public:
        CefDevToolsClient(std::shared_ptr<CefViewLifetime> lifetime, CefEngineClient *owner)
            : m_lifetime(std::move(lifetime)),
              m_owner(owner) {}

        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
            return this;
        }

        CefRefPtr<CefDialogHandler> GetDialogHandler() override {
            return this;
        }

        CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
            return this;
        }

        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
            return this;
        }

        CefRefPtr<CefLoadHandler> GetLoadHandler() override {
            return this;
        }

        CefRefPtr<CefFocusHandler> GetFocusHandler() override {
            return this;
        }

        CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
            return this;
        }

        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
            return this;
        }

        CefRefPtr<CefRenderHandler> GetRenderHandler() override {
            return this;
        }

        void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
        void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect &rect) override;
        void publishPopup();
        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
        bool OnCursorChange(
            CefRefPtr<CefBrowser> browser,
            CefCursorHandle cursor,
            cef_cursor_type_t type,
            const CefCursorInfo &customCursorInfo
        ) override;
        void OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) override;
        void OnGotFocus(CefRefPtr<CefBrowser> browser) override;
        bool OnPreKeyEvent(
            CefRefPtr<CefBrowser> browser,
            const CefKeyEvent &event,
            CefEventHandle osEvent,
            bool *isKeyboardShortcut
        ) override;
        bool RunContextMenu(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefRefPtr<CefContextMenuParams> params,
            CefRefPtr<CefMenuModel> model,
            CefRefPtr<CefRunContextMenuCallback> callback
        ) override;
        void OnContextMenuDismissed(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) override;
        bool OnBeforeDownload(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefDownloadItem> downloadItem,
            const CefString &suggestedName,
            CefRefPtr<CefBeforeDownloadCallback> callback
        ) override;
        void OnDownloadUpdated(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefDownloadItem> downloadItem,
            CefRefPtr<CefDownloadItemCallback> callback
        ) override;
        bool OnFileDialog(
            CefRefPtr<CefBrowser> browser,
            FileDialogMode mode,
            const CefString &title,
            const CefString &defaultFilePath,
            const std::vector<CefString> &acceptFilters,
            const std::vector<CefString> &acceptExtensions,
            const std::vector<CefString> &acceptDescriptions,
            CefRefPtr<CefFileDialogCallback> callback
        ) override;
        void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
        bool OnProcessMessageReceived(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefProcessId sourceProcess,
            CefRefPtr<CefProcessMessage> message
        ) override;
        bool GetRootWindowScreenRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override;
        void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override;
        bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screenInfo) override;
        void OnTextSelectionChanged(
            CefRefPtr<CefBrowser> browser,
            const CefString &selectedText,
            const CefRange &selectedRange
        ) override;
        void OnPaint(
            CefRefPtr<CefBrowser> browser,
            PaintElementType type,
            const RectList &dirtyRects,
            const void *buffer,
            int width,
            int height
        ) override;

        bool beginBrowserCreation();
        bool shouldCreateBrowser();
        void browserCreationFailed();
        void requestClose();
        void requestCloseOnCefUi();
        void finishBrowserClose();
        void setGeometry(const CefRect &rootWindowScreenRect, const CefRect &viewRect, float deviceScaleFactor);
        void executeContextMenuCommand(const QString &command);
        void dismissContextMenu();

      private:
        template <typename Function> bool dispatch(Function function) {
            const std::shared_ptr<CefViewLifetime> lifetime = m_lifetime;
            return CefUiBridge::runOnUiThread([lifetime, function = std::move(function)]() mutable {
                lifetime->runOrQueue([function = std::move(function)](void *state) mutable {
                    function(*static_cast<CefEngineView::Private *>(state));
                });
            });
        }

        void enqueueFrame(QImage frame);
        QVariantList contextMenuActions(CefRefPtr<CefMenuModel> model);

        std::shared_ptr<CefViewLifetime> m_lifetime;
        std::shared_ptr<CefOsrPopupQueue> m_popup = std::make_shared<CefOsrPopupQueue>();
        CefRefPtr<CefEngineClient> m_owner;
        mutable std::mutex m_browserMutex;
        CefRefPtr<CefBrowser> m_browser;
        bool m_creationPending = false;
        bool m_closeRequested = false;
        bool m_closeIssued = false;
        std::mutex m_screenRectMutex;
        CefRect m_rootWindowScreenRect;
        CefRect m_osrViewRect{0, 0, 1, 1};
        float m_deviceScaleFactor = 1.0F;
        std::mutex m_frameMutex;
        QImage m_pendingFrame;
        bool m_frameDeliveryQueued = false;
        CefRefPtr<CefRunContextMenuCallback> m_contextMenuCallback;
        QHash<QString, int> m_contextMenuCommands;
        QString m_selectedText;
        QString m_contextMenuSelectionText;

        IMPLEMENT_REFCOUNTING(CefDevToolsClient);
    };

    class CefEngineClient final : public CefClient,
                                  public CefContextMenuHandler,
                                  public CefDialogHandler,
                                  public CefDisplayHandler,
                                  public CefDownloadHandler,
                                  public CefFocusHandler,
                                  public CefFrameHandler,
                                  public CefJSDialogHandler,
                                  public CefKeyboardHandler,
                                  public CefLifeSpanHandler,
                                  public CefLoadHandler,
                                  public CefPermissionHandler,
                                  public CefRenderHandler,
                                  public CefRequestHandler {
      public:
        CefEngineClient(std::shared_ptr<CefViewLifetime> lifetime, QString downloadDirectory)
            : m_lifetime(std::move(lifetime)),
              m_downloadDirectory(std::move(downloadDirectory)) {}

        CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
            return this;
        }

        CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
            return this;
        }

        CefRefPtr<CefDialogHandler> GetDialogHandler() override {
            return this;
        }

        CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
            return this;
        }

        CefRefPtr<CefFocusHandler> GetFocusHandler() override {
            return this;
        }

        CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
            return this;
        }

        CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override {
            return this;
        }

        CefRefPtr<CefFrameHandler> GetFrameHandler() override {
            return this;
        }

        CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
            return this;
        }

        CefRefPtr<CefLoadHandler> GetLoadHandler() override {
            return this;
        }

        CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
            return this;
        }

        CefRefPtr<CefRenderHandler> GetRenderHandler() override {
            return this;
        }

        CefRefPtr<CefRequestHandler> GetRequestHandler() override {
            return this;
        }

        void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
        void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect &rect) override;
        void publishPopup();
        void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
        void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
        void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &url) override;
        void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title) override;
        void OnFaviconURLChange(CefRefPtr<CefBrowser> browser, const std::vector<CefString> &iconUrls) override;
        void OnLoadingProgressChange(CefRefPtr<CefBrowser> browser, double progress) override;
        bool OnCursorChange(
            CefRefPtr<CefBrowser> browser,
            CefCursorHandle cursor,
            cef_cursor_type_t type,
            const CefCursorInfo &customCursorInfo
        ) override;
        bool RunContextMenu(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefRefPtr<CefContextMenuParams> params,
            CefRefPtr<CefMenuModel> model,
            CefRefPtr<CefRunContextMenuCallback> callback
        ) override;
        void OnContextMenuDismissed(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) override;
        bool OnBeforeDownload(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefDownloadItem> downloadItem,
            const CefString &suggestedName,
            CefRefPtr<CefBeforeDownloadCallback> callback
        ) override;
        void OnDownloadUpdated(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefDownloadItem> downloadItem,
            CefRefPtr<CefDownloadItemCallback> callback
        ) override;
        bool GetRootWindowScreenRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override;
        void OnFullscreenModeChange(CefRefPtr<CefBrowser> browser, bool fullscreen) override;
        void OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) override;
        void OnGotFocus(CefRefPtr<CefBrowser> browser) override;
        bool OnPreKeyEvent(
            CefRefPtr<CefBrowser> browser,
            const CefKeyEvent &event,
            CefEventHandle osEvent,
            bool *isKeyboardShortcut
        ) override;
        void
        OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool loading, bool canGoBack, bool canGoForward) override;
        bool OnBeforeBrowse(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefRefPtr<CefRequest> request,
            bool userGesture,
            bool isRedirect
        ) override;
        void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition) override;
        void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;
        void OnLoadError(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            ErrorCode errorCode,
            const CefString &errorText,
            const CefString &failedUrl
        ) override;
        bool OnProcessMessageReceived(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            CefProcessId sourceProcess,
            CefRefPtr<CefProcessMessage> message
        ) override;
        void OnMainFrameChanged(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> oldFrame,
            CefRefPtr<CefFrame> newFrame
        ) override;
        void OnFrameDestroyed(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame) override;
        bool OnBeforePopup(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            int popupId,
            const CefString &targetUrl,
            const CefString &targetFrameName,
            CefLifeSpanHandler::WindowOpenDisposition targetDisposition,
            bool userGesture,
            const CefPopupFeatures &popupFeatures,
            CefWindowInfo &windowInfo,
            CefRefPtr<CefClient> &client,
            CefBrowserSettings &settings,
            CefRefPtr<CefDictionaryValue> &extraInfo,
            bool *noJavascriptAccess
        ) override;
        void OnBeforePopupAborted(CefRefPtr<CefBrowser> browser, int popupId) override;
        bool OnJSDialog(
            CefRefPtr<CefBrowser> browser,
            const CefString &originUrl,
            JSDialogType dialogType,
            const CefString &messageText,
            const CefString &defaultPromptText,
            CefRefPtr<CefJSDialogCallback> callback,
            bool &suppressMessage
        ) override;
        bool OnBeforeUnloadDialog(
            CefRefPtr<CefBrowser> browser,
            const CefString &messageText,
            bool isReload,
            CefRefPtr<CefJSDialogCallback> callback
        ) override;
        void OnResetDialogState(CefRefPtr<CefBrowser> browser) override;
        bool OnRequestMediaAccessPermission(
            CefRefPtr<CefBrowser> browser,
            CefRefPtr<CefFrame> frame,
            const CefString &requestingOrigin,
            uint32_t requestedPermissions,
            CefRefPtr<CefMediaAccessCallback> callback
        ) override;
        bool OnShowPermissionPrompt(
            CefRefPtr<CefBrowser> browser,
            uint64_t promptId,
            const CefString &requestingOrigin,
            uint32_t requestedPermissions,
            CefRefPtr<CefPermissionPromptCallback> callback
        ) override;
        void OnDismissPermissionPrompt(
            CefRefPtr<CefBrowser> browser,
            uint64_t promptId,
            cef_permission_request_result_t result
        ) override;
        bool OnFileDialog(
            CefRefPtr<CefBrowser> browser,
            FileDialogMode mode,
            const CefString &title,
            const CefString &defaultFilePath,
            const std::vector<CefString> &acceptFilters,
            const std::vector<CefString> &acceptExtensions,
            const std::vector<CefString> &acceptDescriptions,
            CefRefPtr<CefFileDialogCallback> callback
        ) override;
        void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override;
        bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screenInfo) override;
        void OnPaint(
            CefRefPtr<CefBrowser> browser,
            PaintElementType type,
            const RectList &dirtyRects,
            const void *buffer,
            int width,
            int height
        ) override;
        void OnAcceleratedPaint(
            CefRefPtr<CefBrowser> browser,
            PaintElementType type,
            const RectList &dirtyRects,
            const CefAcceleratedPaintInfo &info
        ) override;
        void OnRenderProcessTerminated(
            CefRefPtr<CefBrowser> browser,
            TerminationStatus status,
            int errorCode,
            const CefString &errorString
        ) override;

        bool beginBrowserCreation();
        bool shouldCreateBrowser();
        void browserCreationFailed();
        void requestClose();
        void requestCloseOnCefUi();
        void finishBrowserClose();
        CefRefPtr<CefBrowser> browserSnapshot() const;
        void setRootWindowScreenRect(const CefRect &rect);
        void setOsrGeometry(const CefRect &rect, float deviceScaleFactor);
        void executeContextMenuCommand(const QString &command);
        void dismissContextMenu();
        void resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text);
        void resolvePermissionRequest(quint64 id, cef_permission_request_result_t result);
        void resolveDisplayCaptureRequest(quint64 id, const QString &source);
        void resolveFileDialog(quint64 id, bool accepted, const std::vector<CefString> &files);
        void faviconDownloaded(CefRefPtr<CefBrowser> browser, quint64 serial, CefRefPtr<CefImage> image);
        void retireWindow(QWindow *window);
        void setPopupSource(CefRefPtr<CefEngineClient> source, int popupId);
        void popupCreated(int popupId);
        void abortPopupCreation();
        void detachPopupSource();

      private:
        friend class CefDevToolsClient;
        friend struct CefOsrFrame;

        template <typename Function> bool dispatch(Function function) {
            const std::shared_ptr<CefViewLifetime> lifetime = m_lifetime;
            return CefUiBridge::runOnUiThread([lifetime, function = std::move(function)]() mutable {
                lifetime->runOrQueue([function = std::move(function)](void *state) mutable {
                    function(*static_cast<CefEngineView::Private *>(state));
                });
            });
        }

        static QString shortcutFor(const CefKeyEvent &event);
        static QStringList permissionNames(uint32_t permissions, bool media);
        static QString sanitizedDownloadName(const QString &suggestedName);
        void cancelInteractions();
        struct DownloadRecord {
            QString fileName;
            QUrl sourceUrl;
            QString targetPath;
            qint64 receivedBytes = 0;
            qint64 totalBytes = -1;
            QString state = "downloading";
            bool beforeDownloadSeen = false;
            bool targetAvailable = true;
        };
        DownloadRecord &downloadRecord(CefRefPtr<CefDownloadItem> downloadItem, const QString &suggestedName = {});
        void publishDownload(uint32_t id, DownloadRecord &download);
        void enqueueFrame(CefOsrFrame frame);
        void enqueueCopiedFrame(const void *buffer, const QSize &size, const QRegion &dirtyRegion);
        void dispatchPendingFrame();
        void releaseStagingFrame(int index);
        void requestFaviconCandidate(CefRefPtr<CefBrowser> browser, quint64 serial);
        void publishNavigationState();
        void scheduleRendererGarbageCollection();

        quint64 m_navigationGeneration = 0;
        bool m_mainDocumentLoading = false;
        bool m_mainDocumentCommitted = false;
        bool m_canGoBack = false;
        bool m_canGoForward = false;
        bool m_publishedLoading = false;
        bool m_publishedCanGoBack = false;
        bool m_publishedCanGoForward = false;
        int m_publishedProgress = 0;
        QString m_publishedUrl;
        QString m_publishedTitle;

        std::shared_ptr<CefViewLifetime> m_lifetime;
        std::shared_ptr<CefOsrPopupQueue> m_popup = std::make_shared<CefOsrPopupQueue>();
        std::function<void()> m_windowCleanup;
        bool m_nativeClosePending = false;
        mutable std::mutex m_browserMutex;
        CefRefPtr<CefBrowser> m_browser;
        bool m_creationPending = false;
        bool m_closeRequested = false;
        bool m_closeIssued = false;
        std::mutex m_screenRectMutex;
        CefRect m_rootWindowScreenRect;
        CefRect m_osrViewRect{0, 0, 1, 1};
        float m_deviceScaleFactor = 1.0F;
        std::mutex m_frameMutex;
        CefOsrFrame m_pendingFrame;
        bool m_frameDeliveryQueued = false;
        std::mutex m_stagingMutex;
        std::array<QImage, 2> m_stagingFrames;
        std::array<QRegion, 2> m_stagingDirtyRegions;
        std::array<bool, 2> m_stagingFrameAvailable{true, true};
        int m_nextStagingFrame = 0;
        QSize m_stagingFrameSize;
        std::atomic_bool m_firstPaintLogged = false;
#if EDEN_ENABLE_AUTOMATION
        std::atomic_bool m_benchmarkReadyLogged = false;
        std::atomic_uint64_t m_idleDiagnosticPaints = 0;
#endif
        CefRefPtr<CefRunContextMenuCallback> m_contextMenuCallback;
        QHash<QString, int> m_contextMenuCommands;
        quint64 m_nextInteractionId = 1;
        std::map<quint64, CefRefPtr<CefJSDialogCallback>> m_javaScriptDialogs;
        struct PermissionCallback {
            uint64_t cefPromptId = 0;
            uint32_t requestedPermissions = 0;
            CefRefPtr<CefMediaAccessCallback> media;
            CefRefPtr<CefPermissionPromptCallback> prompt;
        };
        std::map<quint64, PermissionCallback> m_permissionCallbacks;
        struct DisplayCaptureRequest {
            int rendererRequestId = 0;
            bool audioRequested = false;
            QUrl origin;
            CefRefPtr<CefFrame> frame;
        };
        struct DisplayCaptureApproval {
            QUrl origin;
            std::string frameId;
        };
        std::map<quint64, DisplayCaptureRequest> m_displayCaptureRequests;
        std::optional<DisplayCaptureApproval> m_displayCaptureApproval;
        std::map<quint64, CefRefPtr<CefFileDialogCallback>> m_fileDialogs;
        std::unordered_map<int, std::shared_ptr<CefPopupTransfer>> m_pendingPopups;
        CefRefPtr<CefEngineClient> m_popupSource;
        int m_sourcePopupId = -1;
        std::unordered_map<uint32_t, DownloadRecord> m_downloads;
        std::unordered_set<uint32_t> m_announcedDownloads;
        QString m_downloadDirectory;
        bool m_downloadDirectoryReady = false;
        quint64 m_faviconRequestSerial = 0;
        std::vector<std::string> m_faviconCandidates;
        std::size_t m_faviconCandidateIndex = 0;
        QString m_errorPageUrl;
        QString m_failedUrl;
        std::mutex m_framePidMutex;
        std::string m_mainFrameId;
        std::unordered_map<std::string, int> m_framePids;

        IMPLEMENT_REFCOUNTING(CefEngineClient);
    };

    CefOsrFrame::CefOsrFrame(QImage nextImage, CefRefPtr<CefEngineClient> nextOwner, int nextStagingIndex)
        : image(std::move(nextImage)),
          owner(std::move(nextOwner)),
          stagingIndex(nextStagingIndex) {}

    CefOsrFrame::CefOsrFrame(CefOsrFrame &&other) noexcept
        : image(std::move(other.image)),
          owner(std::move(other.owner)),
          stagingIndex(std::exchange(other.stagingIndex, -1)) {}

    CefOsrFrame &CefOsrFrame::operator=(CefOsrFrame &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        image = std::move(other.image);
        owner = std::move(other.owner);
        stagingIndex = std::exchange(other.stagingIndex, -1);
        return *this;
    }

    CefOsrFrame::~CefOsrFrame() {
        reset();
    }

    bool CefOsrFrame::isNull() const {
        return image.isNull();
    }

    void CefOsrFrame::reset() {
        if (stagingIndex < 0) {
            image = {};
            owner = nullptr;
            return;
        }
        const int releasedIndex = std::exchange(stagingIndex, -1);
        CefRefPtr<CefEngineClient> releasedOwner = std::move(owner);
        image = {};
        if (releasedOwner) {
            releasedOwner->releaseStagingFrame(releasedIndex);
        }
    }

    struct CefPopupTransfer {
        std::shared_ptr<CefViewLifetime> lifetime;
        CefRefPtr<CefEngineClient> client;
        std::atomic_bool adopted = false;
        std::atomic_bool canceled = false;

        ~CefPopupTransfer() {
            if (!adopted.load(std::memory_order_acquire) && client) {
                client->requestClose();
            }
        }
    };

    CefPopupNewViewRequest::~CefPopupNewViewRequest() {
        if (!m_opened && m_transfer && m_transfer->client) {
            m_transfer->client->requestClose();
        }
    }

    bool CefPopupNewViewRequest::openIn(EngineView *target) {
        if (!m_transfer || m_transfer->canceled.load(std::memory_order_acquire)) {
            return false;
        }
        auto *cefTarget = qobject_cast<CefEngineView *>(target);
        m_opened = cefTarget && cefTarget->adoptPopup(m_transfer);
        if (m_opened) {
            m_transfer->adopted.store(true, std::memory_order_release);
        }
        return m_opened;
    }

    class CefEngineView::Private {
      public:
        Private(CefEngineView *view, EngineProfile *profile)
            : q(view),
              profile(qobject_cast<CefProfile *>(profile)),
              osr(!QCoreApplication::arguments().contains("--engine-compositing=windowed")),
              lifetime(std::make_shared<CefViewLifetime>()),
              client(new CefEngineClient(lifetime, [] {
                  const QString location = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
                  return location.isEmpty() ? QDir::home().filePath("Downloads") : location;
              }())) {
            lifetime->bind(this);
        }

        ~Private() {
            destroyDevTools();
            lifetime->close();
            client->requestClose();
            browser = nullptr;
            disconnectViewport();
            if (hostWindow) {
                hostWindow->removeEventFilter(q);
                client->retireWindow(hostWindow);
                hostWindow.clear();
            }
            client = nullptr;
            if (osrItem) {
                delete osrItem;
            }
        }

        void attach(QQuickItem *item) {
            if (!item) {
                disconnectViewport();
                viewport.clear();
                syncVisibility();
                return;
            }
            if (viewport == item) {
                attachViewport();
                return;
            }
            disconnectViewport();
            viewport = item;
            attachViewport();
        }

        void attachDevTools(QQuickItem *item) {
            disconnectDevToolsViewport();
            devToolsViewport = item;
            if (!devToolsViewport) {
                if (devToolsOsrItem) {
                    devToolsOsrItem->setParentItem(nullptr);
                    devToolsOsrItem->setVisible(false);
                }
                syncDevToolsVisibility();
                return;
            }
            devToolsViewportConnections.append(
                QObject::connect(devToolsViewport, &QQuickItem::windowChanged, q, [this](QQuickWindow *) {
                    attachDevTools(devToolsViewport);
                })
            );
            if (!devToolsViewport->window()) {
                syncDevToolsVisibility();
                return;
            }
            QQuickWindow *shellWindow = devToolsViewport->window();
            if (!devToolsOsrItem) {
                devToolsOsrItem = new CefOsrItem(nullptr);
                devToolsOsrItem->setParent(q);
                devToolsOsrItem->setEventHandler([this](QEvent *event) { return forwardDevToolsOsrEvent(event); });
            }
            devToolsOsrItem->setParentItem(devToolsViewport);
            devToolsEventWindow = shellWindow;
            for (QQuickItem *ancestor = devToolsViewport; ancestor; ancestor = ancestor->parentItem()) {
                const auto connectGeometry = [this, ancestor](auto signal) {
                    devToolsViewportConnections.append(QObject::connect(ancestor, signal, q, [this] {
                        updateDevToolsGeometry();
                    }));
                };
                connectGeometry(&QQuickItem::xChanged);
                connectGeometry(&QQuickItem::yChanged);
                connectGeometry(&QQuickItem::widthChanged);
                connectGeometry(&QQuickItem::heightChanged);
                devToolsViewportConnections.append(
                    QObject::connect(ancestor, &QQuickItem::parentChanged, q, [this](QQuickItem *) {
                        attachDevTools(devToolsViewport);
                    })
                );
            }
            devToolsViewportConnections.append(
                QObject::connect(devToolsViewport, &QQuickItem::visibleChanged, q, [this] { syncDevToolsVisibility(); })
            );
            devToolsViewportConnections.append(
                QObject::connect(devToolsViewport, &QQuickItem::activeFocusChanged, q, [this] { syncDevToolsFocus(); })
            );
            devToolsViewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::widthChanged, q, [this] {
                updateDevToolsGeometry();
            }));
            devToolsViewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::heightChanged, q, [this] {
                updateDevToolsGeometry();
            }));
            devToolsViewportConnections.append(
                QObject::connect(shellWindow, &QQuickWindow::visibilityChanged, q, [this] { syncDevToolsVisibility(); })
            );
            devToolsViewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::activeChanged, q, [this] {
                syncDevToolsFocus();
            }));
            devToolsViewportConnections.append(
                QObject::connect(shellWindow, &QQuickWindow::devicePixelRatioChanged, q, [this] {
                    updateDevToolsGeometry();
                })
            );
            updateApplicationEventFilter();
            updateDevToolsGeometry();
            syncDevToolsVisibility();
            createDevToolsBrowser();
        }

        void attachViewport() {
            disconnectViewport();
            if (!viewport) {
                syncVisibility();
                return;
            }
            viewportConnections.append(
                QObject::connect(viewport, &QQuickItem::windowChanged, q, [this](QQuickWindow *) { attachViewport(); })
            );
            if (!viewport->window()) {
                syncVisibility();
                return;
            }
            QQuickWindow *shellWindow = viewport->window();
            if (osr) {
                if (!osrItem) {
                    osrItem = new CefOsrItem(nullptr);
                    osrItem->setParent(q);
                    osrItem->setEventHandler([this](QEvent *event) { return forwardOsrEvent(event); });
                }
                if (osrItem->parentItem() != viewport) {
                    osrItem->setParentItem(viewport);
                    if (browser) {
                        const CefRefPtr<CefBrowser> currentBrowser = browser;
                        postToCefUi([currentBrowser] { currentBrowser->GetHost()->Invalidate(PET_VIEW); });
                    }
                }
                osrEventWindow = shellWindow;
                updateApplicationEventFilter();
            } else if (!hostWindow) {
                hostWindow = new QWindow;
                QSurfaceFormat hostFormat = hostWindow->format();
                hostFormat.setAlphaBufferSize(0);
                hostWindow->setFormat(hostFormat);
                hostWindow->setFlag(Qt::FramelessWindowHint);
                hostWindow->create();
                hostWindow->setParent(shellWindow);
                hostWindow->installEventFilter(q);
            } else if (hostWindow->parent() != shellWindow) {
                hostWindow->setParent(shellWindow);
            }
            for (QQuickItem *item = viewport; item; item = item->parentItem()) {
                const auto connectGeometry = [this, item](auto signal) {
                    viewportConnections.append(QObject::connect(item, signal, q, [this] { updateGeometry(); }));
                };
                connectGeometry(&QQuickItem::xChanged);
                connectGeometry(&QQuickItem::yChanged);
                connectGeometry(&QQuickItem::widthChanged);
                connectGeometry(&QQuickItem::heightChanged);
                viewportConnections.append(QObject::connect(item, &QQuickItem::parentChanged, q, [this](QQuickItem *) {
                    attachViewport();
                }));
            }
            viewportConnections.append(QObject::connect(viewport, &QQuickItem::visibleChanged, q, [this] {
                syncVisibility();
            }));
            viewportConnections.append(QObject::connect(viewport, &QQuickItem::activeFocusChanged, q, [this] {
                syncFocus();
            }));
            viewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::widthChanged, q, [this] {
                updateGeometry();
            }));
            viewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::heightChanged, q, [this] {
                updateGeometry();
            }));
            viewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::visibilityChanged, q, [this] {
                syncVisibility();
            }));
            viewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::activeChanged, q, [this] {
                syncFocus();
            }));
            viewportConnections.append(QObject::connect(shellWindow, &QQuickWindow::devicePixelRatioChanged, q, [this] {
                updateGeometry();
            }));
            updateGeometry();
            syncVisibility();
            createBrowser();
        }

        void disconnectViewport() {
            osrPageFocused = false;
            if (osrItem) {
                osrItem->unsetCursor();
            }
            osrEventWindow.clear();
            for (const QMetaObject::Connection &connection : std::as_const(viewportConnections)) {
                QObject::disconnect(connection);
            }
            viewportConnections.clear();
            updateApplicationEventFilter();
        }

        void disconnectDevToolsViewport() {
            devToolsPageFocused = false;
            if (devToolsOsrItem) {
                devToolsOsrItem->unsetCursor();
            }
            devToolsEventWindow.clear();
            for (const QMetaObject::Connection &connection : std::as_const(devToolsViewportConnections)) {
                QObject::disconnect(connection);
            }
            devToolsViewportConnections.clear();
            updateApplicationEventFilter();
        }

        void updateApplicationEventFilter() {
            const bool needed = osrEventWindow || devToolsEventWindow;
            if (needed && !osrApplicationFilterInstalled) {
                qGuiApp->installEventFilter(q);
                osrApplicationFilterInstalled = true;
            } else if (!needed && osrApplicationFilterInstalled) {
                qGuiApp->removeEventFilter(q);
                osrApplicationFilterInstalled = false;
            }
        }

        void createBrowser() {
            if (browser || browserCreationStarted || !profile || !viewport || (!osr && !hostWindow)) {
                return;
            }
            if (!osr && QGuiApplication::platformName() != "xcb") {
                qCritical("The Blink engine requires the Qt xcb platform for windowed rendering");
                return;
            }
            const QSize logicalSize = browserLogicalSize();
            if (logicalSize.isEmpty()) {
                return;
            }
            CefWindowInfo windowInfo;
            if (osr) {
                windowInfo.SetAsWindowless(0);
                windowInfo.shared_texture_enabled = qEnvironmentVariableIntValue("EDEN_CEF_SHARED_TEXTURE_PROBE") == 1;
            } else {
                hostWindow->create();
                windowInfo.SetAsChild(
                    static_cast<CefWindowHandle>(hostWindow->winId()),
                    CefRect(0, 0, logicalSize.width(), logicalSize.height())
                );
            }
            configureAlloyRuntime(windowInfo);
            const CefRefPtr<CefEngineClient> currentClient = client;
            const CefRefPtr<CefRequestContext> requestContext = profile->requestContext();
            const std::string initialUrl = pendingUrl.toString().toStdString();
            if (!currentClient->beginBrowserCreation()) {
                return;
            }
            browserCreationStarted = true;
            loadPendingAfterCreation = true;
            if (!postToCefUi([windowInfo, currentClient, requestContext, initialUrl]() mutable {
                    if (!currentClient->shouldCreateBrowser()) {
                        return;
                    }
                    CefBrowserSettings settings;
                    settings.background_color = CefColorSetARGB(255, 255, 255, 255);
                    settings.windowless_frame_rate = 60;
                    if (!CefBrowserHost::CreateBrowser(
                            windowInfo,
                            currentClient,
                            initialUrl,
                            settings,
                            nullptr,
                            requestContext
                        )) {
                        currentClient->browserCreationFailed();
                    }
                })) {
                currentClient->browserCreationFailed();
            }
        }

        void browserCreated(CefRefPtr<CefBrowser> createdBrowser) {
            CefUiBridge::assertOnUiThread(q);
            browser = createdBrowser;
            browserVisible.reset();
            browserCreationStarted = true;
            browser->GetHost()->SetAudioMuted(muted);
            installFileChooserInterception();
            if (loadPendingAfterCreation && !pendingUrl.isEmpty() &&
                QString::fromStdString(browser->GetMainFrame()->GetURL().ToString()) != pendingUrl.toString()) {
                browser->GetMainFrame()->LoadURL(pendingUrl.toString().toStdString());
            }
            loadPendingAfterCreation = false;
            updateGeometry();
            syncFocus();
        }

        void createDevToolsBrowser() {
            if (!q->devToolsOpen() || devToolsBrowser || devToolsBrowserCreationStarted || !browser || !profile ||
                !devToolsViewport || !devToolsOsrItem) {
                return;
            }
            const QSize logicalSize = devToolsLogicalSize();
            if (logicalSize.isEmpty()) {
                return;
            }
            devToolsBrowserCreationStarted = true;
            devToolsProtocolSession = std::make_shared<CefDevToolsProtocolSession>(browser);
            const std::weak_ptr<CefDevToolsProtocolSession> weakSession = devToolsProtocolSession;
            devToolsSocketSession = sharedDevToolsSocketServer()->openSession([weakSession](const QByteArray &message) {
                if (const std::shared_ptr<CefDevToolsProtocolSession> session = weakSession.lock()) {
                    session->sendToInspected(message);
                }
            });
            if (!devToolsSocketSession.isValid()) {
                destroyDevTools();
                qCritical("The developer tools connection could not be opened");
                q->setDevToolsOpen(false);
                return;
            }
            devToolsLifetime = std::make_shared<CefViewLifetime>();
            devToolsLifetime->bind(this);
            devToolsClient = new CefDevToolsClient(devToolsLifetime, client.get());
            if (!devToolsProtocolSession->start(devToolsSocketSession.token, devToolsLifetime)) {
                devToolsCreationFailed();
            }
        }

        void createDevToolsFrontendBrowser() {
            CefUiBridge::assertOnUiThread(q);
            if (!q->devToolsOpen() || devToolsBrowser || !devToolsBrowserCreationStarted || !devToolsClient ||
                !profile || !devToolsSocketSession.isValid()) {
                return;
            }
            updateDevToolsGeometry();
            CefWindowInfo windowInfo;
            windowInfo.SetAsWindowless(0);
            windowInfo.shared_texture_enabled = false;
            configureAlloyRuntime(windowInfo);
            const CefRefPtr<CefDevToolsClient> currentClient = devToolsClient;
            const CefRefPtr<CefRequestContext> requestContext = profile->requestContext();
            const std::string frontendUrl = QString("devtools://devtools/bundled/inspector.html?ws=%1&panel=elements")
                                                .arg(devToolsSocketSession.endpoint)
                                                .toStdString();
            if (!currentClient->beginBrowserCreation()) {
                destroyDevTools();
                return;
            }
            if (!postToCefUi([windowInfo, currentClient, requestContext, frontendUrl]() mutable {
                    if (!currentClient->shouldCreateBrowser()) {
                        return;
                    }
                    CefBrowserSettings settings;
                    settings.background_color = CefColorSetARGB(255, 255, 255, 255);
                    settings.windowless_frame_rate = 60;
                    if (!CefBrowserHost::CreateBrowser(
                            windowInfo,
                            currentClient,
                            frontendUrl,
                            settings,
                            nullptr,
                            requestContext
                        )) {
                        currentClient->browserCreationFailed();
                    }
                })) {
                currentClient->browserCreationFailed();
            }
        }

        void devToolsBrowserCreated(CefRefPtr<CefBrowser> createdBrowser) {
            CefUiBridge::assertOnUiThread(q);
            if (!q->devToolsOpen()) {
                createdBrowser->GetHost()->CloseBrowser(true);
                return;
            }
            devToolsBrowser = createdBrowser;
            devToolsBrowserVisible.reset();
            devToolsBrowserCreationStarted = true;
            updateDevToolsGeometry();
            syncDevToolsVisibility();
            syncDevToolsFocus();
        }

        void devToolsBrowserClosed(int identifier) {
            CefUiBridge::assertOnUiThread(q);
            if (devToolsBrowser && devToolsBrowser->GetIdentifier() == identifier) {
                devToolsBrowser = nullptr;
            }
            devToolsBrowserCreationStarted = false;
        }

        void devToolsCreationFailed() {
            CefUiBridge::assertOnUiThread(q);
            devToolsBrowserCreationStarted = false;
            destroyDevTools();
            qCritical("The developer tools browser could not be created");
            q->setDevToolsOpen(false);
        }

        void destroyDevTools() {
            disconnectDevToolsViewport();
            if (!devToolsSocketSession.token.isEmpty()) {
                sharedDevToolsSocketServer()->closeSession(devToolsSocketSession.token);
                devToolsSocketSession = {};
            }
            if (devToolsProtocolSession) {
                devToolsProtocolSession->close();
                devToolsProtocolSession.reset();
            }
            if (devToolsLifetime) {
                devToolsLifetime->close();
            }
            if (devToolsClient) {
                devToolsClient->requestClose();
            }
            devToolsBrowser = nullptr;
            devToolsClient = nullptr;
            devToolsLifetime.reset();
            devToolsBrowserCreationStarted = false;
            if (devToolsOsrItem) {
                delete devToolsOsrItem;
                devToolsOsrItem.clear();
            }
            devToolsViewport.clear();
        }

        bool adoptPopup(const std::shared_ptr<CefPopupTransfer> &transfer) {
            CefUiBridge::assertOnUiThread(q);
            if (!transfer || !transfer->lifetime || !transfer->client) {
                return false;
            }
            lifetime->close();
            client->requestClose();
            if (hostWindow) {
                hostWindow->removeEventFilter(q);
                client->retireWindow(hostWindow);
                hostWindow.clear();
            }
            browser = nullptr;
            client = transfer->client;
            lifetime = transfer->lifetime;
            loadPendingAfterCreation = false;
            lifetime->bind(this);
            browserCreationStarted = true;
            if (!osr) {
                osr = true;
                attachViewport();
            }
            if (CefRefPtr<CefBrowser> adoptedBrowser = client->browserSnapshot()) {
                browserCreated(adoptedBrowser);
            }
            return true;
        }

        void browserClosed(int identifier) {
            CefUiBridge::assertOnUiThread(q);
            if (browser && browser->GetIdentifier() == identifier) {
                browser = nullptr;
                fileChooserRegistration = nullptr;
                fileChooserNodes.clear();
                cancelAutofillTargets();
            }
        }

        void creationFailed() {
            CefUiBridge::assertOnUiThread(q);
            browserCreationStarted = false;
            qCritical("The Blink browser window could not be created");
        }

        void updateUrl(const QString &value) {
            CefUiBridge::assertOnUiThread(q);
            const QUrl next(value);
            if (url == next) {
                return;
            }
            url = next;
            pendingUrl = next;
            if (!certificate.isEmpty()) {
                certificate.clear();
                emit q->certificateDetailsChanged();
            }
            emit q->urlChanged();
            emit q->securityStateChanged();
        }

        void updateCertificateDetails(const QVariantMap &details) {
            CefUiBridge::assertOnUiThread(q);
            if (certificate == details) {
                return;
            }
            certificate = details;
            emit q->certificateDetailsChanged();
            emit q->securityStateChanged();
        }

        void updateTitle(const QString &value) {
            CefUiBridge::assertOnUiThread(q);
            if (title == value) {
                return;
            }
            title = value;
            emit q->titleChanged();
        }

        void updateFavicon(QImage image) {
            CefUiBridge::assertOnUiThread(q);
            QUrl next;
            if (!image.isNull()) {
                QByteArray encoded;
                QBuffer buffer(&encoded);
                if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
                    next = QUrl(QString("data:image/png;base64,%1").arg(QString::fromLatin1(encoded.toBase64())));
                }
            }
            if (faviconUrl == next) {
                return;
            }
            faviconUrl = next;
            emit q->faviconUrlChanged();
        }

        void updateProgress(int nextProgress) {
            CefUiBridge::assertOnUiThread(q);
            nextProgress = std::clamp(nextProgress, 0, 100);
            if (progress == nextProgress) {
                return;
            }
            progress = nextProgress;
            emit q->loadProgressChanged();
        }

        void updateCursor(cef_cursor_type_t type) {
            CefUiBridge::assertOnUiThread(q);
            updateItemCursor(osrItem, type);
        }

        void updateDevToolsCursor(cef_cursor_type_t type) {
            CefUiBridge::assertOnUiThread(q);
            updateItemCursor(devToolsOsrItem, type);
        }

        static void updateItemCursor(CefOsrItem *item, cef_cursor_type_t type) {
            if (!item) {
                return;
            }
            auto cursorShape = Qt::ArrowCursor;
            switch (type) {
            case CT_CROSS:
            case CT_CELL:
                cursorShape = Qt::CrossCursor;
                break;
            case CT_HAND:
                cursorShape = Qt::PointingHandCursor;
                break;
            case CT_IBEAM:
            case CT_VERTICALTEXT:
                cursorShape = Qt::IBeamCursor;
                break;
            case CT_WAIT:
            case CT_PROGRESS:
                cursorShape = Qt::BusyCursor;
                break;
            case CT_HELP:
                cursorShape = Qt::WhatsThisCursor;
                break;
            case CT_EASTRESIZE:
            case CT_WESTRESIZE:
            case CT_EASTWESTRESIZE:
            case CT_COLUMNRESIZE:
                cursorShape = Qt::SizeHorCursor;
                break;
            case CT_NORTHRESIZE:
            case CT_SOUTHRESIZE:
            case CT_NORTHSOUTHRESIZE:
            case CT_ROWRESIZE:
                cursorShape = Qt::SizeVerCursor;
                break;
            case CT_NORTHEASTRESIZE:
            case CT_SOUTHWESTRESIZE:
            case CT_NORTHEASTSOUTHWESTRESIZE:
                cursorShape = Qt::SizeBDiagCursor;
                break;
            case CT_NORTHWESTRESIZE:
            case CT_SOUTHEASTRESIZE:
            case CT_NORTHWESTSOUTHEASTRESIZE:
                cursorShape = Qt::SizeFDiagCursor;
                break;
            case CT_MOVE:
                cursorShape = Qt::SizeAllCursor;
                break;
            case CT_NODROP:
            case CT_NOTALLOWED:
            case CT_DND_NONE:
                cursorShape = Qt::ForbiddenCursor;
                break;
            case CT_GRAB:
                cursorShape = Qt::OpenHandCursor;
                break;
            case CT_GRABBING:
                cursorShape = Qt::ClosedHandCursor;
                break;
            case CT_COPY:
            case CT_DND_COPY:
                cursorShape = Qt::DragCopyCursor;
                break;
            case CT_ALIAS:
            case CT_DND_LINK:
                cursorShape = Qt::DragLinkCursor;
                break;
            case CT_DND_MOVE:
                cursorShape = Qt::DragMoveCursor;
                break;
            case CT_NONE:
                cursorShape = Qt::BlankCursor;
                break;
            default:
                break;
            }
            item->setCursor(cursorShape);
        }

        void updateLoading(bool nextLoading, bool nextCanGoBack, bool nextCanGoForward) {
            CefUiBridge::assertOnUiThread(q);
            if (loading != nextLoading) {
                loading = nextLoading;
                if (loading) {
                    cancelAutofillTargets();
                }
                updateProgress(loading ? 0 : 100);
                emit q->loadingChanged();
            }
            if (canGoBack != nextCanGoBack) {
                canGoBack = nextCanGoBack;
                emit q->canGoBackChanged();
            }
            if (canGoForward != nextCanGoForward) {
                canGoForward = nextCanGoForward;
                emit q->canGoForwardChanged();
            }
        }

        void loadFinished() {
            CefUiBridge::assertOnUiThread(q);
            if (progress != 100) {
                progress = 100;
                emit q->loadProgressChanged();
            }
            refreshNavigationHistory();
        }

        void resolveAutofillTarget(const QString &id, AutofillTarget target) {
            CefUiBridge::assertOnUiThread(q);
            const auto found = autofillRequests.find(id);
            if (found == autofillRequests.end()) {
                return;
            }
            AutofillTargetCallback callback = std::move(found.value());
            autofillRequests.erase(found);
            callback(std::move(target));
        }

        void cancelAutofillTargets() {
            QHash<QString, AutofillTargetCallback> callbacks;
            callbacks.swap(autofillRequests);
            for (auto &callback : callbacks) {
                callback({});
            }
        }

        void credentialSubmitted(const QUrl &origin, const QString &username, const QString &password) {
            CredentialSubmissionInfo info;
            info.origin = origin;
            info.username = username;
            info.password = password;
            emit q->credentialSubmitted(info);
        }

        void formFieldFocused(
            const QUrl &origin,
            const QString &type,
            const QString &name,
            const QString &autocomplete,
            const QString &value,
            const QRectF &rect
        ) {
            emit q->formFieldFocused(
                {{"origin", origin},
                 {"type", type},
                 {"name", name},
                 {"autocomplete", autocomplete},
                 {"value", value},
                 {"x", rect.x()},
                 {"y", rect.y()},
                 {"width", rect.width()},
                 {"height", rect.height()}}
            );
        }

        void refreshNavigationHistory() {
            CefUiBridge::assertOnUiThread(q);
            const CefRefPtr<CefBrowser> currentBrowser = browser;
            if (!currentBrowser) {
                return;
            }
            QPointer<CefEngineView> guard(q);
            postToCefUi([currentBrowser, guard] {
                CefNavigationHistoryObserver::capture(currentBrowser, [guard](int currentIndex, QVariantList entries) {
                    if (guard) {
                        if (guard->d->historyCurrentIndex == currentIndex && guard->d->historyEntries == entries) {
                            return;
                        }
                        guard->d->historyCurrentIndex = currentIndex;
                        guard->d->historyEntries = std::move(entries);
                        emit guard->canGoBackChanged();
                        emit guard->canGoForwardChanged();
                    }
                });
            });
        }

        void renderProcessTerminated(const QUrl &failedUrl, const QString &details, const QString &errorPageUrl) {
            CefUiBridge::assertOnUiThread(q);
            cancelAutofillTargets();
            updateTitle("Tab crashed");
            updateLoading(false, canGoBack, canGoForward);
            pendingUrl = failedUrl;
            qWarning().noquote() << "A Blink renderer process terminated:" << details;
            const CefRefPtr<CefEngineClient> currentClient = client;
            postToCefUi([currentClient, errorPageUrl] {
                const CefRefPtr<CefBrowser> currentBrowser = currentClient ? currentClient->browserSnapshot() : nullptr;
                const CefRefPtr<CefFrame> frame = currentBrowser ? currentBrowser->GetMainFrame() : nullptr;
                if (frame) {
                    frame->LoadURL(errorPageUrl.toStdString());
                }
            });
        }

        void requestNewView(
            const QUrl &requestedUrl,
            EngineView::Disposition disposition,
            bool userInitiated,
            const std::shared_ptr<CefPopupTransfer> &transfer
        ) {
            CefUiBridge::assertOnUiThread(q);
            CefPopupNewViewRequest request(requestedUrl, disposition, userInitiated, transfer);
            emit q->newViewRequested(&request);
        }

        void requestContextMenu(const ContextMenuInfo &info) {
            CefUiBridge::assertOnUiThread(q);
            lastContextMenu = info;
            emit q->contextMenuRequested(info);
        }

        void requestJavaScriptDialog(const JavaScriptDialogInfo &info) {
            CefUiBridge::assertOnUiThread(q);
            emit q->javaScriptDialogRequested(info);
        }

        void closeJavaScriptDialog(quint64 id) {
            CefUiBridge::assertOnUiThread(q);
            emit q->javaScriptDialogClosed(id);
        }

        void requestPermission(const PermissionRequestInfo &info) {
            CefUiBridge::assertOnUiThread(q);
            emit q->permissionRequested(info);
        }

        void requestDisplayCapture(const DisplayCaptureRequestInfo &info) {
            CefUiBridge::assertOnUiThread(q);
            emit q->displayCaptureRequested(info);
        }

        void requestFileDialog(const FileDialogInfo &info) {
            CefUiBridge::assertOnUiThread(q);
            emit q->fileDialogRequested(info);
        }

        void closeFileDialog(quint64 id) {
            CefUiBridge::assertOnUiThread(q);
            emit q->fileDialogClosed(id);
        }

        void closePermissionRequest(quint64 id) {
            CefUiBridge::assertOnUiThread(q);
            emit q->permissionRequestClosed(id);
        }

        void closeDisplayCaptureRequest(quint64 id) {
            CefUiBridge::assertOnUiThread(q);
            emit q->displayCaptureRequestClosed(id);
        }

        void updateDownload(
            uint32_t id,
            const QString &fileName,
            const QUrl &sourceUrl,
            const QString &targetPath,
            qint64 receivedBytes,
            qint64 totalBytes,
            const QString &state,
            bool firstUpdate
        ) {
            CefUiBridge::assertOnUiThread(q);
            if (!profile) {
                return;
            }
            if (firstUpdate) {
                emit profile
                    ->downloadStarted(profile->downloadIdentifier(id), fileName, sourceUrl, targetPath, totalBytes);
            }
            emit profile->downloadUpdated(profile->downloadIdentifier(id), receivedBytes, totalBytes, state);
        }

        void presentFrame(CefOsrFrame frame) {
            CefUiBridge::assertOnUiThread(q);
            if (osrItem) {
                osrItem->presentFrame(std::move(frame));
            }
        }

        void presentPopup(CefOsrPopupFrame frame) {
            CefUiBridge::assertOnUiThread(q);
            if (osrItem) {
                osrItem->presentPopup(std::move(frame));
            }
        }

        void presentDevToolsPopup(CefOsrPopupFrame frame) {
            CefUiBridge::assertOnUiThread(q);
            if (devToolsOsrItem) {
                devToolsOsrItem->presentPopup(std::move(frame));
            }
        }

        void presentDevToolsFrame(QImage frame) {
            CefUiBridge::assertOnUiThread(q);
            if (q->devToolsOpen() && devToolsOsrItem) {
                devToolsOsrItem->presentFrame(std::move(frame));
            }
        }

        void requestThumbnail(const QSize &size, EngineView::ThumbnailCallback callback) {
            const CefRefPtr<CefBrowser> currentBrowser = browser;
            if (!currentBrowser || !callback || !size.isValid()) {
                if (callback) {
                    callback({});
                }
                return;
            }
            if (!postToCefUi([currentBrowser, size, callback = std::move(callback)]() mutable {
                    CefThumbnailObserver::capture(currentBrowser, size, std::move(callback));
                })) {
                callback({});
            }
        }

        void installFileChooserInterception() {
            CefUiBridge::assertOnUiThread(q);
            const CefRefPtr<CefBrowser> currentBrowser = browser;
            if (!currentBrowser) {
                return;
            }
            const CefRefPtr<CefFileChooserObserver> observer = new CefFileChooserObserver(lifetime);
            QPointer<CefEngineView> guard(q);
            postToCefUi([currentBrowser, observer, guard] {
                CefRefPtr<CefRegistration> registration =
                    currentBrowser->GetHost()->AddDevToolsMessageObserver(observer);
                currentBrowser->GetHost()->ExecuteDevToolsMethod(nextDevToolsMessageId(), "Page.enable", nullptr);
                CefRefPtr<CefDictionaryValue> parameters = CefDictionaryValue::Create();
                parameters->SetBool("enabled", true);
                currentBrowser->GetHost()
                    ->ExecuteDevToolsMethod(nextDevToolsMessageId(), "Page.setInterceptFileChooserDialog", parameters);
                CefUiBridge::runOnUiThread([guard, registration] {
                    if (guard) {
                        guard->d->fileChooserRegistration = registration;
                    }
                });
            });
        }

        void fileChooserOpened(const QString &mode, qint64 backendNodeId, const QString &frameId) {
            CefUiBridge::assertOnUiThread(q);
            const quint64 id = nextFileChooserId++;
            fileChooserNodes.insert(id, qMakePair(backendNodeId, frameId));
            FileDialogInfo info;
            info.id = id;
            info.mode = mode == "selectMultiple" ? "openMultiple" : "open";
            info.nameFilters = QStringList("All files (*)");
            emit q->fileDialogRequested(info);
        }

        bool resolveFileChooser(quint64 id, bool accepted, const QList<QUrl> &files) {
            CefUiBridge::assertOnUiThread(q);
            const auto found = fileChooserNodes.constFind(id);
            if (found == fileChooserNodes.cend()) {
                return false;
            }
            const qint64 backendNodeId = found.value().first;
            const QString frameId = found.value().second;
            fileChooserNodes.erase(found);
            if (!accepted || files.isEmpty() || !browser) {
                return true;
            }
            const CefRefPtr<CefBrowser> currentBrowser = browser;
            QStringList paths;
            for (const QUrl &file : files) {
                const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
                if (!path.isEmpty()) {
                    paths.append(path);
                }
            }
            postToCefUi([currentBrowser, backendNodeId, frameId, paths] {
                CefRefPtr<CefDictionaryValue> parameters = CefDictionaryValue::Create();
                CefRefPtr<CefListValue> fileList = CefListValue::Create();
                for (qsizetype index = 0; index < paths.size(); ++index) {
                    fileList->SetString(static_cast<size_t>(index), paths.at(index).toStdString());
                }
                parameters->SetList("files", fileList);
                parameters->SetDouble("backendNodeId", static_cast<double>(backendNodeId));
                if (!frameId.isEmpty()) {
                    parameters->SetString("frameId", frameId.toStdString());
                }
                currentBrowser->GetHost()
                    ->ExecuteDevToolsMethod(nextDevToolsMessageId(), "DOM.setFileInputFiles", parameters);
            });
            return true;
        }

        void updateRendererClientId(int clientId) {
            CefUiBridge::assertOnUiThread(q);
            if (rendererClientId != clientId) {
                rendererClientId = clientId;
                cachedRendererPid = 0;
            }
        }

        void pasteFromShellClipboard(CefRefPtr<CefBrowser> targetBrowser = nullptr) {
            CefUiBridge::assertOnUiThread(q);
            const CefRefPtr<CefBrowser> currentBrowser = targetBrowser ? targetBrowser : browser;
            if (!currentBrowser) {
                return;
            }
            QClipboard *clipboard = QGuiApplication::clipboard();
            const QString text = clipboard ? clipboard->text() : QString();
            if (text.isEmpty() || text == mirroredClipboardText()) {
                postToCefUi([currentBrowser] {
                    CefRefPtr<CefFrame> frame = currentBrowser->GetFocusedFrame();
                    if (!frame) {
                        frame = currentBrowser->GetMainFrame();
                    }
                    if (frame) {
                        frame->Paste();
                    }
                });
                return;
            }
            const std::string encoded = text.toStdString();
            postToCefUi([currentBrowser, encoded] {
                CefRefPtr<CefDictionaryValue> parameters = CefDictionaryValue::Create();
                parameters->SetString("text", encoded);
                currentBrowser->GetHost()
                    ->ExecuteDevToolsMethod(nextDevToolsMessageId(), "Input.insertText", parameters);
            });
        }

        void requestFullscreen(bool fullscreen) {
            CefUiBridge::assertOnUiThread(q);
            emit q->fullscreenRequested(fullscreen);
        }

        void requestShortcut(const QString &command) {
            CefUiBridge::assertOnUiThread(q);
            if (command == "focus_omnibox") {
                focusShell();
                focusDevToolsShell();
            }
            emit q->shortcutRequested(command);
        }

        void takeFocus(bool next) {
            CefUiBridge::assertOnUiThread(q);
            if (!viewport) {
                return;
            }
            focusShell();
            QQuickItem *target = viewport->nextItemInFocusChain(next);
            if (target && target != viewport) {
                target->forceActiveFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            }
        }

        void browserFocused() {
            CefUiBridge::assertOnUiThread(q);
            if (!browser || !viewport) {
                return;
            }
            if (osrItem) {
                if (!viewport->isVisible() || !osrItem->isVisible()) {
                    return;
                }
                QQuickWindow *shellWindow = viewport->window();
                QQuickItem *activeItem = shellWindow ? shellWindow->activeFocusItem() : nullptr;
                if (activeItem && activeItem != viewport && activeItem != osrItem) {
                    browser->GetHost()->SetFocus(false);
                    return;
                }
                osrPageFocused = true;
                if (!osrItem->hasActiveFocus()) {
                    osrItem->forceActiveFocus(Qt::MouseFocusReason);
                }
                return;
            }
            if (viewport->hasActiveFocus()) {
                return;
            }
            auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
            Display *display = x11 ? x11->display() : nullptr;
            Window focusedWindow = 0;
            int revertTo = 0;
            if (display && XGetInputFocus(display, &focusedWindow, &revertTo) &&
                windowContains(display, static_cast<Window>(browser->GetHost()->GetWindowHandle()), focusedWindow)) {
                viewport->forceActiveFocus(Qt::MouseFocusReason);
            }
        }

        void devToolsTakeFocus(bool next) {
            CefUiBridge::assertOnUiThread(q);
            if (!devToolsViewport) {
                return;
            }
            focusDevToolsShell();
            QQuickItem *target = devToolsViewport->nextItemInFocusChain(next);
            if (target && target != devToolsViewport) {
                target->forceActiveFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            }
        }

        void devToolsBrowserFocused() {
            CefUiBridge::assertOnUiThread(q);
            if (!devToolsBrowser || !devToolsViewport || !devToolsOsrItem || !devToolsViewport->isVisible() ||
                !devToolsOsrItem->isVisible()) {
                return;
            }
            osrPageFocused = false;
            devToolsPageFocused = true;
            if (!devToolsOsrItem->hasActiveFocus()) {
                devToolsOsrItem->forceActiveFocus(Qt::MouseFocusReason);
            }
        }

        void focusShell() {
            osrPageFocused = false;
            if (!browser || !viewport || !viewport->window()) {
                return;
            }
            browser->GetHost()->SetFocus(false);
            auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
            Display *display = x11 ? x11->display() : nullptr;
            if (display) {
                XUngrabKeyboard(display, CurrentTime);
                XSetInputFocus(display, static_cast<Window>(viewport->window()->winId()), RevertToParent, CurrentTime);
                XFlush(display);
            }
        }

        void focusDevToolsShell() {
            devToolsPageFocused = false;
            if (!devToolsBrowser || !devToolsViewport || !devToolsViewport->window()) {
                return;
            }
            devToolsBrowser->GetHost()->SetFocus(false);
            devToolsViewport->window()->requestActivate();
        }

        void updateGeometry() {
            if (!viewport || !viewport->window() || (!osr && !hostWindow)) {
                return;
            }
            const QRectF sceneRect = viewport->mapRectToScene(viewport->boundingRect());
            const QRect geometry = sceneRect.toAlignedRect();
            if (osrItem) {
                osrItem->setSize(viewport->size());
            } else if (hostWindow->geometry() != geometry) {
                hostWindow->setGeometry(geometry);
            }
            const QRect screenGeometry = viewport->window()->geometry();
            client->setRootWindowScreenRect(
                CefRect(screenGeometry.x(), screenGeometry.y(), screenGeometry.width(), screenGeometry.height())
            );
            client->setOsrGeometry(
                CefRect(0, 0, browserLogicalSize().width(), browserLogicalSize().height()),
                static_cast<float>(viewport->window()->devicePixelRatio())
            );
            syncVisibility();
            resizeBrowser();
        }

        void updateDevToolsGeometry() {
            if (!devToolsViewport || !devToolsViewport->window() || !devToolsOsrItem) {
                return;
            }
            devToolsOsrItem->setSize(devToolsViewport->size());
            if (devToolsClient) {
                const QRect screenGeometry = devToolsViewport->window()->geometry();
                devToolsClient->setGeometry(
                    CefRect(screenGeometry.x(), screenGeometry.y(), screenGeometry.width(), screenGeometry.height()),
                    CefRect(0, 0, devToolsLogicalSize().width(), devToolsLogicalSize().height()),
                    static_cast<float>(devToolsViewport->window()->devicePixelRatio())
                );
            }
            syncDevToolsVisibility();
            resizeDevToolsBrowser();
        }

        QSize browserLogicalSize() const {
            if (!viewport) {
                return {};
            }
            return QSize(std::max(1, qRound(viewport->width())), std::max(1, qRound(viewport->height())));
        }

        QSize browserPixelSize() const {
            if (!viewport || !viewport->window()) {
                return {};
            }
            const qreal scale = viewport->window()->devicePixelRatio();
            return QSize(
                std::max(1, qRound(viewport->width() * scale)),
                std::max(1, qRound(viewport->height() * scale))
            );
        }

#if EDEN_ENABLE_AUTOMATION
        bool automationWheel(int delta) {
            if (!browser) {
                return false;
            }
            const QSize size = browserLogicalSize();
            static std::atomic_int messageId{1000000};
            const QByteArray message =
                QByteArray("{\"id\":") + QByteArray::number(messageId.fetch_add(1, std::memory_order_relaxed)) +
                ",\"method\":\"Input.dispatchMouseEvent\",\"params\":{\"type\":\"mouseWheel\",\"x\":" +
                QByteArray::number(size.width() / 2) + ",\"y\":" + QByteArray::number(size.height() / 2) +
                ",\"deltaX\":0,\"deltaY\":" + QByteArray::number(-delta) + ",\"pointerType\":\"mouse\"}}";
            const CefRefPtr<CefBrowser> target = browser;
            return postToCefUi([target, message] {
                target->GetHost()->SendDevToolsMessage(message.constData(), static_cast<size_t>(message.size()));
            });
        }
#endif

        QSize devToolsLogicalSize() const {
            if (!devToolsViewport) {
                return {};
            }
            return QSize(
                std::max(1, qRound(devToolsViewport->width())),
                std::max(1, qRound(devToolsViewport->height()))
            );
        }

        void resizeBrowser() {
            if (!browser || (osr ? !osrItem : !hostWindow)) {
                return;
            }
            if (osr) {
                browser->GetHost()->WasResized();
                browser->GetHost()->NotifyScreenInfoChanged();
                return;
            }
            const QSize pixelSize = browserPixelSize();
            const CefRefPtr<CefBrowser> currentBrowser = browser;
            postToCefUi([currentBrowser, pixelSize] {
                CefRefPtr<CefBrowserHost> browserHost = currentBrowser->GetHost();
                Display *display = cef_get_xdisplay();
                const CefWindowHandle browserWindow = browserHost->GetWindowHandle();
                browserHost->NotifyMoveOrResizeStarted();
                if (display && browserWindow && !pixelSize.isEmpty()) {
                    XMoveResizeWindow(
                        display,
                        browserWindow,
                        0,
                        0,
                        static_cast<unsigned int>(pixelSize.width()),
                        static_cast<unsigned int>(pixelSize.height())
                    );
                    XFlush(display);
                }
                browserHost->WasResized();
                browserHost->NotifyScreenInfoChanged();
            });
        }

        void resizeDevToolsBrowser() {
            if (!devToolsBrowser || !devToolsOsrItem) {
                return;
            }
            devToolsBrowser->GetHost()->WasResized();
            devToolsBrowser->GetHost()->NotifyScreenInfoChanged();
        }

        void syncVisibility() {
            const bool visible = viewport && viewport->window() && viewport->window()->isVisible() &&
                                 viewport->window()->visibility() != QWindow::Minimized && viewport->isVisible() &&
                                 viewport->width() > 0 && viewport->height() > 0;
            if (osrItem) {
                osrItem->setVisible(visible);
                if (!visible) {
                    osrPageFocused = false;
                }
            } else if (hostWindow) {
                hostWindow->setVisible(visible);
            }
            if (osr) {
                const bool showing = visible && browserVisible != visible;
                syncBrowserVisibility(browser, visible, browserVisible);
                if (showing) {
                    syncFocus();
                }
            }
        }

        void syncDevToolsVisibility() {
            const bool visible = q->devToolsOpen() && devToolsViewport && devToolsViewport->window() &&
                                 devToolsViewport->window()->isVisible() &&
                                 devToolsViewport->window()->visibility() != QWindow::Minimized &&
                                 devToolsViewport->isVisible() && devToolsViewport->width() > 0 &&
                                 devToolsViewport->height() > 0;
            if (devToolsOsrItem) {
                devToolsOsrItem->setVisible(visible);
            }
            if (!visible) {
                devToolsPageFocused = false;
            }
            const bool showing = visible && devToolsBrowserVisible != visible;
            syncBrowserVisibility(devToolsBrowser, visible, devToolsBrowserVisible);
            if (showing) {
                syncDevToolsFocus();
            }
        }

        static void
        syncBrowserVisibility(CefRefPtr<CefBrowser> target, bool visible, std::optional<bool> &lastVisible) {
            if (!target || lastVisible == visible) {
                return;
            }
            lastVisible = visible;
            postToCefUi([target, visible] {
                target->GetHost()->WasHidden(!visible);
                if (visible) {
                    target->GetHost()->Invalidate(PET_VIEW);
                } else {
                    target->GetHost()->SetFocus(false);
                }
            });
        }

        void syncFocus() {
            if (!browser || !viewport || !viewport->window()) {
                return;
            }
            QQuickWindow *shellWindow = viewport->window();
            if (!shellWindow->isActive()) {
                osrPageFocused = false;
                browser->GetHost()->SetFocus(false);
                return;
            }
            QQuickItem *activeItem = shellWindow->activeFocusItem();
            if (activeItem && activeItem != viewport && activeItem != osrItem) {
                focusShell();
                return;
            }
            if (viewport->hasActiveFocus() || (osrItem && osrItem->hasActiveFocus())) {
                osrPageFocused = osrItem && osrItem->hasActiveFocus();
                browser->GetHost()->SetFocus(true);
            }
        }

        void syncDevToolsFocus() {
            if (!devToolsBrowser || !devToolsViewport || !devToolsViewport->window() || !devToolsOsrItem) {
                return;
            }
            QQuickWindow *shellWindow = devToolsViewport->window();
            if (!shellWindow->isActive()) {
                devToolsPageFocused = false;
                devToolsBrowser->GetHost()->SetFocus(false);
                return;
            }
            QQuickItem *activeItem = shellWindow->activeFocusItem();
            if (activeItem && activeItem != devToolsViewport && activeItem != devToolsOsrItem) {
                focusDevToolsShell();
                return;
            }
            if (devToolsViewport->hasActiveFocus() || devToolsOsrItem->hasActiveFocus()) {
                osrPageFocused = false;
                devToolsPageFocused = devToolsOsrItem->hasActiveFocus();
                devToolsBrowser->GetHost()->SetFocus(true);
            }
        }

        bool forwardOsrEvent(QEvent *event) {
            return forwardOsrEventToBrowser(event, browser, osrItem, osrPageFocused, devToolsPageFocused);
        }

        bool forwardDevToolsOsrEvent(QEvent *event) {
            return forwardOsrEventToBrowser(
                event,
                devToolsBrowser,
                devToolsOsrItem,
                devToolsPageFocused,
                osrPageFocused
            );
        }

        bool forwardOsrEventToBrowser(
            QEvent *event,
            CefRefPtr<CefBrowser> targetBrowser,
            CefOsrItem *targetItem,
            bool &focused,
            bool &otherFocused
        ) {
            if (!targetBrowser || !targetItem) {
                return false;
            }
            CefRefPtr<CefBrowser> currentBrowser = targetBrowser;
            if (event->type() == QEvent::FocusIn) {
                otherFocused = false;
                focused = true;
                postToCefUi([currentBrowser] { currentBrowser->GetHost()->SetFocus(true); });
                return false;
            }
            if (event->type() == QEvent::FocusOut) {
                focused = false;
                postToCefUi([currentBrowser] { currentBrowser->GetHost()->SetFocus(false); });
                return false;
            }
            if (event->type() == QEvent::MouseMove) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                CefMouseEvent cefEvent;
                const QPoint position = targetItem->browserPosition(mouse->position());
                cefEvent.x = position.x();
                cefEvent.y = position.y();
                cefEvent.modifiers = cefMouseModifiers(mouse->modifiers(), mouse->buttons());
                postToCefUi([currentBrowser, cefEvent] {
                    currentBrowser->GetHost()->SendMouseMoveEvent(cefEvent, false);
                });
                return true;
            }
            if (event->type() == QEvent::HoverMove) {
                auto *hover = static_cast<QHoverEvent *>(event);
                CefMouseEvent cefEvent;
                const QPoint position = targetItem->browserPosition(hover->position());
                cefEvent.x = position.x();
                cefEvent.y = position.y();
                cefEvent.modifiers =
                    cefMouseModifiers(QGuiApplication::keyboardModifiers(), QGuiApplication::mouseButtons());
                postToCefUi([currentBrowser, cefEvent] {
                    currentBrowser->GetHost()->SendMouseMoveEvent(cefEvent, false);
                });
                return true;
            }
            if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) {
                targetItem->unsetCursor();
                CefMouseEvent cefEvent;
                postToCefUi([currentBrowser, cefEvent] {
                    currentBrowser->GetHost()->SendMouseMoveEvent(cefEvent, true);
                });
                return true;
            }
            if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
                event->type() == QEvent::MouseButtonDblClick) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                CefBrowserHost::MouseButtonType buttonType = MBT_LEFT;
                if (mouse->button() == Qt::MiddleButton) {
                    buttonType = MBT_MIDDLE;
                } else if (mouse->button() == Qt::RightButton) {
                    buttonType = MBT_RIGHT;
                }
                CefMouseEvent cefEvent;
                const QPoint position = targetItem->browserPosition(mouse->position());
                cefEvent.x = position.x();
                cefEvent.y = position.y();
                cefEvent.modifiers = cefMouseModifiers(mouse->modifiers(), mouse->buttons());
                const bool released = event->type() == QEvent::MouseButtonRelease;
                const int clickCount = event->type() == QEvent::MouseButtonDblClick ? 2 : 1;
                if (!released) {
                    otherFocused = false;
                    focused = true;
                    targetItem->forceActiveFocus(Qt::MouseFocusReason);
                }
                postToCefUi([currentBrowser, cefEvent, buttonType, released, clickCount] {
                    if (!released) {
                        currentBrowser->GetHost()->SetFocus(true);
                    }
                    currentBrowser->GetHost()->SendMouseClickEvent(cefEvent, buttonType, released, clickCount);
                });
                return true;
            }
            if (event->type() == QEvent::Wheel) {
                auto *wheel = static_cast<QWheelEvent *>(event);
                CefMouseEvent cefEvent;
                const QPoint position = targetItem->browserPosition(wheel->position());
                cefEvent.x = position.x();
                cefEvent.y = position.y();
                cefEvent.modifiers = cefMouseModifiers(wheel->modifiers(), wheel->buttons());
                const QPoint delta = wheel->angleDelta();
                postToCefUi([currentBrowser, cefEvent, delta] {
                    currentBrowser->GetHost()->SendMouseWheelEvent(cefEvent, delta.x(), delta.y());
                });
                return true;
            }
            if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
                auto *key = static_cast<QKeyEvent *>(event);
                CefKeyEvent cefEvent;
                cefEvent.type = event->type() == QEvent::KeyPress ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
                cefEvent.modifiers = cefEventModifiers(key->modifiers());
                cefEvent.windows_key_code = cefWindowsKeyCode(key->key());
                cefEvent.native_key_code = static_cast<int>(key->nativeScanCode());
                const QString text = key->text();
                if (!text.isEmpty()) {
                    cefEvent.character = text.front().unicode();
                    cefEvent.unmodified_character = cefEvent.character;
                }
                postToCefUi([currentBrowser, cefEvent, text] {
                    currentBrowser->GetHost()->SendKeyEvent(cefEvent);
                    if (cefEvent.type == KEYEVENT_RAWKEYDOWN && !text.isEmpty() &&
                        !(cefEvent.modifiers & EVENTFLAG_CONTROL_DOWN) && !(cefEvent.modifiers & EVENTFLAG_ALT_DOWN)) {
                        CefKeyEvent characterEvent = cefEvent;
                        characterEvent.type = KEYEVENT_CHAR;
                        characterEvent.windows_key_code = cefEvent.character;
                        characterEvent.native_key_code = cefEvent.character;
                        currentBrowser->GetHost()->SendKeyEvent(characterEvent);
                    }
                });
                return true;
            }
            return false;
        }

        bool acceptsOsrKeyboardInput() const {
            return osrPageFocused && osrEventWindow && osrEventWindow->isActive() && viewport &&
                   viewport->isVisible() && osrItem && osrItem->isVisible();
        }

        bool acceptsDevToolsOsrKeyboardInput() const {
            return devToolsPageFocused && devToolsEventWindow && devToolsEventWindow->isActive() && devToolsViewport &&
                   devToolsViewport->isVisible() && devToolsOsrItem && devToolsOsrItem->isVisible();
        }

        CefEngineView *q;
        QPointer<CefProfile> profile;
        bool osr = false;
        QPointer<QQuickItem> viewport;
        QPointer<QWindow> hostWindow;
        QPointer<CefOsrItem> osrItem;
        QPointer<QQuickWindow> osrEventWindow;
        QList<QMetaObject::Connection> viewportConnections;
        QPointer<QQuickItem> devToolsViewport;
        QPointer<CefOsrItem> devToolsOsrItem;
        QPointer<QQuickWindow> devToolsEventWindow;
        QList<QMetaObject::Connection> devToolsViewportConnections;
        std::shared_ptr<CefViewLifetime> lifetime;
        CefRefPtr<CefEngineClient> client;
        CefRefPtr<CefBrowser> browser;
        std::optional<bool> browserVisible;
        std::shared_ptr<CefViewLifetime> devToolsLifetime;
        CefRefPtr<CefDevToolsClient> devToolsClient;
        CefRefPtr<CefBrowser> devToolsBrowser;
        std::optional<bool> devToolsBrowserVisible;
        std::shared_ptr<CefDevToolsProtocolSession> devToolsProtocolSession;
        DevToolsSocketServer::Session devToolsSocketSession;
        QUrl pendingUrl = QUrl("about:blank");
        QUrl url;
        QHash<QString, AutofillTargetCallback> autofillRequests;
        QUrl faviconUrl;
        QString title;
        ContextMenuInfo lastContextMenu;
        int rendererClientId = 0;
        mutable qint64 cachedRendererPid = 0;
        int historyCurrentIndex = -1;
        QVariantList historyEntries;
        QVariantMap certificate;
        quint64 nextFileChooserId = Q_UINT64_C(1) << 32;
        QHash<quint64, QPair<qint64, QString>> fileChooserNodes;
        CefRefPtr<CefRegistration> fileChooserRegistration;
        int progress = 0;
        bool loading = false;
        bool canGoBack = false;
        bool canGoForward = false;
        bool audible = false;
        bool muted = false;
        bool browserCreationStarted = false;
        bool loadPendingAfterCreation = false;
        bool osrPageFocused = false;
        bool devToolsBrowserCreationStarted = false;
        bool devToolsPageFocused = false;
        bool osrApplicationFilterInstalled = false;
    };

    void CefFileChooserObserver::OnDevToolsEvent(
        CefRefPtr<CefBrowser>,
        const CefString &method,
        const void *parametersData,
        size_t parametersSize
    ) {
        if (method != "Page.fileChooserOpened" || !parametersData || parametersSize == 0) {
            return;
        }
        const QByteArray payload(static_cast<const char *>(parametersData), static_cast<qsizetype>(parametersSize));
        const QJsonObject parameters = QJsonDocument::fromJson(payload).object();
        const QString mode = parameters.value("mode").toString();
        const qint64 backendNodeId = parameters.value("backendNodeId").toVariant().toLongLong();
        const QString frameId = parameters.value("frameId").toString();
        if (backendNodeId <= 0) {
            return;
        }
        const std::shared_ptr<CefViewLifetime> lifetime = m_lifetime;
        CefUiBridge::runOnUiThread([lifetime, mode, backendNodeId, frameId] {
            lifetime->runOrQueue([mode, backendNodeId, frameId](void *state) {
                static_cast<CefEngineView::Private *>(state)->fileChooserOpened(mode, backendNodeId, frameId);
            });
        });
    }

    bool
    CefDevToolsMessageForwarder::OnDevToolsMessage(CefRefPtr<CefBrowser>, const void *message, size_t messageSize) {
        if (!message || messageSize == 0) {
            return false;
        }
        if (const std::shared_ptr<CefDevToolsProtocolSession> session = m_session.lock()) {
            session->deliverToFrontend(
                QByteArray(static_cast<const char *>(message), static_cast<qsizetype>(messageSize))
            );
            return true;
        }
        return false;
    }

    bool CefDevToolsProtocolSession::start(const QString &token, std::shared_ptr<CefViewLifetime> lifetime) {
        m_token = token;
        const std::shared_ptr<CefDevToolsProtocolSession> self = shared_from_this();
        return postToCefUi([self, lifetime = std::move(lifetime)] {
            if (self->m_closed.load(std::memory_order_acquire) || !self->m_browser) {
                return;
            }
            self->m_observer = new CefDevToolsMessageForwarder(self);
            self->m_registration = self->m_browser->GetHost()->AddDevToolsMessageObserver(self->m_observer);
            const bool registered = self->m_registration != nullptr;
            CefUiBridge::runOnUiThread([lifetime, registered] {
                lifetime->runOrQueue([registered](void *state) {
                    auto &view = *static_cast<CefEngineView::Private *>(state);
                    if (registered) {
                        view.createDevToolsFrontendBrowser();
                    } else {
                        view.devToolsCreationFailed();
                    }
                });
            });
        });
    }

    void CefDevToolsProtocolSession::sendToInspected(QByteArray message) {
        if (m_closed.load(std::memory_order_acquire) || message.isEmpty()) {
            return;
        }
        const std::shared_ptr<CefDevToolsProtocolSession> self = shared_from_this();
        postToCefUi([self, message = std::move(message)] {
            if (!self->m_closed.load(std::memory_order_acquire) && self->m_browser) {
                self->m_browser->GetHost()->SendDevToolsMessage(
                    message.constData(),
                    static_cast<size_t>(message.size())
                );
            }
        });
    }

    void CefDevToolsProtocolSession::deliverToFrontend(QByteArray message) {
        if (m_closed.load(std::memory_order_acquire)) {
            return;
        }
        const QString token = m_token;
        CefUiBridge::runOnUiThread([token, message = std::move(message)]() mutable {
            sharedDevToolsSocketServer()->sendMessage(token, message);
        });
    }

    void CefDevToolsProtocolSession::close() {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const std::shared_ptr<CefDevToolsProtocolSession> self = shared_from_this();
        postToCefUi([self] {
            self->m_registration = nullptr;
            self->m_observer = nullptr;
            self->m_browser = nullptr;
        });
    }

    void CefDevToolsClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
        bool closeBrowser = false;
        {
            const std::lock_guard lock(m_browserMutex);
            m_creationPending = false;
            m_browser = browser;
            if (m_closeRequested && !m_closeIssued) {
                m_closeIssued = true;
                closeBrowser = true;
            }
        }
        dispatch([browser](CefEngineView::Private &state) { state.devToolsBrowserCreated(browser); });
        if (closeBrowser) {
            browser->GetHost()->CloseBrowser(true);
        }
    }

    void CefDevToolsClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
        OnPopupShow(browser, false);
        const int identifier = browser->GetIdentifier();
        dismissContextMenu();
        {
            const std::lock_guard lock(m_frameMutex);
            m_pendingFrame = {};
            m_frameDeliveryQueued = false;
        }
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_browser && m_browser->IsSame(browser)) {
                m_browser = nullptr;
            }
            m_creationPending = false;
        }
        dispatch([identifier](CefEngineView::Private &state) { state.devToolsBrowserClosed(identifier); });
        finishBrowserClose();
    }

    bool CefDevToolsClient::OnCursorChange(
        CefRefPtr<CefBrowser>,
        CefCursorHandle,
        cef_cursor_type_t type,
        const CefCursorInfo &
    ) {
        if (type == CT_CUSTOM) {
            return false;
        }
        dispatch([type](CefEngineView::Private &state) { state.updateDevToolsCursor(type); });
        return true;
    }

    void CefDevToolsClient::OnTakeFocus(CefRefPtr<CefBrowser>, bool next) {
        dispatch([next](CefEngineView::Private &state) { state.devToolsTakeFocus(next); });
    }

    void CefDevToolsClient::OnGotFocus(CefRefPtr<CefBrowser>) {
        dispatch([](CefEngineView::Private &state) { state.devToolsBrowserFocused(); });
    }

    bool
    CefDevToolsClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent &event, CefEventHandle, bool *) {
        if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
            return false;
        }
        const QString command = CefEngineClient::shortcutFor(event);
        if (!command.isEmpty()) {
            dispatch([command](CefEngineView::Private &state) { state.requestShortcut(command); });
            return true;
        }
        const bool control = event.modifiers & EVENTFLAG_CONTROL_DOWN;
        const bool blockedModifier = event.modifiers & (EVENTFLAG_ALT_DOWN | EVENTFLAG_COMMAND_DOWN);
        if (!control || blockedModifier) {
            return false;
        }
        CefRefPtr<CefFrame> focusedFrame = browser->GetFocusedFrame();
        if (!focusedFrame) {
            focusedFrame = browser->GetMainFrame();
        }
        if (!focusedFrame) {
            return false;
        }
        int keyCode = event.windows_key_code;
        if (keyCode >= 'a' && keyCode <= 'z') {
            keyCode -= 'a' - 'A';
        }
        if (keyCode == 'V') {
            dispatch([browser](CefEngineView::Private &state) { state.pasteFromShellClipboard(browser); });
        } else if (keyCode == 'X') {
            const QString selectedText = m_selectedText;
            if (!selectedText.isEmpty()) {
                CefUiBridge::runOnUiThread([selectedText] { setMirroredClipboardText(selectedText); });
            }
            focusedFrame->Cut();
        } else if (keyCode == 'C') {
            const QString selectedText = m_selectedText;
            if (!selectedText.isEmpty()) {
                CefUiBridge::runOnUiThread([selectedText] { setMirroredClipboardText(selectedText); });
            }
            focusedFrame->Copy();
        } else if (keyCode == 'A') {
            focusedFrame->SelectAll();
        } else {
            return false;
        }
        return true;
    }

    QVariantList CefDevToolsClient::contextMenuActions(CefRefPtr<CefMenuModel> model) {
        QVariantList actions;
        if (!model) {
            return actions;
        }
        for (size_t index = 0; index < model->GetCount(); ++index) {
            if (!model->IsVisibleAt(index)) {
                continue;
            }
            const auto type = model->GetTypeAt(index);
            if (type == MENUITEMTYPE_SEPARATOR) {
                if (!actions.isEmpty() && !actions.constLast().toMap().value("separator").toBool()) {
                    actions.append(QVariantMap{{"separator", true}, {"id", QString()}, {"title", QString()}});
                }
                continue;
            }
            const QString title = contextMenuLabel(model->GetLabelAt(index));
            if (title.isEmpty()) {
                continue;
            }
            const int commandId = model->GetCommandIdAt(index);
            const QString id = "devtools:" + QString::number(commandId);
            QVariantMap action{
                {"id", id},
                {"title", title},
                {"icon", contextMenuIcon(commandId, title)},
                {"enabled", model->IsEnabledAt(index)},
                {"checked", model->IsCheckedAt(index)}
            };
            if (type == MENUITEMTYPE_SUBMENU) {
                const QVariantList children = contextMenuActions(model->GetSubMenuAt(index));
                if (children.isEmpty()) {
                    continue;
                }
                action.insert("children", children);
            }
            actions.append(action);
            m_contextMenuCommands.insert(id, commandId);
        }
        if (!actions.isEmpty() && actions.constLast().toMap().value("separator").toBool()) {
            actions.removeLast();
        }
        return actions;
    }

    bool CefDevToolsClient::RunContextMenu(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame>,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model,
        CefRefPtr<CefRunContextMenuCallback> callback
    ) {
        dismissContextMenu();
        m_contextMenuCallback = callback;
        m_contextMenuCommands.clear();
        m_contextMenuSelectionText = QString::fromStdString(params->GetSelectionText().ToString());
        if (m_contextMenuSelectionText.isEmpty()) {
            m_contextMenuSelectionText = m_selectedText;
        }
        ContextMenuInfo info;
        info.position = QPoint(params->GetXCoord(), params->GetYCoord());
        info.actions = contextMenuActions(model);
        QVariantList textActions;
        const auto hasCommand = [this](int commandId) {
            return std::any_of(
                m_contextMenuCommands.cbegin(),
                m_contextMenuCommands.cend(),
                [commandId](int candidate) { return candidate == commandId; }
            );
        };
        const auto appendTextAction = [&textActions](const QString &id, const QString &title, const QString &icon) {
            textActions.append(QVariantMap{{"id", id}, {"title", title}, {"icon", icon}, {"enabled", true}});
        };
        const auto editFlags = params->GetEditStateFlags();
        if (params->IsEditable() && (editFlags & CM_EDITFLAG_CAN_CUT) && !hasCommand(MENU_ID_CUT)) {
            appendTextAction("devtools:text-cut", "Cut", "cut");
        }
        if ((!m_contextMenuSelectionText.isEmpty() || (params->IsEditable() && (editFlags & CM_EDITFLAG_CAN_COPY))) &&
            !hasCommand(MENU_ID_COPY)) {
            appendTextAction("devtools:text-copy", "Copy", "copy");
        }
        if (params->IsEditable() && !hasCommand(MENU_ID_PASTE)) {
            appendTextAction("devtools:text-paste", "Paste", "clipboard");
        }
        if (params->IsEditable() && !hasCommand(MENU_ID_SELECT_ALL)) {
            appendTextAction("devtools:text-select-all", "Select all", "text-selection");
        }
        if (!textActions.isEmpty() && !info.actions.isEmpty()) {
            textActions.append(QVariantMap{{"separator", true}, {"id", QString()}, {"title", QString()}});
        }
        textActions.append(info.actions);
        info.actions = std::move(textActions);
        info.surface = "devtools";
        if (info.actions.isEmpty() ||
            !dispatch([info](CefEngineView::Private &state) { state.requestContextMenu(info); })) {
            dismissContextMenu();
        }
        return true;
    }

    void CefDevToolsClient::OnContextMenuDismissed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) {
        m_contextMenuCallback = nullptr;
        m_contextMenuCommands.clear();
        m_contextMenuSelectionText.clear();
    }

    void CefDevToolsClient::executeContextMenuCommand(const QString &command) {
        if (!m_contextMenuCallback) {
            return;
        }
        const auto found = m_contextMenuCommands.constFind(command);
        const int commandId = found == m_contextMenuCommands.cend() ? -1 : found.value();
        CefRefPtr<CefRunContextMenuCallback> callback = m_contextMenuCallback;
        const CefRefPtr<CefBrowser> browser = m_browser;
        const QString selectedText = m_contextMenuSelectionText;
        m_contextMenuCallback = nullptr;
        m_contextMenuCommands.clear();
        m_contextMenuSelectionText.clear();
        CefRefPtr<CefFrame> focusedFrame = browser ? browser->GetFocusedFrame() : nullptr;
        if (!focusedFrame && browser) {
            focusedFrame = browser->GetMainFrame();
        }
        if (command == "devtools:text-copy" || command == "devtools:text-cut") {
            callback->Cancel();
            if (!selectedText.isEmpty()) {
                CefUiBridge::runOnUiThread([selectedText] { setMirroredClipboardText(selectedText); });
            }
            if (command == "devtools:text-cut" && focusedFrame) {
                focusedFrame->Cut();
            }
            return;
        }
        if (command == "devtools:text-paste") {
            callback->Cancel();
            dispatch([browser](CefEngineView::Private &state) { state.pasteFromShellClipboard(browser); });
            return;
        }
        if (command == "devtools:text-select-all") {
            callback->Cancel();
            if (focusedFrame) {
                focusedFrame->SelectAll();
            }
            return;
        }
        if (commandId < 0) {
            callback->Cancel();
            return;
        }
        if ((commandId == MENU_ID_COPY || commandId == MENU_ID_CUT) && !selectedText.isEmpty()) {
            CefUiBridge::runOnUiThread([selectedText] { setMirroredClipboardText(selectedText); });
        }
        if (commandId == MENU_ID_PASTE || commandId == MENU_ID_PASTE_MATCH_STYLE) {
            callback->Cancel();
            dispatch([browser](CefEngineView::Private &state) { state.pasteFromShellClipboard(browser); });
            return;
        }
        callback->Continue(commandId, EVENTFLAG_NONE);
    }

    void CefDevToolsClient::dismissContextMenu() {
        CefRefPtr<CefRunContextMenuCallback> callback = m_contextMenuCallback;
        m_contextMenuCallback = nullptr;
        m_contextMenuCommands.clear();
        m_contextMenuSelectionText.clear();
        if (callback) {
            callback->Cancel();
        }
    }

    void
    CefDevToolsClient::OnTextSelectionChanged(CefRefPtr<CefBrowser>, const CefString &selectedText, const CefRange &) {
        m_selectedText = QString::fromStdString(selectedText.ToString());
    }

    bool CefDevToolsClient::OnBeforeDownload(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefDownloadItem> downloadItem,
        const CefString &suggestedName,
        CefRefPtr<CefBeforeDownloadCallback> callback
    ) {
        return m_owner && m_owner->OnBeforeDownload(browser, downloadItem, suggestedName, callback);
    }

    void CefDevToolsClient::OnDownloadUpdated(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefDownloadItem> downloadItem,
        CefRefPtr<CefDownloadItemCallback> callback
    ) {
        if (m_owner) {
            m_owner->OnDownloadUpdated(browser, downloadItem, callback);
        }
    }

    bool CefDevToolsClient::OnFileDialog(
        CefRefPtr<CefBrowser> browser,
        FileDialogMode mode,
        const CefString &title,
        const CefString &defaultFilePath,
        const std::vector<CefString> &acceptFilters,
        const std::vector<CefString> &acceptExtensions,
        const std::vector<CefString> &acceptDescriptions,
        CefRefPtr<CefFileDialogCallback> callback
    ) {
        return m_owner && m_owner->OnFileDialog(
                              browser,
                              mode,
                              title,
                              defaultFilePath,
                              acceptFilters,
                              acceptExtensions,
                              acceptDescriptions,
                              callback
                          );
    }

    bool CefDevToolsClient::OnProcessMessageReceived(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame>,
        CefProcessId,
        CefRefPtr<CefProcessMessage> message
    ) {
        if (!message || message->GetName() != "eden_clipboard_copy") {
            return false;
        }
        const QString text = QString::fromStdString(message->GetArgumentList()->GetString(0).ToString());
        if (!text.isEmpty()) {
            CefUiBridge::runOnUiThread([text] { setMirroredClipboardText(text); });
        }
        return true;
    }

    void CefDevToolsClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) {
        if (!frame || !frame->IsMain()) {
            return;
        }
        frame->ExecuteJavaScript(
            "(function(){try{"
            "if(!window.InspectorFrontendHost||!InspectorFrontendHost.getPreferences){return;}"
            "InspectorFrontendHost.getPreferences(function(prefs){"
            "if(prefs['screencast-enabled']!=='false'||prefs['screencastEnabled']!=='false'){"
            "InspectorFrontendHost.setPreference('screencast-enabled','false');"
            "InspectorFrontendHost.setPreference('screencastEnabled','false');"
            "location.reload();}});}catch(e){}})()",
            frame->GetURL(),
            0
        );
    }

    bool CefDevToolsClient::GetRootWindowScreenRect(CefRefPtr<CefBrowser>, CefRect &rect) {
        const std::lock_guard lock(m_screenRectMutex);
        if (m_rootWindowScreenRect.width <= 0 || m_rootWindowScreenRect.height <= 0) {
            return false;
        }
        rect = m_rootWindowScreenRect;
        return true;
    }

    void CefDevToolsClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) {
        const std::lock_guard lock(m_screenRectMutex);
        rect = m_osrViewRect;
    }

    bool CefDevToolsClient::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo &screenInfo) {
        const std::lock_guard lock(m_screenRectMutex);
        screenInfo.device_scale_factor = m_deviceScaleFactor;
        screenInfo.rect = m_rootWindowScreenRect;
        screenInfo.available_rect = m_rootWindowScreenRect;
        return true;
    }

    void CefDevToolsClient::OnPopupShow(CefRefPtr<CefBrowser>, bool show) {
        if (m_popup->setVisible(show)) {
            publishPopup();
        }
    }

    void CefDevToolsClient::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect &rect) {
        if (m_popup->setBounds(rect)) {
            publishPopup();
        }
    }

    void CefDevToolsClient::publishPopup() {
        const auto popup = m_popup;
        if (!dispatch([popup](CefEngineView::Private &state) { state.presentDevToolsPopup(popup->take()); })) {
            popup->take();
        }
    }

    void CefDevToolsClient::OnPaint(
        CefRefPtr<CefBrowser>,
        PaintElementType type,
        const RectList &,
        const void *buffer,
        int width,
        int height
    ) {
        if (type == PET_POPUP) {
            if (m_popup->paint(buffer, width, height)) {
                publishPopup();
            }
            return;
        }
        if (type != PET_VIEW) {
            return;
        }
        QImage frame = copyCefFrame(buffer, width, height);
        if (!frame.isNull()) {
            enqueueFrame(std::move(frame));
        }
    }

    void CefDevToolsClient::enqueueFrame(QImage frame) {
        {
            const std::lock_guard lock(m_frameMutex);
            m_pendingFrame = std::move(frame);
            if (m_frameDeliveryQueued) {
                return;
            }
            m_frameDeliveryQueued = true;
        }
        const CefRefPtr<CefDevToolsClient> self(this);
        if (!dispatch([self](CefEngineView::Private &state) {
                QImage frame;
                {
                    const std::lock_guard lock(self->m_frameMutex);
                    frame = std::move(self->m_pendingFrame);
                    self->m_frameDeliveryQueued = false;
                }
                if (!frame.isNull()) {
                    state.presentDevToolsFrame(std::move(frame));
                }
            })) {
            const std::lock_guard lock(m_frameMutex);
            m_pendingFrame = {};
            m_frameDeliveryQueued = false;
        }
    }

    bool CefDevToolsClient::beginBrowserCreation() {
        const std::lock_guard lock(m_browserMutex);
        if (m_closeRequested || m_creationPending || m_browser) {
            return false;
        }
        const CefRefPtr<CefDevToolsClient> self(this);
        if (!CefRuntime::instance().registerBrowserClient(this, [self] { self->requestClose(); })) {
            return false;
        }
        m_creationPending = true;
        return true;
    }

    bool CefDevToolsClient::shouldCreateBrowser() {
        bool canceled = false;
        {
            const std::lock_guard lock(m_browserMutex);
            if (!m_creationPending) {
                return false;
            }
            canceled = m_closeRequested;
            if (canceled) {
                m_creationPending = false;
            }
        }
        if (canceled) {
            finishBrowserClose();
        }
        return !canceled;
    }

    void CefDevToolsClient::browserCreationFailed() {
        {
            const std::lock_guard lock(m_browserMutex);
            m_creationPending = false;
        }
        dispatch([](CefEngineView::Private &state) { state.devToolsCreationFailed(); });
        finishBrowserClose();
    }

    void CefDevToolsClient::requestClose() {
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_closeRequested) {
                return;
            }
            m_closeRequested = true;
            if (!m_browser && !m_creationPending) {
                return;
            }
        }
        const CefRefPtr<CefDevToolsClient> self(this);
        postToCefUi([self] { self->requestCloseOnCefUi(); });
    }

    void CefDevToolsClient::requestCloseOnCefUi() {
        CefRefPtr<CefBrowser> closingBrowser;
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_browser && !m_closeIssued) {
                m_closeIssued = true;
                closingBrowser = m_browser;
            }
        }
        if (closingBrowser) {
            closingBrowser->GetHost()->CloseBrowser(true);
        }
    }

    void CefDevToolsClient::finishBrowserClose() {
        const CefRefPtr<CefDevToolsClient> self(this);
        const auto finish = [self] {
            CefRuntime::instance().releaseBrowserClient(self.get());
        };
        if (!CefPostTask(TID_UI, new CefFunctionTask(finish))) {
            finish();
        }
    }

    void CefDevToolsClient::setGeometry(
        const CefRect &rootWindowScreenRect,
        const CefRect &viewRect,
        float deviceScaleFactor
    ) {
        const std::lock_guard lock(m_screenRectMutex);
        m_rootWindowScreenRect = rootWindowScreenRect;
        m_osrViewRect = viewRect;
        m_deviceScaleFactor = deviceScaleFactor;
    }

    void CefEngineClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
        bool closeBrowser = false;
        {
            const std::lock_guard lock(m_browserMutex);
            m_creationPending = false;
            m_browser = browser;
            if (m_closeRequested && !m_closeIssued) {
                m_closeIssued = true;
                closeBrowser = true;
            }
        }
        dispatch([browser](CefEngineView::Private &state) { state.browserCreated(browser); });
        if (m_popupSource) {
            CefRefPtr<CefEngineClient> source = m_popupSource;
            const int popupId = m_sourcePopupId;
            m_popupSource = nullptr;
            m_sourcePopupId = -1;
            source->popupCreated(popupId);
        }
        if (closeBrowser) {
            browser->GetHost()->CloseBrowser(true);
        }
    }

    void CefEngineClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
        OnPopupShow(browser, false);
        const int identifier = browser->GetIdentifier();
#if EDEN_ENABLE_AUTOMATION
        if (qEnvironmentVariableIsSet("EDEN_IDLE_DIAGNOSTICS")) {
            qInfo(
                "EDEN_PERF diagnostic.osr_paints=%llu",
                static_cast<unsigned long long>(m_idleDiagnosticPaints.load())
            );
        }
#endif
        cancelInteractions();
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_browser && m_browser->IsSame(browser)) {
                m_browser = nullptr;
            }
            m_creationPending = false;
        }
        CefOsrFrame pendingFrame;
        {
            const std::lock_guard lock(m_frameMutex);
            pendingFrame = std::move(m_pendingFrame);
            m_frameDeliveryQueued = false;
        }
        pendingFrame.reset();
        dispatch([identifier](CefEngineView::Private &state) { state.browserClosed(identifier); });
        finishBrowserClose();
    }

    void CefEngineClient::OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString &url) {
        if (!frame || !frame->IsMain()) {
            return;
        }
        QString value = QString::fromStdString(url.ToString());
        if (value == m_errorPageUrl) {
            value = m_failedUrl;
        } else {
            m_errorPageUrl.clear();
            m_failedUrl.clear();
        }
        const bool refreshHistory = !m_mainDocumentCommitted;
        if (m_publishedUrl == value && !refreshHistory) {
            return;
        }
        m_publishedUrl = value;
        dispatch([value, refreshHistory](CefEngineView::Private &state) {
            state.updateUrl(value);
            if (refreshHistory) {
                state.refreshNavigationHistory();
            }
        });
    }

    void CefEngineClient::OnTitleChange(CefRefPtr<CefBrowser>, const CefString &title) {
        const QString value = QString::fromStdString(title.ToString());
        if (m_publishedTitle == value) {
            return;
        }
        m_publishedTitle = value;
#if EDEN_ENABLE_AUTOMATION
        if (value == "bench:ready") {
            eden::core::PerformanceMetrics::record("page.script_ready", 1);
        }
        if (value.startsWith("video:frames:")) {
            bool valid = false;
            const double frame = value.sliced(13).toDouble(&valid);
            if (valid) {
                eden::core::PerformanceMetrics::record("video.frame", frame);
            }
        }
        if (value.startsWith("bench:wheel:") || value.startsWith("bench:scroll:")) {
            const std::optional<double> pageEvent =
                eden::core::PerformanceMetrics::complete("scroll.input_to_page_event_ms");
            if (pageEvent && qEnvironmentVariableIsSet("EDEN_PERF")) {
                qInfo("EDEN_PERF scroll.input_to_page_event_ms=%.3f", *pageEvent);
            }
        }
#endif
        if (qEnvironmentVariableIsSet("EDEN_PERF")) {
            qInfo().noquote() << "EDEN_PERF page.title=" << value;
        }
        dispatch([value](CefEngineView::Private &state) { state.updateTitle(value); });
    }

    class CefFaviconDownloadCallback final : public CefDownloadImageCallback {
      public:
        CefFaviconDownloadCallback(CefRefPtr<CefEngineClient> owner, CefRefPtr<CefBrowser> browser, quint64 serial)
            : m_owner(std::move(owner)),
              m_browser(std::move(browser)),
              m_serial(serial) {}

        void OnDownloadImageFinished(const CefString &, int, CefRefPtr<CefImage> image) override {
            if (m_owner) {
                m_owner->faviconDownloaded(m_browser, m_serial, image);
            }
        }

      private:
        CefRefPtr<CefEngineClient> m_owner;
        CefRefPtr<CefBrowser> m_browser;
        quint64 m_serial;

        IMPLEMENT_REFCOUNTING(CefFaviconDownloadCallback);
    };

    void CefEngineClient::OnFaviconURLChange(CefRefPtr<CefBrowser> browser, const std::vector<CefString> &iconUrls) {
        std::vector<std::string> candidates;
        candidates.reserve(iconUrls.size());
        for (const CefString &iconUrl : iconUrls) {
            candidates.push_back(iconUrl.ToString());
        }
        if (candidates == m_faviconCandidates) {
            return;
        }
        const quint64 serial = ++m_faviconRequestSerial;
        m_faviconCandidates = std::move(candidates);
        m_faviconCandidateIndex = 0;
        if (m_faviconCandidates.empty()) {
            dispatch([](CefEngineView::Private &state) { state.updateFavicon({}); });
            return;
        }
        requestFaviconCandidate(browser, serial);
    }

    void CefEngineClient::requestFaviconCandidate(CefRefPtr<CefBrowser> browser, quint64 serial) {
        if (!browser || serial != m_faviconRequestSerial || m_faviconCandidateIndex >= m_faviconCandidates.size()) {
            return;
        }
        browser->GetHost()->DownloadImage(
            m_faviconCandidates.at(m_faviconCandidateIndex),
            true,
            128,
            false,
            new CefFaviconDownloadCallback(this, browser, serial)
        );
    }

    void CefEngineClient::faviconDownloaded(CefRefPtr<CefBrowser> browser, quint64 serial, CefRefPtr<CefImage> image) {
        if (serial != m_faviconRequestSerial) {
            return;
        }
        QImage decoded;
        if (image && !image->IsEmpty()) {
            int width = 0;
            int height = 0;
            CefRefPtr<CefBinaryValue> encoded = image->GetAsPNG(1.0, true, width, height);
            if (encoded && encoded->GetSize() > 0) {
                QByteArray bytes(static_cast<qsizetype>(encoded->GetSize()), Qt::Uninitialized);
                encoded->GetData(bytes.data(), encoded->GetSize(), 0);
                decoded = QImage::fromData(bytes, "PNG");
            }
        }
        if (!decoded.isNull()) {
            dispatch([decoded](CefEngineView::Private &state) mutable { state.updateFavicon(std::move(decoded)); });
            return;
        }
        ++m_faviconCandidateIndex;
        requestFaviconCandidate(browser, serial);
    }

    void CefEngineClient::OnLoadingProgressChange(CefRefPtr<CefBrowser>, double progress) {
        if (!m_mainDocumentLoading) {
            return;
        }
        const int percentage = std::clamp(qRound(progress * 100.0), 0, 100);
        if (percentage <= m_publishedProgress) {
            return;
        }
        m_publishedProgress = percentage;
        dispatch([percentage](CefEngineView::Private &state) { state.updateProgress(percentage); });
    }

    bool CefEngineClient::OnCursorChange(
        CefRefPtr<CefBrowser>,
        CefCursorHandle,
        cef_cursor_type_t type,
        const CefCursorInfo &
    ) {
        if (type == CT_CUSTOM) {
            return false;
        }
        dispatch([type](CefEngineView::Private &state) { state.updateCursor(type); });
        return true;
    }

    bool CefEngineClient::RunContextMenu(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame>,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model,
        CefRefPtr<CefRunContextMenuCallback> callback
    ) {
        dismissContextMenu();
        m_contextMenuCallback = callback;
        m_contextMenuCommands = {
            {"back", MENU_ID_BACK},
            {"forward", MENU_ID_FORWARD},
            {"reload", MENU_ID_RELOAD},
            {"cut", MENU_ID_CUT},
            {"copy", MENU_ID_COPY},
            {"paste", MENU_ID_PASTE},
            {"select_all", MENU_ID_SELECT_ALL},
            {"view_source", MENU_ID_VIEW_SOURCE}
        };
        if (model) {
            for (size_t index = 0; index < model->GetCount(); ++index) {
                if (!model->IsEnabledAt(index)) {
                    continue;
                }
                const QString label = QString::fromStdString(model->GetLabelAt(index).ToString()).toLower();
                const int commandId = model->GetCommandIdAt(index);
                if (label.contains("open link in new tab")) {
                    m_contextMenuCommands.insert("open_link_new_tab", commandId);
                } else if (label.contains("copy link")) {
                    m_contextMenuCommands.insert("copy_link", commandId);
                } else if (label.contains("save link")) {
                    m_contextMenuCommands.insert("download_link", commandId);
                } else if (label.contains("inspect")) {
                    m_contextMenuCommands.insert("inspect", commandId);
                }
            }
        }
        ContextMenuInfo info;
        info.position = QPoint(params->GetXCoord(), params->GetYCoord());
        info.linkUrl = QUrl(QString::fromStdString(params->GetLinkUrl().ToString()));
        info.mediaUrl = QUrl(QString::fromStdString(params->GetSourceUrl().ToString()));
        info.selectedText = QString::fromStdString(params->GetSelectionText().ToString());
        info.editable = params->IsEditable();
        if (!dispatch([info](CefEngineView::Private &state) { state.requestContextMenu(info); })) {
            dismissContextMenu();
        }
        return true;
    }

    void CefEngineClient::OnContextMenuDismissed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) {
        m_contextMenuCallback = nullptr;
        m_contextMenuCommands.clear();
    }

    bool CefEngineClient::OnBeforeDownload(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefDownloadItem> downloadItem,
        const CefString &suggestedName,
        CefRefPtr<CefBeforeDownloadCallback> callback
    ) {
        if (!downloadItem || !downloadItem->IsValid() || !callback) {
            return false;
        }
        DownloadRecord &download = downloadRecord(downloadItem, QString::fromStdString(suggestedName.ToString()));
        download.beforeDownloadSeen = true;
        if (!m_downloadDirectoryReady) {
            m_downloadDirectoryReady = QDir().mkpath(m_downloadDirectory);
        }
        if (!m_downloadDirectoryReady) {
            download.targetAvailable = false;
            download.state = "failed";
            publishDownload(downloadItem->GetId(), download);
            return false;
        }
        publishDownload(downloadItem->GetId(), download);
        const QString targetPath = download.targetPath;
        const bool terminal = download.state != "downloading";
        callback->Continue(targetPath.toStdString(), false);
        if (terminal) {
            m_downloads.erase(downloadItem->GetId());
            m_announcedDownloads.erase(downloadItem->GetId());
        }
        return true;
    }

    void CefEngineClient::OnDownloadUpdated(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefDownloadItem> downloadItem,
        CefRefPtr<CefDownloadItemCallback>
    ) {
        if (!downloadItem || !downloadItem->IsValid()) {
            return;
        }
        const uint32_t id = downloadItem->GetId();
        DownloadRecord &download = downloadRecord(downloadItem);
        download.receivedBytes = downloadItem->GetReceivedBytes();
        download.totalBytes = downloadItem->GetTotalBytes();
        if (!download.targetAvailable) {
            download.state = "failed";
            publishDownload(id, download);
            if (downloadItem->IsComplete() || downloadItem->IsCanceled() || downloadItem->IsInterrupted()) {
                m_downloads.erase(id);
                m_announcedDownloads.erase(id);
            }
            return;
        }
        download.state = "downloading";
        if (downloadItem->IsComplete()) {
            download.state = "complete";
        } else if (downloadItem->IsCanceled()) {
            download.state = "canceled";
        } else if (downloadItem->IsInterrupted()) {
            download.state = "failed";
        }
        publishDownload(id, download);
        if (download.state != "downloading" && download.beforeDownloadSeen) {
            m_downloads.erase(id);
            m_announcedDownloads.erase(id);
        }
    }

    bool CefEngineClient::GetRootWindowScreenRect(CefRefPtr<CefBrowser>, CefRect &rect) {
        const std::lock_guard lock(m_screenRectMutex);
        if (m_rootWindowScreenRect.width <= 0 || m_rootWindowScreenRect.height <= 0) {
            return false;
        }
        rect = m_rootWindowScreenRect;
        return true;
    }

    void CefEngineClient::OnFullscreenModeChange(CefRefPtr<CefBrowser>, bool fullscreen) {
        dispatch([fullscreen](CefEngineView::Private &state) { state.requestFullscreen(fullscreen); });
    }

    void CefEngineClient::OnTakeFocus(CefRefPtr<CefBrowser>, bool next) {
        dispatch([next](CefEngineView::Private &state) { state.takeFocus(next); });
    }

    void CefEngineClient::OnGotFocus(CefRefPtr<CefBrowser> browser) {
        const CefWindowHandle browserWindow = browser->GetHost()->GetWindowHandle();
        if (!browserWindow) {
            dispatch([](CefEngineView::Private &state) { state.browserFocused(); });
            return;
        }
        Display *display = cef_get_xdisplay();
        Window focusedWindow = 0;
        int revertTo = 0;
        if (!display || !XGetInputFocus(display, &focusedWindow, &revertTo) ||
            !windowContains(display, static_cast<Window>(browser->GetHost()->GetWindowHandle()), focusedWindow)) {
            return;
        }
        dispatch([](CefEngineView::Private &state) { state.browserFocused(); });
    }

    bool
    CefEngineClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser, const CefKeyEvent &event, CefEventHandle, bool *) {
        const CefWindowHandle browserWindow = browser->GetHost()->GetWindowHandle();
        if (browserWindow) {
            Display *display = cef_get_xdisplay();
            Window focusedWindow = 0;
            int revertTo = 0;
            if (display && XGetInputFocus(display, &focusedWindow, &revertTo) &&
                !windowContains(display, static_cast<Window>(browserWindow), focusedWindow)) {
                return true;
            }
        }
        if (event.type != KEYEVENT_RAWKEYDOWN && event.type != KEYEVENT_KEYDOWN) {
            return false;
        }
        if (event.windows_key_code == 27 && browser->GetHost()->IsFullscreen()) {
            browser->GetHost()->ExitFullscreen(true);
            return true;
        }
        const QString command = shortcutFor(event);
        if (!command.isEmpty()) {
            if (command == "focus_omnibox") {
                browser->GetHost()->SetFocus(false);
            }
            dispatch([command](CefEngineView::Private &state) { state.requestShortcut(command); });
            return true;
        }
        const bool control = event.modifiers & EVENTFLAG_CONTROL_DOWN;
        const bool blockedModifier = event.modifiers & (EVENTFLAG_ALT_DOWN | EVENTFLAG_COMMAND_DOWN);
        if (!control || blockedModifier) {
            return false;
        }
        CefRefPtr<CefFrame> focusedFrame = browser->GetFocusedFrame();
        if (!focusedFrame) {
            focusedFrame = browser->GetMainFrame();
        }
        if (!focusedFrame) {
            return false;
        }
        int keyCode = event.windows_key_code;
        if (keyCode >= 'a' && keyCode <= 'z') {
            keyCode -= 'a' - 'A';
        }
        if (keyCode == 'V') {
            dispatch([](CefEngineView::Private &state) { state.pasteFromShellClipboard(); });
        } else if (keyCode == 'X') {
            focusedFrame->Cut();
        } else if (keyCode == 'C') {
            focusedFrame->Copy();
        } else if (keyCode == 'A') {
            focusedFrame->SelectAll();
        } else {
            return false;
        }
        return true;
    }

    void CefEngineClient::publishNavigationState() {
        const bool loading = m_mainDocumentLoading;
        const bool canGoBack = m_canGoBack;
        const bool canGoForward = m_canGoForward;
        if (m_publishedLoading == loading && m_publishedCanGoBack == canGoBack &&
            m_publishedCanGoForward == canGoForward) {
            return;
        }
        if (m_publishedLoading != loading) {
            m_publishedProgress = loading ? 0 : 100;
        }
        m_publishedLoading = loading;
        m_publishedCanGoBack = canGoBack;
        m_publishedCanGoForward = canGoForward;
        dispatch([loading, canGoBack, canGoForward](CefEngineView::Private &state) {
            state.updateLoading(loading, canGoBack, canGoForward);
        });
    }

    void CefEngineClient::OnLoadingStateChange(CefRefPtr<CefBrowser>, bool loading, bool canGoBack, bool canGoForward) {
        m_canGoBack = canGoBack;
        m_canGoForward = canGoForward;
        if (!loading) {
            m_mainDocumentLoading = false;
            m_mainDocumentCommitted = false;
        }
        publishNavigationState();
    }

    bool CefEngineClient::OnBeforeBrowse(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest>,
        bool,
        bool
    ) {
        if (frame && frame->IsMain()) {
            ++m_navigationGeneration;
            m_mainDocumentLoading = true;
            m_mainDocumentCommitted = false;
            publishNavigationState();
        }
        return false;
    }

    void CefEngineClient::OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType) {
        if (!isCurrentMainFrame(browser, frame)) {
            return;
        }
        OnPopupShow(browser, false);
        m_mainDocumentLoading = true;
        m_mainDocumentCommitted = true;
        ++m_navigationGeneration;
        ++m_faviconRequestSerial;
        m_faviconCandidates.clear();
        m_faviconCandidateIndex = 0;
        publishNavigationState();
        dispatch([](CefEngineView::Private &state) { state.updateFavicon({}); });
    }

    void CefEngineClient::OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int) {
        if (isCurrentMainFrame(browser, frame) && m_mainDocumentCommitted) {
            m_mainDocumentCommitted = false;
            m_mainDocumentLoading = false;
#if EDEN_ENABLE_AUTOMATION
            if (!m_benchmarkReadyLogged.exchange(true, std::memory_order_acq_rel)) {
                eden::core::PerformanceMetrics::record("page.ready", 1);
            }
#endif
            const QVariantMap details = certificateDetails(browser);
            dispatch([details](CefEngineView::Private &state) {
                state.updateCertificateDetails(details);
                state.loadFinished();
            });
            publishNavigationState();
            scheduleRendererGarbageCollection();
        }
    }

    void CefEngineClient::scheduleRendererGarbageCollection() {
        const quint64 generation = m_navigationGeneration;
        const CefRefPtr<CefEngineClient> self(this);
        CefPostDelayedTask(
            TID_UI,
            new CefFunctionTask([self, generation] {
                if (self->m_navigationGeneration != generation) {
                    return;
                }
                const CefRefPtr<CefBrowser> browser = self->browserSnapshot();
                if (browser && !browser->IsLoading()) {
                    browser->GetHost()
                        ->ExecuteDevToolsMethod(nextDevToolsMessageId(), "HeapProfiler.collectGarbage", nullptr);
                }
            }),
            1500
        );
    }

    bool CefEngineClient::OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId sourceProcess,
        CefRefPtr<CefProcessMessage> message
    ) {
        if (!message) {
            return false;
        }
        const std::string name = message->GetName().ToString();
        if (name == "eden_autofill_target") {
            if (sourceProcess != PID_RENDERER) {
                return true;
            }
            const CefRefPtr<CefListValue> values = message->GetArgumentList();
            const QString id = QString::fromStdString(values->GetString(0).ToString());
            AutofillTarget target;
            if (isCurrentMainFrame(browser, frame) && values->GetSize() == 4) {
                target = {
                    QUrl(QString::fromStdString(values->GetString(3).ToString())),
                    QUrl(QString::fromStdString(values->GetString(2).ToString())),
                    QString::fromStdString(values->GetString(1).ToString())
                };
                if (!target.isValid() ||
                    target.origin != autofillOrigin(QUrl(QString::fromStdString(frame->GetURL().ToString())))) {
                    target = {};
                }
            }
            dispatch([id, target](CefEngineView::Private &state) { state.resolveAutofillTarget(id, target); });
            return true;
        }
        if (name == "eden_renderer_client_id") {
            if (frame && frame->IsMain()) {
                const std::string frameId = frame->GetIdentifier().ToString();
                const int clientId = message->GetArgumentList()->GetInt(0);
                bool current = false;
                {
                    const std::lock_guard lock(m_framePidMutex);
                    m_framePids[frameId] = clientId;
                    current = frameId == m_mainFrameId;
                }
                if (current) {
                    dispatch([clientId](CefEngineView::Private &state) { state.updateRendererClientId(clientId); });
                }
            }
            return true;
        }
        if (name == "eden_clipboard_copy") {
            const QString text = QString::fromStdString(message->GetArgumentList()->GetString(0).ToString());
            if (!text.isEmpty()) {
                CefUiBridge::runOnUiThread([text] { setMirroredClipboardText(text); });
            }
            return true;
        }
        if (name == "eden_credential_submit" && frame) {
            const QString username = QString::fromStdString(message->GetArgumentList()->GetString(0).ToString());
            const QString password = QString::fromStdString(message->GetArgumentList()->GetString(1).ToString());
            QUrl origin(QString::fromStdString(frame->GetURL().ToString()));
            origin.setPath({});
            origin.setQuery(QString());
            origin.setFragment({});
            dispatch([origin, username, password](CefEngineView::Private &state) {
                state.credentialSubmitted(origin, username, password);
            });
            return true;
        }
        if (name == "eden_form_field_focus" && frame) {
            const QString type = QString::fromStdString(message->GetArgumentList()->GetString(0).ToString());
            const QString fieldName = QString::fromStdString(message->GetArgumentList()->GetString(1).ToString());
            const QString autocomplete = QString::fromStdString(message->GetArgumentList()->GetString(2).ToString());
            const QString value = QString::fromStdString(message->GetArgumentList()->GetString(3).ToString());
            const QRectF rect(
                message->GetArgumentList()->GetDouble(4),
                message->GetArgumentList()->GetDouble(5),
                message->GetArgumentList()->GetDouble(6),
                message->GetArgumentList()->GetDouble(7)
            );
            QUrl origin(QString::fromStdString(frame->GetURL().ToString()));
            origin.setPath({});
            origin.setQuery(QString());
            origin.setFragment({});
            dispatch([origin, type, fieldName, autocomplete, value, rect](CefEngineView::Private &state) {
                state.formFieldFocused(origin, type, fieldName, autocomplete, value, rect);
            });
            return true;
        }
        if (name == "eden_display_capture_request" && frame) {
            CefRefPtr<CefListValue> arguments = message->GetArgumentList();
            const int rendererRequestId = arguments->GetInt(0);
            const bool audioRequested = arguments->GetBool(1);
            QUrl origin(QString::fromStdString(frame->GetURL().ToString()));
            origin.setPath({});
            origin.setQuery(QString());
            origin.setFragment({});
            const quint64 id = m_nextInteractionId++;
            m_displayCaptureRequests.emplace(
                id,
                DisplayCaptureRequest{rendererRequestId, audioRequested, origin, frame}
            );
            DisplayCaptureRequestInfo info;
            info.id = id;
            info.origin = origin;
            info.audioRequested = audioRequested;
            if (!dispatch([info](CefEngineView::Private &state) { state.requestDisplayCapture(info); })) {
                m_displayCaptureRequests.erase(id);
                CefRefPtr<CefProcessMessage> response = CefProcessMessage::Create("eden_display_capture_response");
                response->GetArgumentList()->SetInt(0, rendererRequestId);
                response->GetArgumentList()->SetString(1, {});
                frame->SendProcessMessage(PID_RENDERER, response);
            }
            return true;
        }
        return false;
    }

    void CefEngineClient::OnMainFrameChanged(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefFrame> newFrame) {
        m_displayCaptureApproval.reset();
        int pid = 0;
        {
            const std::lock_guard lock(m_framePidMutex);
            m_mainFrameId = newFrame ? newFrame->GetIdentifier().ToString() : std::string();
            const auto found = m_framePids.find(m_mainFrameId);
            pid = found == m_framePids.cend() ? 0 : found->second;
        }
        if (pid > 0) {
            dispatch([pid](CefEngineView::Private &state) { state.updateRendererClientId(pid); });
        }
    }

    void CefEngineClient::OnFrameDestroyed(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame) {
        if (!frame) {
            return;
        }
        const std::string frameId = frame->GetIdentifier().ToString();
        for (auto iterator = m_displayCaptureRequests.begin(); iterator != m_displayCaptureRequests.end();) {
            if (iterator->second.frame && iterator->second.frame->GetIdentifier().ToString() == frameId) {
                const quint64 id = iterator->first;
                iterator = m_displayCaptureRequests.erase(iterator);
                dispatch([id](CefEngineView::Private &state) { state.closeDisplayCaptureRequest(id); });
            } else {
                ++iterator;
            }
        }
        if (m_displayCaptureApproval && m_displayCaptureApproval->frameId == frameId) {
            m_displayCaptureApproval.reset();
        }
        const std::lock_guard lock(m_framePidMutex);
        m_framePids.erase(frameId);
    }

    void CefEngineClient::OnLoadError(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        ErrorCode errorCode,
        const CefString &errorText,
        const CefString &failedUrl
    ) {
        if (!frame || !frame->IsMain() || errorCode == ERR_ABORTED) {
            return;
        }
        const QString failed = QString::fromStdString(failedUrl.ToString());
        const QString message = QString::fromStdString(errorText.ToString());
        const QString html =
            QString(
                "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
                "content=\"width=device-width,initial-scale=1\">"
                "<title>Page unavailable</title><style>html{color-scheme:light "
                "dark}body{margin:0;min-height:100vh;display:grid;place-items:center;"
                "font-family:system-ui,sans-serif;background:#f6f8f4;color:#1a1c19}.card{box-sizing:border-box;width:"
                "min(560px,calc(100% "
                "- 48px));"
                "padding:40px;border-radius:24px;background:#fff;box-shadow:0 12px 36px #0002}h1{margin:0 0 "
                "12px;font-size:30px}p{line-height:1.55}"
                ".detail{color:#586056;overflow-wrap:anywhere}a{display:inline-block;margin-top:16px;padding:12px "
                "20px;border-radius:24px;background:#315d32;"
                "color:#fff;text-decoration:none;font-weight:650}@media(prefers-color-scheme:dark){body{background:#"
                "10140f;color:#e1e4dc}."
                "card{background:#1b211a}"
                ".detail{color:#bbc6b7}a{background:#a5d6a7;color:#173a1c}}</style></head><body><main "
                "class=card><h1>This page could not "
                "be reached</h1>"
                "<p class=detail>%1</p><p class=detail>%2</p><a href=\"%3\">Try again</a></main></body></html>"
            )
                .arg(failed.toHtmlEscaped(), message.toHtmlEscaped(), failed.toHtmlEscaped());
        const QString dataUrl =
            QString("data:text/html;charset=utf-8,%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(html)));
        m_errorPageUrl = dataUrl;
        m_failedUrl = failed;
        frame->LoadURL(dataUrl.toStdString());
    }

    bool CefEngineClient::OnBeforePopup(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame>,
        int popupId,
        const CefString &targetUrl,
        const CefString &,
        CefLifeSpanHandler::WindowOpenDisposition targetDisposition,
        bool userGesture,
        const CefPopupFeatures &popupFeatures,
        CefWindowInfo &windowInfo,
        CefRefPtr<CefClient> &client,
        CefBrowserSettings &settings,
        CefRefPtr<CefDictionaryValue> &,
        bool *
    ) {
        EngineView::Disposition disposition = EngineView::Disposition::NewForegroundTab;
        if (targetDisposition == CEF_WOD_CURRENT_TAB) {
            disposition = EngineView::Disposition::CurrentTab;
        } else if (targetDisposition == CEF_WOD_NEW_BACKGROUND_TAB) {
            disposition = EngineView::Disposition::NewBackgroundTab;
        } else if (
            targetDisposition == CEF_WOD_NEW_WINDOW || targetDisposition == CEF_WOD_NEW_POPUP || popupFeatures.isPopup
        ) {
            disposition = EngineView::Disposition::NewWindow;
        }
        const QUrl url(QString::fromStdString(targetUrl.ToString()));
        auto transfer = std::make_shared<CefPopupTransfer>();
        transfer->lifetime = std::make_shared<CefViewLifetime>();
        transfer->client = new CefEngineClient(transfer->lifetime, m_downloadDirectory);
        if (!transfer->client->beginBrowserCreation()) {
            return true;
        }
        m_pendingPopups[popupId] = transfer;
        transfer->client->setPopupSource(this, popupId);
        windowInfo.SetAsWindowless(0);
        windowInfo.shared_texture_enabled = false;
        configureAlloyRuntime(windowInfo);
        settings.windowless_frame_rate = 60;
        client = transfer->client;
        if (!dispatch([url, disposition, userGesture, transfer](CefEngineView::Private &state) {
                if (!transfer->canceled.load(std::memory_order_acquire)) {
                    state.requestNewView(url, disposition, userGesture, transfer);
                }
            })) {
            m_pendingPopups.erase(popupId);
            transfer->canceled.store(true, std::memory_order_release);
            transfer->client->abortPopupCreation();
            return true;
        }
        return false;
    }

    void CefEngineClient::OnBeforePopupAborted(CefRefPtr<CefBrowser>, int popupId) {
        const auto found = m_pendingPopups.find(popupId);
        if (found == m_pendingPopups.end()) {
            return;
        }
        const std::shared_ptr<CefPopupTransfer> transfer = found->second;
        m_pendingPopups.erase(found);
        transfer->canceled.store(true, std::memory_order_release);
        if (transfer && transfer->client) {
            transfer->client->abortPopupCreation();
        }
    }

    bool CefEngineClient::OnJSDialog(
        CefRefPtr<CefBrowser>,
        const CefString &originUrl,
        JSDialogType dialogType,
        const CefString &messageText,
        const CefString &defaultPromptText,
        CefRefPtr<CefJSDialogCallback> callback,
        bool &suppressMessage
    ) {
        suppressMessage = false;
        if (!callback) {
            return true;
        }
        const quint64 id = m_nextInteractionId++;
        m_javaScriptDialogs.emplace(id, callback);
        JavaScriptDialogInfo info;
        info.id = id;
        info.origin = QUrl(QString::fromStdString(originUrl.ToString()));
        info.kind = dialogType == JSDIALOGTYPE_PROMPT    ? "prompt"
                    : dialogType == JSDIALOGTYPE_CONFIRM ? "confirm"
                                                         : "alert";
        info.message = QString::fromStdString(messageText.ToString());
        info.defaultText = QString::fromStdString(defaultPromptText.ToString());
        if (!dispatch([info](CefEngineView::Private &state) { state.requestJavaScriptDialog(info); })) {
            m_javaScriptDialogs.erase(id);
            callback->Continue(false, {});
        }
        return true;
    }

    bool CefEngineClient::OnBeforeUnloadDialog(
        CefRefPtr<CefBrowser>,
        const CefString &messageText,
        bool,
        CefRefPtr<CefJSDialogCallback> callback
    ) {
        if (!callback) {
            return true;
        }
        const quint64 id = m_nextInteractionId++;
        m_javaScriptDialogs.emplace(id, callback);
        JavaScriptDialogInfo info;
        info.id = id;
        info.kind = "beforeUnload";
        info.message = QString::fromStdString(messageText.ToString());
        if (info.message.isEmpty()) {
            info.message = "This page is asking you to confirm that you want to leave.";
        }
        if (!dispatch([info](CefEngineView::Private &state) { state.requestJavaScriptDialog(info); })) {
            m_javaScriptDialogs.erase(id);
            callback->Continue(false, {});
        }
        return true;
    }

    void CefEngineClient::OnResetDialogState(CefRefPtr<CefBrowser>) {
        std::map<quint64, CefRefPtr<CefJSDialogCallback>> dialogs;
        dialogs.swap(m_javaScriptDialogs);
        for (const auto &[id, callback] : dialogs) {
            callback->Continue(false, {});
            dispatch([id](CefEngineView::Private &state) { state.closeJavaScriptDialog(id); });
        }
    }

    bool CefEngineClient::OnRequestMediaAccessPermission(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        const CefString &requestingOrigin,
        uint32_t requestedPermissions,
        CefRefPtr<CefMediaAccessCallback> callback
    ) {
        if (!callback) {
            return true;
        }
        QUrl origin(QString::fromStdString(requestingOrigin.ToString()));
        origin.setPath({});
        origin.setQuery(QString());
        origin.setFragment({});
        const uint32_t devicePermissions =
            CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
        const bool displayCapture = (requestedPermissions & CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE) != 0;
        if (m_displayCaptureApproval && frame && displayCapture && (requestedPermissions & devicePermissions) == 0 &&
            m_displayCaptureApproval->origin == origin &&
            m_displayCaptureApproval->frameId == frame->GetIdentifier().ToString()) {
            m_displayCaptureApproval.reset();
            callback->Continue(requestedPermissions);
            return true;
        }
        const quint64 id = m_nextInteractionId++;
        PermissionCallback pending;
        pending.requestedPermissions = requestedPermissions;
        pending.media = callback;
        m_permissionCallbacks.emplace(id, pending);
        PermissionRequestInfo info;
        info.id = id;
        info.origin = origin;
        info.permissions = permissionNames(requestedPermissions, true);
        if (!dispatch([info](CefEngineView::Private &state) { state.requestPermission(info); })) {
            m_permissionCallbacks.erase(id);
            callback->Cancel();
        }
        return true;
    }

    bool CefEngineClient::OnShowPermissionPrompt(
        CefRefPtr<CefBrowser>,
        uint64_t promptId,
        const CefString &requestingOrigin,
        uint32_t requestedPermissions,
        CefRefPtr<CefPermissionPromptCallback> callback
    ) {
        if (!callback) {
            return true;
        }
        const quint64 id = m_nextInteractionId++;
        PermissionCallback pending;
        pending.cefPromptId = promptId;
        pending.requestedPermissions = requestedPermissions;
        pending.prompt = callback;
        m_permissionCallbacks.emplace(id, pending);
        PermissionRequestInfo info;
        info.id = id;
        info.origin = QUrl(QString::fromStdString(requestingOrigin.ToString()));
        info.permissions = permissionNames(requestedPermissions, false);
        if (!dispatch([info](CefEngineView::Private &state) { state.requestPermission(info); })) {
            m_permissionCallbacks.erase(id);
            callback->Continue(CEF_PERMISSION_RESULT_DISMISS);
        }
        return true;
    }

    bool CefEngineClient::OnFileDialog(
        CefRefPtr<CefBrowser>,
        FileDialogMode mode,
        const CefString &title,
        const CefString &defaultFilePath,
        const std::vector<CefString> &acceptFilters,
        const std::vector<CefString> &acceptExtensions,
        const std::vector<CefString> &acceptDescriptions,
        CefRefPtr<CefFileDialogCallback> callback
    ) {
        if (!callback) {
            return true;
        }
        const quint64 id = m_nextInteractionId++;
        m_fileDialogs.emplace(id, callback);
        FileDialogInfo info;
        info.id = id;
        info.title = QString::fromStdString(title.ToString());
        if (mode == FILE_DIALOG_OPEN_MULTIPLE) {
            info.mode = "openMultiple";
        } else if (mode == FILE_DIALOG_OPEN_FOLDER) {
            info.mode = "folder";
        } else if (mode == FILE_DIALOG_SAVE) {
            info.mode = "save";
        } else {
            info.mode = "open";
        }
        info.defaultPath = QString::fromStdString(defaultFilePath.ToString());
        for (std::size_t index = 0; index < acceptFilters.size(); ++index) {
            const QString filter = QString::fromStdString(acceptFilters.at(index).ToString()).trimmed();
            const QString extensions = index < acceptExtensions.size()
                                           ? QString::fromStdString(acceptExtensions.at(index).ToString())
                                           : QString();
            const QString description = index < acceptDescriptions.size()
                                            ? QString::fromStdString(acceptDescriptions.at(index).ToString()).trimmed()
                                            : QString();
            QStringList patterns;
            const QStringList extensionList = extensions.split(';', Qt::SkipEmptyParts);
            for (const QString &extension : extensionList) {
                const QString trimmed = extension.trimmed();
                if (trimmed.startsWith('.')) {
                    patterns.append("*" + trimmed);
                }
            }
            if (patterns.isEmpty() && filter.startsWith('.')) {
                patterns.append("*" + filter);
            }
            if (patterns.isEmpty()) {
                continue;
            }
            const QString label = description.isEmpty() ? filter : description;
            info.nameFilters.append(label + " (" + patterns.join(' ') + ")");
        }
        info.nameFilters.append("All files (*)");
        if (!dispatch([info](CefEngineView::Private &state) { state.requestFileDialog(info); })) {
            m_fileDialogs.erase(id);
            callback->Cancel();
        }
        return true;
    }

    void CefEngineClient::resolveFileDialog(quint64 id, bool accepted, const std::vector<CefString> &files) {
        const auto found = m_fileDialogs.find(id);
        if (found == m_fileDialogs.end()) {
            return;
        }
        CefRefPtr<CefFileDialogCallback> callback = found->second;
        m_fileDialogs.erase(found);
        if (accepted && !files.empty()) {
            callback->Continue(files);
        } else {
            callback->Cancel();
        }
    }

    void CefEngineClient::OnDismissPermissionPrompt(
        CefRefPtr<CefBrowser>,
        uint64_t promptId,
        cef_permission_request_result_t
    ) {
        for (auto iterator = m_permissionCallbacks.begin(); iterator != m_permissionCallbacks.end(); ++iterator) {
            if (iterator->second.cefPromptId != promptId || !iterator->second.prompt) {
                continue;
            }
            const quint64 id = iterator->first;
            m_permissionCallbacks.erase(iterator);
            dispatch([id](CefEngineView::Private &state) { state.closePermissionRequest(id); });
            return;
        }
    }

    void CefEngineClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) {
        const std::lock_guard lock(m_screenRectMutex);
        rect = m_osrViewRect;
    }

    bool CefEngineClient::GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo &screenInfo) {
        const std::lock_guard lock(m_screenRectMutex);
        screenInfo.device_scale_factor = m_deviceScaleFactor;
        screenInfo.rect = m_rootWindowScreenRect;
        screenInfo.available_rect = m_rootWindowScreenRect;
        return true;
    }

    void CefEngineClient::OnPopupShow(CefRefPtr<CefBrowser>, bool show) {
        if (m_popup->setVisible(show)) {
            publishPopup();
        }
    }

    void CefEngineClient::OnPopupSize(CefRefPtr<CefBrowser>, const CefRect &rect) {
        if (m_popup->setBounds(rect)) {
            publishPopup();
        }
    }

    void CefEngineClient::publishPopup() {
        const auto popup = m_popup;
        if (!dispatch([popup](CefEngineView::Private &state) { state.presentPopup(popup->take()); })) {
            popup->take();
        }
    }

    void CefEngineClient::OnPaint(
        CefRefPtr<CefBrowser>,
        PaintElementType type,
        const RectList &dirtyRects,
        const void *buffer,
        int width,
        int height
    ) {
        if (type == PET_POPUP) {
            if (m_popup->paint(buffer, width, height)) {
                publishPopup();
            }
            return;
        }
        if (type != PET_VIEW || !buffer || !validCefFrameDimensions(width, height)) {
            return;
        }
#if EDEN_ENABLE_AUTOMATION
        if (qEnvironmentVariableIsSet("EDEN_IDLE_DIAGNOSTICS")) {
            m_idleDiagnosticPaints.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        if (qEnvironmentVariableIsSet("EDEN_PERF") && !m_firstPaintLogged.exchange(true)) {
            bool validStart = false;
            const qint64 startNanoseconds = qEnvironmentVariable("EDEN_START_TIME_NS").toLongLong(&validStart);
            if (validStart) {
                const qint64 nowNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch()
                )
                                                  .count();
                const double milliseconds = static_cast<double>(nowNanoseconds - startNanoseconds) / 1000000.0;
#if EDEN_ENABLE_AUTOMATION
                eden::core::PerformanceMetrics::record("startup.web_first_paint_ms", milliseconds);
#endif
                qInfo("EDEN_PERF startup.web_first_paint_ms=%.3f", milliseconds);
            }
        }
        const QSize frameSize(width, height);
        const QRect frameBounds(QPoint(), frameSize);
        CefOsrFrame supersededFrame;
        {
            const std::lock_guard lock(m_frameMutex);
            if (m_pendingFrame.owner) {
                supersededFrame = std::move(m_pendingFrame);
            }
        }
        supersededFrame.reset();
        int stagingIndex = -1;
        QImage stagedFrame;
        {
            std::unique_lock lock(m_stagingMutex);
            QRegion incomingDirtyRegion;
            for (const CefRect &dirtyRect : dirtyRects) {
                incomingDirtyRegion += QRect(dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height) & frameBounds;
            }
            if (incomingDirtyRegion.isEmpty()) {
                incomingDirtyRegion = frameBounds;
            }
            if (m_stagingFrameSize != frameSize) {
                for (QRegion &region : m_stagingDirtyRegions) {
                    region = frameBounds;
                }
                m_stagingFrameSize = frameSize;
            } else {
                for (QRegion &region : m_stagingDirtyRegions) {
                    region += incomingDirtyRegion;
                }
            }
            for (int offset = 0; offset < static_cast<int>(m_stagingFrames.size()); ++offset) {
                const int candidate = (m_nextStagingFrame + offset) % static_cast<int>(m_stagingFrames.size());
                if (m_stagingFrameAvailable[candidate]) {
                    stagingIndex = candidate;
                    break;
                }
            }
            if (stagingIndex < 0) {
                lock.unlock();
                enqueueCopiedFrame(buffer, frameSize, incomingDirtyRegion);
                return;
            }
            QImage &target = m_stagingFrames[stagingIndex];
            if (target.size() != frameSize) {
                target = QImage(frameSize, QImage::Format_ARGB32);
                m_stagingDirtyRegions[stagingIndex] = frameBounds;
            }
            if (target.isNull()) {
                return;
            }
            const QRegion copyRegion = m_stagingDirtyRegions[stagingIndex] & frameBounds;
            updateCefFrame(target, buffer, copyRegion);
            m_stagingDirtyRegions[stagingIndex] = {};
            m_stagingFrameAvailable[stagingIndex] = false;
            m_nextStagingFrame = (stagingIndex + 1) % static_cast<int>(m_stagingFrames.size());
            stagedFrame = target;
        }
#if EDEN_ENABLE_AUTOMATION
        eden::core::PerformanceMetrics::markReady("scroll.input_to_frame_ms");
#endif
        enqueueFrame(CefOsrFrame(std::move(stagedFrame), this, stagingIndex));
    }

    void CefEngineClient::enqueueFrame(CefOsrFrame frame) {
        CefOsrFrame replacedFrame;
        {
            const std::lock_guard lock(m_frameMutex);
            replacedFrame = std::move(m_pendingFrame);
            m_pendingFrame = std::move(frame);
            if (m_frameDeliveryQueued) {
                return;
            }
            m_frameDeliveryQueued = true;
        }
        dispatchPendingFrame();
    }

    void CefEngineClient::enqueueCopiedFrame(const void *buffer, const QSize &size, const QRegion &dirtyRegion) {
        CefOsrFrame replacedFrame;
        bool queueDelivery = false;
        {
            const std::lock_guard lock(m_frameMutex);
            if (m_pendingFrame.owner || m_pendingFrame.image.size() != size) {
                QImage image = copyCefFrame(buffer, size.width(), size.height());
                if (image.isNull()) {
                    return;
                }
                replacedFrame = std::move(m_pendingFrame);
                m_pendingFrame = CefOsrFrame(std::move(image), nullptr, -1);
            } else {
                updateCefFrame(m_pendingFrame.image, buffer, dirtyRegion);
            }
            queueDelivery = !m_frameDeliveryQueued;
            m_frameDeliveryQueued = true;
        }
#if EDEN_ENABLE_AUTOMATION
        eden::core::PerformanceMetrics::markReady("scroll.input_to_frame_ms");
#endif
        if (queueDelivery) {
            dispatchPendingFrame();
        }
    }

    void CefEngineClient::dispatchPendingFrame() {
        const CefRefPtr<CefEngineClient> self(this);
        if (!dispatch([self](CefEngineView::Private &state) {
                CefOsrFrame frame;
                {
                    const std::lock_guard lock(self->m_frameMutex);
                    frame = std::move(self->m_pendingFrame);
                    self->m_frameDeliveryQueued = false;
                }
                if (!frame.isNull()) {
                    state.presentFrame(std::move(frame));
                }
            })) {
            CefOsrFrame droppedFrame;
            const std::lock_guard lock(m_frameMutex);
            droppedFrame = std::move(m_pendingFrame);
            m_frameDeliveryQueued = false;
        }
    }

    void CefEngineClient::releaseStagingFrame(int index) {
        if (index < 0 || index >= static_cast<int>(m_stagingFrames.size())) {
            return;
        }
        const std::lock_guard lock(m_stagingMutex);
        m_stagingFrameAvailable[index] = true;
    }

    void CefEngineClient::OnAcceleratedPaint(
        CefRefPtr<CefBrowser>,
        PaintElementType type,
        const RectList &,
        const CefAcceleratedPaintInfo &info
    ) {
        if (type != PET_VIEW || !qEnvironmentVariableIsSet("EDEN_PERF")) {
            return;
        }
        qInfo(
            "EDEN_PERF osr.accelerated_paint planes=%d modifier=%llu format=%d width=%d height=%d",
            info.plane_count,
            static_cast<unsigned long long>(info.modifier),
            static_cast<int>(info.format),
            info.extra.coded_size.width,
            info.extra.coded_size.height
        );
    }

    void CefEngineClient::OnRenderProcessTerminated(
        CefRefPtr<CefBrowser> browser,
        TerminationStatus status,
        int errorCode,
        const CefString &errorString
    ) {
        QString statusText;
        if (status == TS_PROCESS_WAS_KILLED) {
            statusText = "killed";
        } else if (status == TS_PROCESS_CRASHED) {
            statusText = "crashed";
        } else if (status == TS_PROCESS_OOM) {
            statusText = "out of memory";
        } else if (status == TS_LAUNCH_FAILED) {
            statusText = "launch failed";
        } else if (status == TS_INTEGRITY_FAILURE) {
            statusText = "integrity failure";
        } else {
            statusText = "terminated";
        }
        const CefRefPtr<CefFrame> frame = browser ? browser->GetMainFrame() : nullptr;
        const QUrl failedUrl(frame ? QString::fromStdString(frame->GetURL().ToString()) : QString());
        const QString error = QString::fromStdString(errorString.ToString());
        const QString details = error.isEmpty() ? QString("%1, code %2").arg(statusText).arg(errorCode)
                                                : QString("%1, code %2, %3").arg(statusText).arg(errorCode).arg(error);
        const QString html =
            QString(
                "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
                "content=\"width=device-width,initial-scale=1\">"
                "<title>Tab crashed</title><style>html{color-scheme:light "
                "dark}body{margin:0;min-height:100vh;display:grid;place-items:center;"
                "font-family:system-ui,sans-serif;background:#f6f8f4;color:#1a1c19}.card{box-sizing:border-box;width:"
                "min(560px,calc(100% "
                "- 48px));padding:40px;border-radius:24px;background:#fff;box-shadow:0 12px 36px #0002}h1{margin:0 0 "
                "12px;font-size:30px}"
                "p{line-height:1.55}.detail{color:#586056;overflow-wrap:anywhere}a{display:inline-block;margin-top:"
                "16px;padding:12px "
                "20px;border-radius:24px;background:#315d32;color:#fff;text-decoration:none;font-weight:650}"
                "@media(prefers-color-scheme:dark){body{background:#10140f;color:#e1e4dc}.card{background:#1b211a}."
                "detail{color:#bbc6b7}"
                "a{background:#a5d6a7;color:#173a1c}}</style></head><body><main class=card><h1>This tab stopped "
                "working</h1>"
                "<p>The page renderer ended unexpectedly. Eden and your other tabs are still running.</p><p "
                "class=detail>%1</p>"
                "<a href=\"%2\">Reload page</a></main></body></html>"
            )
                .arg(details.toHtmlEscaped(), failedUrl.toString().toHtmlEscaped());
        const QString dataUrl =
            QString("data:text/html;charset=utf-8,%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(html)));
        dispatch([failedUrl, details, dataUrl](CefEngineView::Private &state) {
            state.renderProcessTerminated(failedUrl, details, dataUrl);
        });
    }

    bool CefEngineClient::beginBrowserCreation() {
        const std::lock_guard lock(m_browserMutex);
        if (m_closeRequested || m_creationPending || m_browser) {
            return false;
        }
        const CefRefPtr<CefEngineClient> self(this);
        if (!CefRuntime::instance().registerBrowserClient(this, [self] { self->requestClose(); })) {
            return false;
        }
        m_creationPending = true;
        m_nativeClosePending = true;
        return true;
    }

    bool CefEngineClient::shouldCreateBrowser() {
        bool canceled = false;
        {
            const std::lock_guard lock(m_browserMutex);
            if (!m_creationPending) {
                return false;
            }
            canceled = m_closeRequested;
            if (canceled) {
                m_creationPending = false;
            }
        }
        if (canceled) {
            finishBrowserClose();
        }
        return !canceled;
    }

    void CefEngineClient::browserCreationFailed() {
        {
            const std::lock_guard lock(m_browserMutex);
            m_creationPending = false;
        }
        dispatch([](CefEngineView::Private &state) { state.creationFailed(); });
        finishBrowserClose();
    }

    void CefEngineClient::requestClose() {
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_closeRequested) {
                return;
            }
            m_closeRequested = true;
            if (!m_browser && !m_creationPending) {
                return;
            }
        }
        const CefRefPtr<CefEngineClient> self(this);
        postToCefUi([self] { self->requestCloseOnCefUi(); });
    }

    void CefEngineClient::requestCloseOnCefUi() {
        CefRefPtr<CefBrowser> closingBrowser;
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_browser && !m_closeIssued) {
                m_closeIssued = true;
                closingBrowser = m_browser;
            }
        }
        if (closingBrowser) {
            cancelInteractions();
            closingBrowser->GetHost()->CloseBrowser(true);
        }
    }

    void CefEngineClient::finishBrowserClose() {
        const CefRefPtr<CefEngineClient> self(this);
        const auto finish = [self] {
            std::function<void()> windowCleanup;
            {
                const std::lock_guard lock(self->m_browserMutex);
                self->m_nativeClosePending = false;
                windowCleanup = std::move(self->m_windowCleanup);
            }
            if (windowCleanup) {
                CefUiBridge::runOnUiThread(std::move(windowCleanup));
            }
            CefRuntime::instance().releaseBrowserClient(self.get());
        };
        if (!CefPostTask(TID_UI, new CefFunctionTask(finish))) {
            finish();
        }
    }

    void CefEngineClient::retireWindow(QWindow *window) {
        CefUiBridge::assertOnUiThread(window);
        window->hide();
        window->setParent(nullptr);
        auto *owner = new QObject(qApp);
        window->QObject::setParent(owner);
        std::function<void()> cleanup = [guard = QPointer<QObject>(owner)] {
            delete guard.data();
        };
        {
            const std::lock_guard lock(m_browserMutex);
            if (m_nativeClosePending) {
                m_windowCleanup = std::move(cleanup);
                return;
            }
        }
        cleanup();
    }

    CefRefPtr<CefBrowser> CefEngineClient::browserSnapshot() const {
        const std::lock_guard lock(m_browserMutex);
        return m_browser;
    }

    void CefEngineClient::setRootWindowScreenRect(const CefRect &rect) {
        const std::lock_guard lock(m_screenRectMutex);
        m_rootWindowScreenRect = rect;
    }

    void CefEngineClient::setOsrGeometry(const CefRect &rect, float deviceScaleFactor) {
        const std::lock_guard lock(m_screenRectMutex);
        m_osrViewRect = rect;
        m_deviceScaleFactor = deviceScaleFactor;
    }

    QString CefEngineClient::shortcutFor(const CefKeyEvent &event) {
        const bool control = event.modifiers & EVENTFLAG_CONTROL_DOWN;
        const bool shift = event.modifiers & EVENTFLAG_SHIFT_DOWN;
        const bool alt = event.modifiers & EVENTFLAG_ALT_DOWN;
        if (alt) {
            return {};
        }
        if (!control && !shift && event.windows_key_code == 122) {
            return "fullscreen";
        }
        if (!control && !shift && event.windows_key_code == 123) {
            return "devtools";
        }
        if (!control) {
            return {};
        }
        int keyCode = event.windows_key_code;
        if (keyCode >= 'a' && keyCode <= 'z') {
            keyCode -= 'a' - 'A';
        }
        if (keyCode == 9) {
            return shift ? "previous_tab" : "next_tab";
        }
        if (shift && keyCode == 'T') {
            return "restore_tab";
        }
        if (shift && keyCode == 'P') {
            return "private_window";
        }
        if (shift) {
            return {};
        }
        switch (keyCode) {
        case 'T':
            return "new_tab";
        case 'W':
            return "close_tab";
        case 'L':
            return "focus_omnibox";
        case 'D':
            return "bookmark";
        case 'F':
            return "find";
        case 'K':
            return "command_palette";
        case 'H':
            return "history";
        case 'J':
            return "downloads";
        case 'Q':
            return "quit";
        case 188:
            return "settings";
        default:
            return {};
        }
    }

    QStringList CefEngineClient::permissionNames(uint32_t permissions, bool media) {
        QStringList names;
        const auto append = [&names, permissions](uint32_t flag, const QString &name) {
            if (permissions & flag) {
                names.append(name);
            }
        };
        if (media) {
            append(CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE, "microphone");
            append(CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE, "camera");
            append(CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE, "screen audio");
            append(CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE, "screen sharing");
        } else {
            append(CEF_PERMISSION_TYPE_CAMERA_STREAM, "camera");
            append(CEF_PERMISSION_TYPE_CLIPBOARD, "clipboard");
            append(CEF_PERMISSION_TYPE_GEOLOCATION, "location");
            append(CEF_PERMISSION_TYPE_MIC_STREAM, "microphone");
            append(CEF_PERMISSION_TYPE_MIDI_SYSEX, "MIDI devices");
            append(CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS, "multiple downloads");
            append(CEF_PERMISSION_TYPE_NOTIFICATIONS, "notifications");
            append(CEF_PERMISSION_TYPE_POINTER_LOCK, "pointer lock");
            append(CEF_PERMISSION_TYPE_STORAGE_ACCESS, "site storage");
            append(CEF_PERMISSION_TYPE_WINDOW_MANAGEMENT, "window management");
            append(CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS, "files");
        }
        if (names.isEmpty()) {
            names.append("this capability");
        }
        return names;
    }

    QString CefEngineClient::sanitizedDownloadName(const QString &suggestedName) {
        QString name = QFileInfo(suggestedName).fileName();
        for (qsizetype index = 0; index < name.size(); ++index) {
            const ushort value = name.at(index).unicode();
            if (value < 32 || value == 127 || name.at(index) == '/' || name.at(index) == '\\') {
                name[index] = '_';
            }
        }
        if (name.isEmpty() || name == "." || name == "..") {
            return "download";
        }
        return name;
    }

    CefEngineClient::DownloadRecord &
    CefEngineClient::downloadRecord(CefRefPtr<CefDownloadItem> downloadItem, const QString &suggestedName) {
        const uint32_t id = downloadItem->GetId();
        DownloadRecord &download = m_downloads[id];
        if (download.fileName.isEmpty()) {
            const QString candidate = suggestedName.isEmpty()
                                          ? QString::fromStdString(downloadItem->GetSuggestedFileName().ToString())
                                          : suggestedName;
            download.fileName = sanitizedDownloadName(candidate);
            const QFileInfo nameInfo(download.fileName);
            const QString suffix = nameInfo.suffix();
            const QString baseName =
                nameInfo.completeBaseName().isEmpty() ? QString("download") : nameInfo.completeBaseName();
            QString uniqueName = download.fileName;
            int sequence = 0;
            const auto targetReserved = [this, id](const QString &path) {
                if (QFileInfo::exists(path)) {
                    return true;
                }
                for (const auto &[otherId, otherDownload] : m_downloads) {
                    if (otherId != id && otherDownload.targetPath == path) {
                        return true;
                    }
                }
                return false;
            };
            do {
                if (sequence > 0) {
                    uniqueName = suffix.isEmpty() ? QString("%1 (%2)").arg(baseName).arg(sequence)
                                                  : QString("%1 (%2).%3").arg(baseName).arg(sequence).arg(suffix);
                }
                download.targetPath = QDir(m_downloadDirectory).filePath(uniqueName);
                ++sequence;
            } while (targetReserved(download.targetPath));
        }
        download.sourceUrl = QUrl(QString::fromStdString(downloadItem->GetURL().ToString()));
        download.totalBytes = downloadItem->GetTotalBytes();
        return download;
    }

    void CefEngineClient::publishDownload(uint32_t id, DownloadRecord &download) {
        const bool firstUpdate = m_announcedDownloads.insert(id).second;
        const DownloadRecord snapshot = download;
        dispatch([id, snapshot, firstUpdate](CefEngineView::Private &view) {
            view.updateDownload(
                id,
                snapshot.fileName,
                snapshot.sourceUrl,
                snapshot.targetPath,
                snapshot.receivedBytes,
                snapshot.totalBytes,
                snapshot.state,
                firstUpdate
            );
        });
    }

    void CefEngineClient::executeContextMenuCommand(const QString &command) {
        if (!m_contextMenuCallback) {
            return;
        }
        const auto found = m_contextMenuCommands.constFind(command);
        const int commandId = found == m_contextMenuCommands.cend() ? -1 : found.value();
        CefRefPtr<CefRunContextMenuCallback> callback = m_contextMenuCallback;
        m_contextMenuCallback = nullptr;
        m_contextMenuCommands.clear();
        if (commandId < 0) {
            callback->Cancel();
            return;
        }
        callback->Continue(commandId, EVENTFLAG_NONE);
    }

    void CefEngineClient::dismissContextMenu() {
        CefRefPtr<CefRunContextMenuCallback> callback = m_contextMenuCallback;
        m_contextMenuCallback = nullptr;
        m_contextMenuCommands.clear();
        if (callback) {
            callback->Cancel();
        }
    }

    void CefEngineClient::resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text) {
        const auto found = m_javaScriptDialogs.find(id);
        if (found == m_javaScriptDialogs.end()) {
            return;
        }
        CefRefPtr<CefJSDialogCallback> callback = found->second;
        m_javaScriptDialogs.erase(found);
        callback->Continue(accepted, text.toStdString());
    }

    void CefEngineClient::resolvePermissionRequest(quint64 id, cef_permission_request_result_t result) {
        const auto found = m_permissionCallbacks.find(id);
        if (found == m_permissionCallbacks.end()) {
            return;
        }
        PermissionCallback callback = found->second;
        m_permissionCallbacks.erase(found);
        if (callback.media) {
            if (result == CEF_PERMISSION_RESULT_ACCEPT) {
                callback.media->Continue(callback.requestedPermissions);
            } else {
                callback.media->Cancel();
            }
        } else if (callback.prompt) {
            callback.prompt->Continue(result);
        }
    }

    void CefEngineClient::resolveDisplayCaptureRequest(quint64 id, const QString &source) {
        const auto found = m_displayCaptureRequests.find(id);
        if (found == m_displayCaptureRequests.end()) {
            return;
        }
        DisplayCaptureRequest request = found->second;
        m_displayCaptureRequests.erase(found);
        const bool accepted = source == "window" || source == "screen";
        if (accepted && request.frame) {
            m_displayCaptureApproval =
                DisplayCaptureApproval{request.origin, request.frame->GetIdentifier().ToString()};
        }
        if (!request.frame) {
            m_displayCaptureApproval.reset();
            return;
        }
        CefRefPtr<CefProcessMessage> response = CefProcessMessage::Create("eden_display_capture_response");
        response->GetArgumentList()->SetInt(0, request.rendererRequestId);
        response->GetArgumentList()->SetString(1, accepted ? CefString(source.toStdString()) : CefString());
        request.frame->SendProcessMessage(PID_RENDERER, response);
    }

    void CefEngineClient::setPopupSource(CefRefPtr<CefEngineClient> source, int popupId) {
        m_popupSource = std::move(source);
        m_sourcePopupId = popupId;
    }

    void CefEngineClient::popupCreated(int popupId) {
        m_pendingPopups.erase(popupId);
    }

    void CefEngineClient::abortPopupCreation() {
        m_popupSource = nullptr;
        m_sourcePopupId = -1;
        CefRefPtr<CefBrowser> closingBrowser;
        bool creationFailed = false;
        {
            const std::lock_guard lock(m_browserMutex);
            m_closeRequested = true;
            if (m_browser && !m_closeIssued) {
                m_closeIssued = true;
                closingBrowser = m_browser;
            } else if (!m_browser) {
                m_creationPending = false;
                creationFailed = true;
            }
        }
        if (creationFailed) {
            dispatch([](CefEngineView::Private &state) { state.creationFailed(); });
            finishBrowserClose();
        }
        if (closingBrowser) {
            const CefRefPtr<CefEngineClient> self(this);
            postToCefUi([self, closingBrowser] {
                self->cancelInteractions();
                closingBrowser->GetHost()->CloseBrowser(true);
            });
        }
    }

    void CefEngineClient::detachPopupSource() {
        m_popupSource = nullptr;
        m_sourcePopupId = -1;
    }

    void CefEngineClient::cancelInteractions() {
        ++m_navigationGeneration;
        ++m_faviconRequestSerial;
        dismissContextMenu();
        std::map<quint64, CefRefPtr<CefJSDialogCallback>> dialogs;
        dialogs.swap(m_javaScriptDialogs);
        for (const auto &[id, callback] : dialogs) {
            callback->Continue(false, {});
            dispatch([id](CefEngineView::Private &state) { state.closeJavaScriptDialog(id); });
        }
        std::map<quint64, CefRefPtr<CefFileDialogCallback>> fileDialogs;
        fileDialogs.swap(m_fileDialogs);
        for (const auto &[id, callback] : fileDialogs) {
            callback->Cancel();
            dispatch([id](CefEngineView::Private &state) { state.closeFileDialog(id); });
        }
        std::map<quint64, PermissionCallback> permissions;
        permissions.swap(m_permissionCallbacks);
        for (const auto &[id, callback] : permissions) {
            if (callback.media) {
                callback.media->Cancel();
            } else if (callback.prompt) {
                callback.prompt->Continue(CEF_PERMISSION_RESULT_DISMISS);
            }
            dispatch([id](CefEngineView::Private &state) { state.closePermissionRequest(id); });
        }
        std::map<quint64, DisplayCaptureRequest> displayCaptureRequests;
        displayCaptureRequests.swap(m_displayCaptureRequests);
        m_displayCaptureApproval.reset();
        for (const auto &[id, request] : displayCaptureRequests) {
            if (request.frame) {
                CefRefPtr<CefProcessMessage> response = CefProcessMessage::Create("eden_display_capture_response");
                response->GetArgumentList()->SetInt(0, request.rendererRequestId);
                response->GetArgumentList()->SetString(1, CefString());
                request.frame->SendProcessMessage(PID_RENDERER, response);
            }
            dispatch([id](CefEngineView::Private &state) { state.closeDisplayCaptureRequest(id); });
        }
        for (auto iterator = m_pendingPopups.begin(); iterator != m_pendingPopups.end();) {
            const std::shared_ptr<CefPopupTransfer> transfer = iterator->second;
            if (transfer->adopted.load(std::memory_order_acquire)) {
                transfer->client->detachPopupSource();
                iterator = m_pendingPopups.erase(iterator);
            } else {
                transfer->canceled.store(true, std::memory_order_release);
                transfer->client->requestClose();
                ++iterator;
            }
        }
        m_downloads.clear();
        m_announcedDownloads.clear();
    }

    static QUrl aliasEngineInternalUrl(QUrl url) {
        if (url.scheme() == "chrome") {
            url.setScheme("eden");
        }
        return url;
    }

    CefEngineView::CefEngineView(EngineProfile *profile, QObject *parent)
        : EngineView(parent),
          d(std::make_unique<Private>(this, profile)) {}

    CefEngineView::~CefEngineView() = default;

    QUrl CefEngineView::url() const {
        return aliasEngineInternalUrl(d->url.isEmpty() ? d->pendingUrl : d->url);
    }

    QString CefEngineView::title() const {
        return d->title;
    }

    QUrl CefEngineView::faviconUrl() const {
        return d->faviconUrl;
    }

    int CefEngineView::loadProgress() const {
        return d->progress;
    }

    bool CefEngineView::isLoading() const {
        return d->loading;
    }

    bool CefEngineView::canGoBack() const {
        return d->canGoBack;
    }

    bool CefEngineView::canGoForward() const {
        return d->canGoForward;
    }

    bool CefEngineView::isAudible() const {
        return d->audible;
    }

    bool CefEngineView::isMuted() const {
        return d->muted;
    }

    QString CefEngineView::securityState() const {
        const QString scheme = url().scheme();
        if (scheme == "about" || scheme == "data" || scheme == "eden" || scheme.isEmpty()) {
            return "local";
        }
        if (scheme != "https") {
            return "insecure";
        }
        if (d->certificate.isEmpty()) {
            return "secure";
        }
        return d->certificate.value("secure").toBool() && d->certificate.value("certificateStatus").toULongLong() == 0
                   ? "secure"
                   : "insecure";
    }

    QVariantMap CefEngineView::certificateDetails() const {
        return d->certificate;
    }

    QString CefEngineView::backendName() const {
        return "Blink";
    }

    EngineView::Capabilities CefEngineView::capabilities() const {
        return d->osr ? Capabilities(DockedDevtools | ThumbnailCapture | OsrCompositing | PerTabMute)
                      : Capabilities(ThumbnailCapture | PerTabMute);
    }

    static void collectDescendantProcesses(qint64 processId, QList<qint64> &result) {
        const QStringList tasks =
            QDir(QString("/proc/%1/task").arg(processId)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &task : tasks) {
            QFile children(QString("/proc/%1/task/%2/children").arg(processId).arg(task));
            if (!children.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QList<QByteArray> fields = children.readAll().simplified().split(' ');
            for (const QByteArray &field : fields) {
                bool valid = false;
                const qint64 child = field.toLongLong(&valid);
                if (valid && child > 0) {
                    result.append(child);
                    collectDescendantProcesses(child, result);
                }
            }
        }
    }

    static bool cmdlineMatchesRendererClient(qint64 pid, const QByteArray &needle) {
        QFile file(QString("/proc/%1/cmdline").arg(pid));
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        QByteArray cmdline = file.readAll();
        cmdline.replace('\0', ' ');
        cmdline.append(' ');
        return cmdline.contains("--type=renderer ") && cmdline.contains(needle);
    }

    qint64 CefEngineView::rendererProcessId() const {
        if (d->rendererClientId <= 0) {
            return 0;
        }
        const QByteArray needle = "--renderer-client-id=" + QByteArray::number(d->rendererClientId) + " ";
        if (d->cachedRendererPid > 0 && cmdlineMatchesRendererClient(d->cachedRendererPid, needle)) {
            return d->cachedRendererPid;
        }
        d->cachedRendererPid = 0;
        QList<qint64> descendants;
        collectDescendantProcesses(QCoreApplication::applicationPid(), descendants);
        for (qint64 pid : std::as_const(descendants)) {
            if (cmdlineMatchesRendererClient(pid, needle)) {
                d->cachedRendererPid = pid;
                break;
            }
        }
        return d->cachedRendererPid;
    }

    bool CefEngineView::containsPageScenePoint(const QPointF &windowScenePoint) const {
        if (!d->viewport) {
            return false;
        }
        const QPointF localPoint = d->viewport->mapFromScene(windowScenePoint);
        return QRectF(QPointF(), d->viewport->size()).contains(localPoint);
    }

#if EDEN_ENABLE_AUTOMATION
    bool CefEngineView::automationWheel(int delta) {
        return d->automationWheel(delta);
    }
#endif

    QUrl CefEngineView::internalUrlFor(const QUrl &url) const {
        if (url.scheme() != "eden" || url.host().isEmpty()) {
            return {};
        }
        const QString host = url.host();
        if (host == "newtab" || host == "new-tab" || host == "new-tab-page") {
            return QUrl("about:blank");
        }
        QUrl mapped = url;
        mapped.setScheme("chrome");
        return mapped;
    }

    void CefEngineView::load(const QUrl &url) {
        const QUrl destination = url.isEmpty() ? QUrl("about:blank") : url;
        if (d->pendingUrl != destination) {
            d->pendingUrl = destination;
            emit urlChanged();
            emit securityStateChanged();
        }
        if (d->browser) {
            d->browser->GetMainFrame()->LoadURL(destination.toString().toStdString());
        }
    }

    void CefEngineView::back() {
        if (d->browser && d->browser->CanGoBack()) {
            d->browser->GoBack();
        }
    }

    void CefEngineView::forward() {
        if (d->browser && d->browser->CanGoForward()) {
            d->browser->GoForward();
        }
    }

    QVariantList CefEngineView::navigationHistory(int direction, int maximumItems) const {
        QVariantList entries;
        if (direction == 0 || maximumItems <= 0 || d->historyCurrentIndex < 0) {
            return entries;
        }
        const int normalizedDirection = direction < 0 ? -1 : 1;
        const int boundedMaximum = std::min(maximumItems, 100);
        for (int distance = 1; distance <= boundedMaximum; ++distance) {
            const int offset = normalizedDirection * distance;
            const int index = d->historyCurrentIndex + offset;
            if (index < 0 || index >= d->historyEntries.size()) {
                break;
            }
            const QVariantMap item = d->historyEntries.at(index).toMap();
            const QString url =
                aliasEngineInternalUrl(QUrl(item.value("url").toString())).toDisplayString(QUrl::RemovePassword);
            const QString title = item.value("title").toString();
            QVariantMap entry;
            entry.insert("id", offset);
            entry.insert("title", title.isEmpty() ? url : title);
            entry.insert("subtitle", url);
            entries.append(entry);
        }
        return entries;
    }

    void CefEngineView::goToHistoryOffset(int offset) {
        if (!d->browser || offset == 0) {
            return;
        }
        const int index = d->historyCurrentIndex + offset;
        if (d->historyCurrentIndex >= 0 && index >= 0 && index < d->historyEntries.size()) {
            const qint64 entryId = d->historyEntries.at(index).toMap().value("entryId").toLongLong();
            const CefRefPtr<CefBrowser> currentBrowser = d->browser;
            postToCefUi([currentBrowser, entryId] {
                CefRefPtr<CefDictionaryValue> parameters = CefDictionaryValue::Create();
                parameters->SetInt("entryId", static_cast<int>(entryId));
                currentBrowser->GetHost()
                    ->ExecuteDevToolsMethod(nextDevToolsMessageId(), "Page.navigateToHistoryEntry", parameters);
            });
            return;
        }
        d->browser->GetMainFrame()->ExecuteJavaScript(
            QString("history.go(%1)").arg(offset).toStdString(),
            d->url.toString().toStdString(),
            0
        );
    }

    void CefEngineView::reload() {
        if (d->browser) {
            d->browser->Reload();
        }
    }

    void CefEngineView::stop() {
        if (d->browser) {
            d->browser->StopLoad();
        }
    }

    void CefEngineView::requestAutofillTarget(AutofillTargetCallback callback) {
        if (!callback) {
            return;
        }
        if (!d->browser) {
            callback({});
            return;
        }
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        d->autofillRequests.insert(id, std::move(callback));
        const CefRefPtr<CefBrowser> browser = d->browser;
        const std::shared_ptr<CefViewLifetime> lifetime = d->lifetime;
        if (!postToCefUi([browser, lifetime, id] {
                const CefRefPtr<CefFrame> frame = browser->GetMainFrame();
                if (frame && frame->IsValid()) {
                    const CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_get_autofill_target");
                    message->GetArgumentList()->SetString(0, id.toStdString());
                    frame->SendProcessMessage(PID_RENDERER, message);
                } else {
                    CefUiBridge::runOnUiThread([lifetime, id] {
                        lifetime->runOrQueue([id](void *state) {
                            static_cast<Private *>(state)->resolveAutofillTarget(id, {});
                        });
                    });
                }
            })) {
            d->resolveAutofillTarget(id, {});
        }
    }

    void CefEngineView::fillCredential(const AutofillTarget &target, const QString &username, const QString &password) {
        if (!d->browser || !target.isValid()) {
            return;
        }
        const QByteArray values = QJsonDocument(QJsonArray{username, password}).toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(values){var fields=Array.from(document.querySelectorAll('input,textarea'));"
                "var tokens=function(f){return String(f.autocomplete||'').toLowerCase().split(/\\s+/);};"
                "var has=function(f,v){return tokens(f).indexOf(v)>=0;};var identity=function(f){return[f.name,"
                "f.id,f.placeholder,f.getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();};"
                "var secret=fields.find(function(f){return!f.disabled&&!f.readOnly&&!has(f,'one-time-code')&&"
                "(f.type==='password'||has(f,'current-password')||"
                "/password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-]*key/.test(identity(f)));});"
                "if(!secret)return;var username=fields.find(function(f){return!f.disabled&&(has(f,'username')||"
                "has(f,'email')||f.type==='email'||/user|email|login|identifier/.test(identity(f)));});"
                "if(!username){var i=fields.indexOf(secret);for(var p=i-1;p>=0;--p){if("
                "/^(text|email|tel|$)/.test(fields[p].type||'')){username=fields[p];break;}}}"
                "var set=function(field,value){if(!field)return;var prototype=field.tagName==='TEXTAREA'?"
                "HTMLTextAreaElement.prototype:HTMLInputElement.prototype;var setter=Object.getOwnPropertyDescriptor("
                "prototype,'value').set;setter.call(field,value);"
                "field.dispatchEvent(new Event('input',{bubbles:true}));"
                "field.dispatchEvent(new Event('change',{bubbles:true}));};"
                "set(username,values[0]);set(secret,values[1]);})(%1)"
            )
                .arg(QString::fromUtf8(values));
        const CefRefPtr<CefBrowser> browser = d->browser;
        postToCefUi([browser, target, script] {
            const CefRefPtr<CefFrame> frame = browser->GetMainFrame();
            if (!frame || !frame->IsValid() ||
                autofillOrigin(QUrl(QString::fromStdString(frame->GetURL().ToString()))) != target.origin) {
                return;
            }
            const CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("eden_fill_credential");
            const CefRefPtr<CefListValue> values = message->GetArgumentList();
            values->SetString(0, target.documentId.toStdString());
            values->SetString(1, target.origin.toString(QUrl::FullyEncoded).toStdString());
            values->SetString(2, script.toStdString());
            frame->SendProcessMessage(PID_RENDERER, message);
        });
    }

    void CefEngineView::fillForm(const QVariantMap &fields) {
        if (!d->browser || fields.isEmpty()) {
            return;
        }
        const QByteArray values = QJsonDocument(QJsonObject::fromVariantMap(fields)).toJson(QJsonDocument::Compact);
        const QString script =
            QStringLiteral(
                "(function(values){var aliases={name:['name','full-name'],email:['email'],phone:["
                "'tel','phone'],addressLine1:['address-line1','street-address'],addressLine2:["
                "'address-line2'],city:['address-level2','city'],region:['address-level1','state',"
                "'province'],postalCode:['postal-code','zip'],country:['country','country-name']};"
                "var normalize=function(v){return String(v||'').toLowerCase().replace(/[^a-z0-9]/g,'');};"
                "var set=function(field,value){if(!field||!value)return;var prototype="
                "field.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
                "var setter=Object.getOwnPropertyDescriptor(prototype,'value').set;"
                "setter.call(field,value);field.dispatchEvent(new Event('input',{bubbles:true}));"
                "field.dispatchEvent(new Event('change',{bubbles:true}));};"
                "Object.keys(aliases).forEach(function(key){var names=aliases[key].map(normalize);"
                "var field=Array.from(document.querySelectorAll('input,textarea')).find(function(item){"
                "var candidates=[item.autocomplete,item.name,item.id,item.placeholder].map(normalize);"
                "return candidates.some(function(candidate){return names.some(function(name){return "
                "candidate.indexOf(name)>=0;});});});set(field,values[key]);});})(%1)"
            )
                .arg(QString::fromUtf8(values));
        d->browser->GetMainFrame()->ExecuteJavaScript(script.toStdString(), d->url.toString().toStdString(), 0);
    }

    void CefEngineView::openDevTools() {
        if (!d->osr) {
            return;
        }
        if (devToolsOpen()) {
            closeDevTools();
            return;
        }
        setDevToolsOpen(true);
        d->createDevToolsBrowser();
    }

    void CefEngineView::closeDevTools() {
        d->destroyDevTools();
        setDevToolsOpen(false);
    }

    void CefEngineView::attachDevTools(QQuickItem *viewport) {
        d->attachDevTools(viewport);
    }

    void CefEngineView::detachDevTools(QQuickItem *viewport) {
        if (viewport && d->devToolsViewport == viewport) {
            d->attachDevTools(nullptr);
        }
    }

    void CefEngineView::findInPage(const QString &text, FindFlags flags) {
        if (!d->browser) {
            return;
        }
        if (text.isEmpty()) {
            d->browser->GetHost()->StopFinding(true);
            return;
        }
        d->browser->GetHost()->Find(text.toStdString(), !(flags & FindBackward), flags & FindCaseSensitive, true);
    }

    void CefEngineView::attach(QQuickItem *viewport) {
        d->attach(viewport);
    }

    void CefEngineView::releaseFocus() {
        d->focusShell();
    }

    void CefEngineView::setMuted(bool muted) {
        if (d->muted == muted) {
            return;
        }
        d->muted = muted;
        if (d->browser) {
            d->browser->GetHost()->SetAudioMuted(muted);
        }
        emit mutedChanged();
    }

    void CefEngineView::executeContextMenuCommand(const QString &command) {
        const CefRefPtr<CefEngineClient> client = d->client;
        const CefRefPtr<CefDevToolsClient> devToolsClient = d->devToolsClient;
        if (d->lastContextMenu.surface == "devtools") {
            postToCefUi([devToolsClient, command] {
                if (devToolsClient) {
                    devToolsClient->executeContextMenuCommand(command);
                }
            });
            return;
        }
        if (command == "inspect") {
            postToCefUi([client] { client->dismissContextMenu(); });
            if (!devToolsOpen()) {
                openDevTools();
            }
            return;
        }
        if (command == "paste") {
            postToCefUi([client] { client->dismissContextMenu(); });
            d->pasteFromShellClipboard();
            return;
        }
        if (command == "copy_link" && !d->lastContextMenu.linkUrl.isEmpty()) {
            setMirroredClipboardText(d->lastContextMenu.linkUrl.toString());
        }
        postToCefUi([client, command] { client->executeContextMenuCommand(command); });
    }

    void CefEngineView::dismissContextMenu() {
        const CefRefPtr<CefEngineClient> client = d->client;
        const CefRefPtr<CefDevToolsClient> devToolsClient = d->devToolsClient;
        if (d->lastContextMenu.surface == "devtools") {
            postToCefUi([devToolsClient] {
                if (devToolsClient) {
                    devToolsClient->dismissContextMenu();
                }
            });
        } else {
            postToCefUi([client] { client->dismissContextMenu(); });
        }
    }

    void CefEngineView::resolveJavaScriptDialog(quint64 id, bool accepted, const QString &text) {
        const CefRefPtr<CefEngineClient> client = d->client;
        postToCefUi([client, id, accepted, text] { client->resolveJavaScriptDialog(id, accepted, text); });
    }

    void CefEngineView::resolvePermissionRequest(quint64 id, bool allowed) {
        const CefRefPtr<CefEngineClient> client = d->client;
        const auto result = allowed ? CEF_PERMISSION_RESULT_ACCEPT : CEF_PERMISSION_RESULT_DENY;
        postToCefUi([client, id, result] { client->resolvePermissionRequest(id, result); });
    }

    void CefEngineView::dismissPermissionRequest(quint64 id) {
        const CefRefPtr<CefEngineClient> client = d->client;
        postToCefUi([client, id] { client->resolvePermissionRequest(id, CEF_PERMISSION_RESULT_DISMISS); });
    }

    void CefEngineView::resolveDisplayCaptureRequest(quint64 id, const QString &source) {
        const CefRefPtr<CefEngineClient> client = d->client;
        postToCefUi([client, id, source] { client->resolveDisplayCaptureRequest(id, source); });
    }

    void CefEngineView::resolveFileDialog(quint64 id, bool accepted, const QList<QUrl> &files) {
        if (d->resolveFileChooser(id, accepted, files)) {
            return;
        }
        std::vector<CefString> paths;
        paths.reserve(static_cast<std::size_t>(files.size()));
        for (const QUrl &file : files) {
            const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
            if (!path.isEmpty()) {
                paths.emplace_back(path.toStdString());
            }
        }
        const CefRefPtr<CefEngineClient> client = d->client;
        postToCefUi([client, id, accepted, paths] { client->resolveFileDialog(id, accepted, paths); });
    }

    void CefEngineView::requestThumbnail(const QSize &size, ThumbnailCallback callback) {
        d->requestThumbnail(size, std::move(callback));
    }

    bool CefEngineView::adoptPopup(const std::shared_ptr<CefPopupTransfer> &transfer) {
        return d->adoptPopup(transfer);
    }

    bool CefEngineView::eventFilter(QObject *watched, QEvent *event) {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            if (d->acceptsDevToolsOsrKeyboardInput()) {
                return d->forwardDevToolsOsrEvent(event);
            }
            if (d->acceptsOsrKeyboardInput()) {
                return d->forwardOsrEvent(event);
            }
        }
        if (watched == d->hostWindow && (event->type() == QEvent::FocusIn || event->type() == QEvent::WindowActivate)) {
            d->syncFocus();
        }
        return EngineView::eventFilter(watched, event);
    }

}
