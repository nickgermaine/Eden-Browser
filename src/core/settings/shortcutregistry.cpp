#include "core/settings/shortcutregistry.h"

#include <QShortcut>

#include <algorithm>

namespace eden::core {

ShortcutRegistry::ShortcutRegistry(QObject *parent)
    : QAbstractListModel(parent),
      m_commands({{"new_tab", "New tab", QKeySequence("Ctrl+T")},
                  {"close_tab", "Close tab", QKeySequence("Ctrl+W")},
                  {"restore_tab", "Reopen closed tab", QKeySequence("Ctrl+Shift+T")},
                  {"next_tab", "Next tab", QKeySequence("Ctrl+Tab")},
                  {"previous_tab", "Previous tab", QKeySequence("Ctrl+Shift+Tab")},
                  {"focus_omnibox", "Focus address bar", QKeySequence("Ctrl+L")},
                  {"bookmark", "Bookmark this page", QKeySequence("Ctrl+D")},
                  {"find", "Find in page", QKeySequence("Ctrl+F")},
                  {"private_window", "New private window", QKeySequence("Ctrl+Shift+P")},
                  {"fullscreen", "Toggle fullscreen", QKeySequence("F11")},
                  {"devtools", "Open developer tools", QKeySequence("F12")},
                  {"command_palette", "Open command palette", QKeySequence("Ctrl+K")},
                  {"settings", "Open settings", QKeySequence("Ctrl+,")},
                  {"history", "Open history", QKeySequence("Ctrl+H")},
                  {"downloads", "Open downloads", QKeySequence("Ctrl+J")},
                  {"quit", "Quit Eden", QKeySequence("Ctrl+Q")}}) {
    rebuildVisible();
}

int ShortcutRegistry::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_visible.size();
}

QVariant ShortcutRegistry::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size()) {
        return {};
    }
    const Command &command = m_commands.at(m_visible.at(index.row()));
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
    qDeleteAll(m_shortcuts);
    m_shortcuts.clear();
    m_window = window;
    for (const Command &command : std::as_const(m_commands)) {
        QShortcut *shortcut = new QShortcut(command.key, window);
        connect(shortcut, &QShortcut::activated, this, [this, id = command.id] { execute(id); });
        m_shortcuts.append(shortcut);
    }
}

void ShortcutRegistry::execute(const QString &id) {
    const auto found = std::find_if(m_commands.cbegin(), m_commands.cend(), [&id](const Command &command) { return command.id == id; });
    if (found != m_commands.cend()) {
        emit commandTriggered(id);
    }
}

void ShortcutRegistry::executeRow(int row) {
    if (row < 0 || row >= m_visible.size()) {
        return;
    }
    execute(m_commands.at(m_visible.at(row)).id);
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

void ShortcutRegistry::rebuildVisible() {
    QList<QPair<int, int>> matches;
    for (int index = 0; index < m_commands.size(); ++index) {
        const int score = fuzzyScore(m_query, m_commands.at(index).title);
        if (score >= 0) {
            matches.append({score, index});
        }
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto &left, const auto &right) { return left.first > right.first; });
    beginResetModel();
    m_visible.clear();
    for (const auto &match : matches) {
        m_visible.append(match.second);
    }
    endResetModel();
}

}
