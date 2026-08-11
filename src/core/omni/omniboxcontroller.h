#pragma once

#include <QAbstractListModel>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

namespace eden::core {

class BookmarkStore;
class HistoryStore;
class TabModel;

class OmniboxController final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QString completionSuffix READ completionSuffix NOTIFY completionSuffixChanged)

  public:
    enum Role { TitleRole = Qt::UserRole + 1, UrlRole, KindRole, ScoreRole };

    explicit OmniboxController(TabModel *tabs, HistoryStore *history, BookmarkStore *bookmarks, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const;
    QString completionSuffix() const;
    void setQuery(const QString &query);
    Q_INVOKABLE QUrl destination(const QString &text, bool controlEnter = false) const;
    Q_INVOKABLE QUrl suggestionUrl(int row) const;

  signals:
    void queryChanged();
    void completionSuffixChanged();

  private:
    enum class RemoteSuggestionProvider { None, DuckDuckGo, Google };

    struct Suggestion {
        QString title;
        QUrl url;
        QString kind;
        int score;
    };

    static int fuzzyScore(const QString &needle, const QString &haystack);
    static void sortAndLimit(QList<Suggestion> &suggestions);
    void rebuildLocalSuggestions();
    void cancelRemoteSuggestions();
    bool queryEligibleForRemoteSuggestions(const QString &query) const;
    RemoteSuggestionProvider remoteSuggestionProvider() const;
    bool remoteSuggestionsAvailable() const;
    QUrl remoteSuggestionUrl(RemoteSuggestionProvider provider, const QString &query) const;
    void requestRemoteSuggestions();
    void applyRemoteSuggestions(const QByteArray &payload, quint64 generation);
    void replaceSuggestions(QList<Suggestion> suggestions, const QString &query);
    void clearCompletionSuffix();
    void updateCompletionSuffix();

    TabModel *m_tabs;
    HistoryStore *m_history;
    BookmarkStore *m_bookmarks;
    QString m_query;
    QString m_suggestionsQuery;
    QString m_completionSuffix;
    QList<Suggestion> m_suggestions;
    QTimer m_localTimer;
    QTimer m_remoteTimer;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_remoteReply;
    quint64 m_generation = 0;
};

}
