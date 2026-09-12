#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QUrl>

namespace eden::core {

    class OmniboxController;

    class OmniboxInput final : public QObject {
        Q_OBJECT
        Q_PROPERTY(QQuickItem *input READ input WRITE setInput NOTIFY inputChanged)
        Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY stateChanged)
        Q_PROPERTY(bool popupOpen READ popupOpen NOTIFY stateChanged)

      public:
        explicit OmniboxInput(OmniboxController *controller);
        QQuickItem *input() const;
        void setInput(QQuickItem *input);
        int selectedIndex() const;
        bool popupOpen() const;
        Q_INVOKABLE void begin(const QString &url);
        Q_INVOKABLE void finish(const QString &url);
        Q_INVOKABLE void dismiss();
        Q_INVOKABLE void activate(int row);

      signals:
        void inputChanged();
        void stateChanged();
        void navigationRequested(const QUrl &url);
        void tabRequested(int index);
        void editingFinished();

      protected:
        bool eventFilter(QObject *watched, QEvent *event) override;

      private slots:
        void textEdited();
        void applyEdit();
        void selectionChanged();
        void composingChanged();

      private:
        struct Edit {
            QString text;
            int cursor = 0;
            int start = 0;
            int end = 0;
        };

        QString text() const;
        bool hasCompletion() const;
        void render(const QString &text, int start, int end);
        void showDefault();
        void moveSelection(int delta);
        void rememberEdit();
        void restoreEdit(int delta);
        void suppressCompletion();
        void accept(bool controlEnter);
        void suggestionsChanging();
        void suggestionsChanged();

        OmniboxController *m_controller;
        QPointer<QQuickItem> m_input;
        QList<Edit> m_edits;
        int m_editIndex = -1;
        int m_selectedIndex = -1;
        QUrl m_selectedUrl;
        QString m_selectedKind;
        QString m_inlineText;
        bool m_editing = false;
        bool m_popupOpen = false;
        bool m_rendering = false;
        bool m_updatingQuery = false;
        bool m_suppressNextCompletion = false;
        bool m_explicitSelection = false;
        bool m_editPending = false;
        bool m_pendingCompletion = false;
    };

}
