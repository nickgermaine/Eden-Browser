#pragma once

#include <QAbstractListModel>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include "core/omni/omniboxinput.h"

class QNetworkReply;

namespace eden::core {

    class BookmarkStore;
    class HistoryStore;
    class ProfileSettings;
    class TabModel;

    class OmniboxController final : public QAbstractListModel {
        Q_OBJECT
        Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
        Q_PROPERTY(QString completionSuffix READ completionSuffix NOTIFY completionSuffixChanged)
        Q_PROPERTY(OmniboxInput *editor READ editor CONSTANT)

      public:
        enum Role { TitleRole = Qt::UserRole + 1, UrlRole, KindRole, ScoreRole };

        explicit OmniboxController(
            TabModel *tabs,
            HistoryStore *history,
            BookmarkStore *bookmarks,
            ProfileSettings *settings = nullptr,
            QObject *parent = nullptr,
            QNetworkAccessManager *network = nullptr
        );
        ~OmniboxController() override;

        int rowCount(const QModelIndex &parent = {}) const override;
        QVariant data(const QModelIndex &index, int role) const override;
        QHash<int, QByteArray> roleNames() const override;

        QString query() const;
        QString completionSuffix() const;
        OmniboxInput *editor() const;
        void setQuery(const QString &query);
        void updateQuery(const QString &query, bool allowCompletion);
        Q_INVOKABLE QUrl destination(const QString &text, bool controlEnter = false) const;
        Q_INVOKABLE QUrl suggestionUrl(int row) const;
        QString suggestionText(int row) const;
        int suggestionTabIndex(int row) const;
        QUrl searchDestination(const QString &text) const;
        static QUrl directUrl(const QString &text);

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
            quint64 tabId = 0;
            QString completion;
        };

        static int fuzzyScore(const QString &needle, const QString &haystack);
        static void sortSuggestions(QList<Suggestion> &suggestions);
        static QString completionFor(const QString &query, const QUrl &url);
        void rebuildLocalSuggestions();
        void cancelRemoteSuggestions();
        bool queryEligibleForRemoteSuggestions(const QString &query) const;
        RemoteSuggestionProvider remoteSuggestionProvider() const;
        bool remoteSuggestionsAvailable() const;
        QUrl remoteSuggestionUrl(RemoteSuggestionProvider provider, const QString &query) const;
        void requestRemoteSuggestions();
        void applyRemoteSuggestions(const QByteArray &payload, quint64 generation);
        void replaceSuggestions(QList<Suggestion> suggestions, const QString &query);
        void updateCompletionSuffix();

        QString searchEngineName() const;
        QString searchUrlTemplate() const;
        bool searchSuggestionsEnabled() const;

        TabModel *m_tabs;
        HistoryStore *m_history;
        BookmarkStore *m_bookmarks;
        ProfileSettings *m_settings;
        OmniboxInput *m_editor;
        QString m_query;
        QString m_suggestionsQuery;
        QString m_completionSuffix;
        QList<Suggestion> m_suggestions;
        QList<Suggestion> m_remoteSuggestions;
        QTimer m_localTimer;
        QTimer m_remoteTimer;
        QNetworkAccessManager *m_network;
        QPointer<QNetworkReply> m_remoteReply;
        quint64 m_generation = 0;
        bool m_allowCompletion = true;
    };

}
