#include "core/settings/shortcutregistry.h"

#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>

namespace eden::core {

    ShortcutRegistry::ShortcutRegistry(QObject *parent)
        : QAbstractListModel(parent) {
        rebuildVisible();
    }

    const QList<ShortcutRegistry::Command> &ShortcutRegistry::commands() {
        static const QList<Command> values{
            {"new_tab", "New tab", QKeySequence("Ctrl+T")},
            {"new_window", "New window", QKeySequence("Ctrl+N")},
            {"close_tab", "Close tab", QKeySequence("Ctrl+W"), {QKeySequence("Ctrl+F4")}},
            {"close_window", "Close window", QKeySequence("Ctrl+Shift+W")},
            {"restore_tab", "Reopen closed tab", QKeySequence("Ctrl+Shift+T")},
            {"next_tab", "Next tab", QKeySequence("Ctrl+Tab"), {QKeySequence("Ctrl+PgDown")}},
            {"previous_tab", "Previous tab", QKeySequence("Ctrl+Shift+Tab"), {QKeySequence("Ctrl+PgUp")}},
            {"activate_tab_1", "Switch to tab 1", QKeySequence("Ctrl+1")},
            {"activate_tab_2", "Switch to tab 2", QKeySequence("Ctrl+2")},
            {"activate_tab_3", "Switch to tab 3", QKeySequence("Ctrl+3")},
            {"activate_tab_4", "Switch to tab 4", QKeySequence("Ctrl+4")},
            {"activate_tab_5", "Switch to tab 5", QKeySequence("Ctrl+5")},
            {"activate_tab_6", "Switch to tab 6", QKeySequence("Ctrl+6")},
            {"activate_tab_7", "Switch to tab 7", QKeySequence("Ctrl+7")},
            {"activate_tab_8", "Switch to tab 8", QKeySequence("Ctrl+8")},
            {"activate_last_tab", "Switch to last tab", QKeySequence("Ctrl+9")},
            {"move_tab_left", "Move tab left", QKeySequence("Ctrl+Shift+PgUp")},
            {"move_tab_right", "Move tab right", QKeySequence("Ctrl+Shift+PgDown")},
            {"back", "Back", QKeySequence("Alt+Left")},
            {"forward", "Forward", QKeySequence("Alt+Right")},
            {"home", "Open home page", QKeySequence("Alt+Home")},
            {"focus_omnibox",
             "Focus address bar",
             QKeySequence("Ctrl+L"),
             {QKeySequence("Alt+D"), QKeySequence("F6"), QKeySequence("Ctrl+K"), QKeySequence("Ctrl+E")}},
            {"reload", "Reload page", QKeySequence("Ctrl+R"), {QKeySequence("F5")}},
            {"reload_bypass_cache",
             "Reload page without cache",
             QKeySequence("Ctrl+Shift+R"),
             {QKeySequence("Shift+F5")}},
            {"zoom_in", "Zoom in", QKeySequence("Ctrl++"), {QKeySequence("Ctrl+=")}},
            {"zoom_out", "Zoom out", QKeySequence("Ctrl+-")},
            {"zoom_reset", "Reset zoom", QKeySequence("Ctrl+0")},
            {"bookmark", "Bookmark this page", QKeySequence("Ctrl+D")},
            {"bookmarks", "Open bookmarks", QKeySequence("Ctrl+Shift+O")},
            {"find", "Find in page", QKeySequence("Ctrl+F"), {QKeySequence("F3")}},
            {"find_next", "Find next match", QKeySequence("Ctrl+G"), {QKeySequence("Ctrl+F3")}},
            {"find_previous",
             "Find previous match",
             QKeySequence("Ctrl+Shift+G"),
             {QKeySequence("Shift+F3"), QKeySequence("Ctrl+Shift+F3")}},
            {"view_source", "View page source", QKeySequence("Ctrl+U")},
            {"private_window", "New private window", QKeySequence("Ctrl+Shift+N")},
            {"fullscreen", "Toggle fullscreen", QKeySequence("F11")},
            {"devtools",
             "Open developer tools",
             QKeySequence("Ctrl+Shift+I"),
             {QKeySequence("F12"), QKeySequence("Ctrl+Shift+J")}},
            {"command_palette", "Open command palette", QKeySequence("Ctrl+Shift+P")},
            {"settings", "Open settings", QKeySequence("Ctrl+,")},
            {"history", "Open history", QKeySequence("Ctrl+H")},
            {"downloads", "Open downloads", QKeySequence("Ctrl+J")},
            {"quit", "Quit Eden", QKeySequence("Ctrl+Q")}
        };
        return values;
    }

    int ShortcutRegistry::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_visible.size();
    }

    QVariant ShortcutRegistry::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size()) {
            return {};
        }
        const Command &command = commands().at(m_visible.at(index.row()));
        if (role == IdRole) {
            return command.id;
        }
        if (role == TitleRole) {
            return command.title;
        }
        if (role == ShortcutRole) {
            return command.key.toString(QKeySequence::NativeText);
        }
        return {};
    }

    QHash<int, QByteArray> ShortcutRegistry::roleNames() const {
        return {{IdRole, "commandId"}, {TitleRole, "title"}, {ShortcutRole, "shortcut"}};
    }

    QString ShortcutRegistry::query() const {
        return m_query;
    }

    void ShortcutRegistry::setQuery(const QString &query) {
        if (m_query == query) {
            return;
        }
        m_query = query;
        emit queryChanged();
        rebuildVisible();
    }

    void ShortcutRegistry::attach(QObject *window) {
        if (!window || m_window == window) {
            return;
        }
        m_window = window;
        QCoreApplication::instance()->installEventFilter(this);
    }

    bool ShortcutRegistry::eventFilter(QObject *watched, QEvent *event) {
        if (!m_window || (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress &&
                          event->type() != QEvent::KeyRelease)) {
            return QAbstractListModel::eventFilter(watched, event);
        }
        const auto *item = qobject_cast<QQuickItem *>(watched);
        if (watched != m_window && (!item || item->window() != m_window)) {
            return QAbstractListModel::eventFilter(watched, event);
        }
        const QString command = commandFor(static_cast<QKeyEvent *>(event));
        if (command.isEmpty()) {
            return QAbstractListModel::eventFilter(watched, event);
        }
        event->accept();
        if (event->type() == QEvent::KeyPress) {
            execute(command);
        }
        return true;
    }

    void ShortcutRegistry::execute(const QString &id) {
        const auto found = std::find_if(commands().cbegin(), commands().cend(), [&id](const Command &command) {
            return command.id == id;
        });
        if (found != commands().cend()) {
            emit commandTriggered(id);
        }
    }

    void ShortcutRegistry::executeRow(int row) {
        if (row < 0 || row >= m_visible.size()) {
            return;
        }
        execute(commands().at(m_visible.at(row)).id);
    }

    QString ShortcutRegistry::shortcutText(const QString &id) {
        for (const Command &command : commands()) {
            if (command.id == id) {
                return command.key.toString(QKeySequence::NativeText);
            }
        }
        static const QHash<QString, QKeySequence> editing{
            {"undo", QKeySequence("Ctrl+Z")},
            {"redo", QKeySequence("Ctrl+Shift+Z")},
            {"cut", QKeySequence("Ctrl+X")},
            {"copy", QKeySequence("Ctrl+C")},
            {"paste", QKeySequence("Ctrl+V")},
            {"paste_plain", QKeySequence("Ctrl+Shift+V")},
            {"select_all", QKeySequence("Ctrl+A")}
        };
        return editing.value(id).toString(QKeySequence::NativeText);
    }

    int ShortcutRegistry::fuzzyScore(const QString &queryValue, const QString &textValue) {
        const QString query = queryValue.toLower();
        const QString text = textValue.toLower();
        if (query.isEmpty()) {
            return 1;
        }
        if (text.startsWith(query)) {
            return 1000 - text.size();
        }
        int cursor = 0;
        int score = 0;
        for (const QChar character : query) {
            cursor = text.indexOf(character, cursor);
            if (cursor < 0) {
                return -1;
            }
            score += 30 - std::min(cursor, 29);
            ++cursor;
        }
        return score;
    }

    QString ShortcutRegistry::commandFor(const QKeyEvent *event) {
        if (!event || event->key() == Qt::Key_unknown) {
            return {};
        }
        Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
        int key = event->key();
        if (key == Qt::Key_Backtab) {
            key = Qt::Key_Tab;
            modifiers |= Qt::ShiftModifier;
        }
        if (key == Qt::Key_Plus) {
            modifiers &= ~Qt::ShiftModifier;
        }
        static const QHash<int, QString> bindings = [] {
            QHash<int, QString> values;
            const auto bind = [&values](const QKeySequence &sequence, const QString &id) {
                if (sequence.count() == 1 && !values.contains(sequence[0].toCombined())) {
                    values.insert(sequence[0].toCombined(), id);
                }
            };
            for (const Command &command : commands()) {
                bind(command.key, command.id);
                for (const QKeySequence &alias : command.aliases) {
                    bind(alias, command.id);
                }
            }
            return values;
        }();
        return bindings.value(QKeyCombination(modifiers, Qt::Key(key)).toCombined());
    }

    void ShortcutRegistry::rebuildVisible() {
        QList<QPair<int, int>> matches;
        for (int index = 0; index < commands().size(); ++index) {
            const int score = fuzzyScore(m_query, commands().at(index).title);
            if (score >= 0) {
                matches.append({score, index});
            }
        }
        std::stable_sort(matches.begin(), matches.end(), [](const auto &left, const auto &right) {
            return left.first > right.first;
        });
        beginResetModel();
        m_visible.clear();
        for (const auto &match : matches) {
            m_visible.append(match.second);
        }
        endResetModel();
    }

}
