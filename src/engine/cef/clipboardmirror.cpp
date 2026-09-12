#include "engine/cef/clipboardmirror.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPointer>
#include <QScopedValueRollback>

namespace eden::engine::cef {

    ClipboardMirror::ClipboardMirror(QObject *parent)
        : QObject(parent) {
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            connect(clipboard, &QClipboard::changed, this, [this](QClipboard::Mode mode) {
                if (mode == QClipboard::Clipboard && !m_writing) {
                    m_pendingToken.clear();
                    m_mirroredText.clear();
                }
            });
        }
    }

    ClipboardMirror &ClipboardMirror::instance() {
        static QPointer<ClipboardMirror> mirror;
        if (!mirror) {
            mirror = new ClipboardMirror(qGuiApp);
        }
        return *mirror;
    }

    void ClipboardMirror::begin(const QString &token) {
        m_pendingToken = token;
    }

    void ClipboardMirror::commit(const QString &token, const QString &text) {
        if (!token.isEmpty() && token == m_pendingToken) {
            setText(text);
        }
    }

    void ClipboardMirror::setText(const QString &text) {
        m_pendingToken.clear();
        m_mirroredText = text;
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            const QScopedValueRollback writing(m_writing, true);
            clipboard->setText(text);
        }
    }

    QString ClipboardMirror::mirroredText() const {
        return m_mirroredText;
    }

}
