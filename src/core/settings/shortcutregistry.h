#pragma once

#include <QAbstractListModel>
#include <QKeySequence>
#include <QPointer>

class QKeyEvent;

namespace eden::core {

    class ShortcutRegistry final : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)

      public:
        enum Role { IdRole = Qt::UserRole + 1, TitleRole, ShortcutRole };

        explicit ShortcutRegistry(QObject *parent = nullptr);

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString query() const;
        void setQuery(const QString &query);
        Q_INVOKABLE void attach(QObject *window);
        Q_INVOKABLE void execute(const QString &id);
        Q_INVOKABLE void executeRow(int row);
        Q_INVOKABLE static QString shortcutText(const QString &id);
        static QString commandFor(const QKeyEvent *event);

      protected:
        bool eventFilter(QObject *watched, QEvent *event) override;

      signals:
        void queryChanged();
        void commandTriggered(const QString &id);

      private:
        struct Command {
            QString id;
            QString title;
            QKeySequence key;
            QList<QKeySequence> aliases;
        };

        static int fuzzyScore(const QString &query, const QString &text);
        static const QList<Command> &commands();
        void rebuildVisible();

        QList<int> m_visible;
        QPointer<QObject> m_window;
        QString m_query;
    };

}
