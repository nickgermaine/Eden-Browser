#pragma once

#include <QObject>
#include <QString>

namespace eden::engine::cef {

    class ClipboardMirror final : public QObject {
      public:
        static ClipboardMirror &instance();
        void begin(const QString &token);
        void commit(const QString &token, const QString &text);
        void setText(const QString &text);
        QString mirroredText() const;

      private:
        explicit ClipboardMirror(QObject *parent);

        QString m_pendingToken;
        QString m_mirroredText;
        bool m_writing = false;
    };

}
