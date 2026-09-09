#include "core/omni/omniboxcontroller.h"
#include "core/bookmarks/bookmarkstore.h"
#include "core/history/historystore.h"
#include "core/profiles/profilesettings.h"
#include "core/window/tabmodel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>

namespace eden::core {

    OmniboxController::OmniboxController(
        TabModel *tabs,
        HistoryStore *history,
        BookmarkStore *bookmarks,
        ProfileSettings *settings,
        QObject *parent
    )
        : QAbstractListModel(parent),
          m_tabs(tabs),
          m_history(history),
          m_bookmarks(bookmarks),
          m_settings(settings) {
        m_localTimer.setSingleShot(true);
        m_localTimer.setInterval(16);
        connect(&m_localTimer, &QTimer::timeout, this, &OmniboxController::rebuildLocalSuggestions);
        m_remoteTimer.setSingleShot(true);
        m_remoteTimer.setInterval(100);
        connect(&m_remoteTimer, &QTimer::timeout, this, &OmniboxController::requestRemoteSuggestions);
        if (settings) {
            const auto searchSettingsChanged = [this] {
                ++m_generation;
                cancelRemoteSuggestions();
                rebuildLocalSuggestions();
            };
            connect(settings, &ProfileSettings::searchEngineChanged, this, searchSettingsChanged);
            connect(settings, &ProfileSettings::searchUrlChanged, this, searchSettingsChanged);
            connect(settings, &ProfileSettings::searchSuggestionsChanged, this, searchSettingsChanged);
        }
    }

    QString OmniboxController::searchEngineName() const {
        return m_settings ? m_settings->searchEngine() : QStringLiteral("DuckDuckGo");
    }

    QString OmniboxController::searchUrlTemplate() const {
        return m_settings ? m_settings->searchUrl() : QStringLiteral("https://duckduckgo.com/?q=%1");
    }

    bool OmniboxController::searchSuggestionsEnabled() const {
        return m_settings && m_settings->searchSuggestions();
    }

    int OmniboxController::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_suggestions.size();
    }

    QVariant OmniboxController::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_suggestions.size()) {
            return {};
        }
        const Suggestion &suggestion = m_suggestions.at(index.row());
        if (role == TitleRole) {
            return suggestion.title;
        }
        if (role == UrlRole) {
            return suggestion.url;
        }
        if (role == KindRole) {
            return suggestion.kind;
        }
        if (role == ScoreRole) {
            return suggestion.score;
        }
        return {};
    }

    QHash<int, QByteArray> OmniboxController::roleNames() const {
        return {{TitleRole, "title"}, {UrlRole, "url"}, {KindRole, "kind"}, {ScoreRole, "score"}};
    }

    QString OmniboxController::query() const {
        return m_query;
    }

    QString OmniboxController::completionSuffix() const {
        return m_completionSuffix;
    }

    void OmniboxController::setQuery(const QString &query) {
        if (m_query == query) {
            return;
        }
        m_query = query;
        ++m_generation;
        cancelRemoteSuggestions();
        clearCompletionSuffix();
        emit queryChanged();
        const QString trimmed = m_query.trimmed();
        if (trimmed.isEmpty()) {
            m_localTimer.stop();
            replaceSuggestions({}, {});
            return;
        }
        m_localTimer.start();
        if (remoteSuggestionsAvailable() && queryEligibleForRemoteSuggestions(trimmed)) {
            m_remoteTimer.start();
        }
    }

    QUrl OmniboxController::destination(const QString &text, bool controlEnter) const {
        const QString trimmed = text.trimmed();
        if (controlEnter && !trimmed.contains(' ') && !trimmed.contains('.')) {
            return QUrl("https://www." + trimmed + ".com");
        }
        QUrl direct = QUrl::fromUserInput(trimmed);
        const bool looksLikeHost = trimmed.contains('.') && !trimmed.contains(' ');
        const bool hasScheme = QUrl(trimmed).scheme().size() > 1;
        if (looksLikeHost || hasScheme || trimmed == "about:blank") {
            return direct;
        }
        QString searchTemplate = searchUrlTemplate();
        return QUrl(searchTemplate.arg(QString::fromUtf8(QUrl::toPercentEncoding(trimmed))));
    }

    QUrl OmniboxController::suggestionUrl(int row) const {
        if (m_suggestionsQuery != m_query) {
            return destination(m_query);
        }
        if (row < 0 || row >= m_suggestions.size()) {
            return {};
        }
        return m_suggestions.at(row).url;
    }

    int OmniboxController::fuzzyScore(const QString &needleValue, const QString &haystackValue) {
        const QString needle = needleValue.toLower();
        const QString haystack = haystackValue.toLower();
        if (needle.isEmpty() || haystack.isEmpty()) {
            return -1;
        }
        if (haystack == needle) {
            return 1400;
        }
        if (haystack.startsWith(needle)) {
            return 1200 - std::min<qsizetype>(haystack.size() - needle.size(), 200);
        }
        const qsizetype substringPosition = haystack.indexOf(needle);
        if (substringPosition >= 0) {
            const bool wordPrefix = substringPosition == 0 || !haystack.at(substringPosition - 1).isLetterOrNumber();
            return (wordPrefix ? 1000 : 800) - std::min<qsizetype>(substringPosition, 100);
        }
        int position = 0;
        int skipped = 0;
        for (const QChar character : needle) {
            const int matchPosition = haystack.indexOf(character, position);
            if (matchPosition < 0) {
                return -1;
            }
            skipped += matchPosition - position;
            position = matchPosition + 1;
        }
        return std::max(0, 400 - skipped * 4);
    }

    void OmniboxController::sortAndLimit(QList<Suggestion> &suggestions) {
        std::stable_sort(suggestions.begin(), suggestions.end(), [](const Suggestion &left, const Suggestion &right) {
            return left.score > right.score;
        });
        if (suggestions.size() > 12) {
            suggestions.erase(suggestions.begin() + 12, suggestions.end());
        }
    }

    void OmniboxController::rebuildLocalSuggestions() {
        QList<Suggestion> suggestions;
        const QString trimmed = m_query.trimmed();
        if (!trimmed.isEmpty()) {
            for (int row = 0; row < m_tabs->rowCount(); ++row) {
                const QString title = m_tabs->data(m_tabs->index(row), TabModel::TitleRole).toString();
                const QUrl url = m_tabs->data(m_tabs->index(row), TabModel::UrlRole).toUrl();
                const int score = std::max(fuzzyScore(trimmed, title), fuzzyScore(trimmed, url.toString()));
                if (score >= 0) {
                    suggestions.append({title, url, "tab", 300 + score});
                }
            }
            for (const QString &result : m_history->search(trimmed)) {
                const QStringList fields = result.split('\n');
                if (fields.size() == 2) {
                    const int score = std::max(fuzzyScore(trimmed, fields.at(0)), fuzzyScore(trimmed, fields.at(1)));
                    suggestions.append({fields.at(0), QUrl(fields.at(1)), "history", 200 + score});
                }
            }
            for (const QString &result : m_bookmarks->search(trimmed)) {
                const QStringList fields = result.split('\n');
                if (fields.size() == 2) {
                    const int score = std::max(fuzzyScore(trimmed, fields.at(0)), fuzzyScore(trimmed, fields.at(1)));
                    suggestions.append({fields.at(0), QUrl(fields.at(1)), "bookmark", 100 + score});
                }
            }
            suggestions.append(
                {"Search " + searchEngineName() + " for " + trimmed, destination(trimmed), "search", 900}
            );
        }
        sortAndLimit(suggestions);
        replaceSuggestions(std::move(suggestions), m_query);
    }

    void OmniboxController::cancelRemoteSuggestions() {
        m_remoteTimer.stop();
        if (m_remoteReply) {
            m_remoteReply->abort();
            m_remoteReply.clear();
        }
    }

    bool OmniboxController::remoteSuggestionsAvailable() const {
        return searchSuggestionsEnabled() && remoteSuggestionProvider() != RemoteSuggestionProvider::None;
    }

    bool OmniboxController::queryEligibleForRemoteSuggestions(const QString &query) const {
        if (query.size() < 2) {
            return false;
        }
        const bool looksLikeHost = query.contains('.') && !query.contains(' ');
        const bool hasScheme = QUrl(query).scheme().size() > 1;
        return !looksLikeHost && !hasScheme;
    }

    OmniboxController::RemoteSuggestionProvider OmniboxController::remoteSuggestionProvider() const {
        const QUrl searchUrl(searchUrlTemplate());
        if (searchUrl.scheme() != "https") {
            return RemoteSuggestionProvider::None;
        }
        const QString host = searchUrl.host().toLower();
        if (host == "duckduckgo.com" || host == "www.duckduckgo.com") {
            return RemoteSuggestionProvider::DuckDuckGo;
        }
        if (host == "google.com" || host == "www.google.com") {
            return RemoteSuggestionProvider::Google;
        }
        return RemoteSuggestionProvider::None;
    }

    QUrl OmniboxController::remoteSuggestionUrl(RemoteSuggestionProvider provider, const QString &queryText) const {
        QUrl url;
        QUrlQuery query;
        if (provider == RemoteSuggestionProvider::DuckDuckGo) {
            url = QUrl("https://duckduckgo.com/ac/");
            query.addQueryItem("q", queryText);
            query.addQueryItem("type", "list");
        } else if (provider == RemoteSuggestionProvider::Google) {
            url = QUrl("https://www.google.com/complete/search");
            query.addQueryItem("client", "chrome");
            query.addQueryItem("q", queryText);
        }
        url.setQuery(query);
        return url;
    }

    void OmniboxController::requestRemoteSuggestions() {
        const QString queryText = m_query.trimmed();
        if (!remoteSuggestionsAvailable() || !queryEligibleForRemoteSuggestions(queryText)) {
            return;
        }
        const quint64 generation = m_generation;
        QNetworkRequest request(remoteSuggestionUrl(remoteSuggestionProvider(), queryText));
        request.setRawHeader("Accept", "application/json");
        request.setTransferTimeout(2000);
        QNetworkReply *reply = m_network.get(request);
        m_remoteReply = reply;
        connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
            const QByteArray payload = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
            if (m_remoteReply == reply) {
                m_remoteReply.clear();
            }
            reply->deleteLater();
            applyRemoteSuggestions(payload, generation);
        });
    }

    void OmniboxController::applyRemoteSuggestions(const QByteArray &payload, quint64 generation) {
        if (generation != m_generation || payload.isEmpty()) {
            return;
        }
        const qsizetype jsonStart = payload.indexOf('[');
        if (jsonStart < 0) {
            return;
        }
        const QJsonArray response = QJsonDocument::fromJson(payload.sliced(jsonStart)).array();
        const QJsonArray values = response.size() > 1 && response.at(1).isArray() ? response.at(1).toArray() : response;
        QList<Suggestion> remote;
        for (const QJsonValue &value : values) {
            QString text;
            if (value.isString()) {
                text = value.toString();
            } else {
                text = value.toObject().value("phrase").toString();
            }
            const bool duplicate = std::any_of(remote.cbegin(), remote.cend(), [&text](const Suggestion &suggestion) {
                return suggestion.title.compare(text, Qt::CaseInsensitive) == 0;
            });
            if (!text.isEmpty() && text.compare(m_query.trimmed(), Qt::CaseInsensitive) != 0 && !duplicate) {
                remote.append({text, destination(text), "remote", 850});
            }
            if (remote.size() == 4) {
                break;
            }
        }
        if (remote.isEmpty()) {
            return;
        }
        QList<Suggestion> suggestions = m_suggestions;
        suggestions.append(remote);
        sortAndLimit(suggestions);
        replaceSuggestions(std::move(suggestions), m_query);
    }

    void OmniboxController::replaceSuggestions(QList<Suggestion> suggestions, const QString &query) {
        beginResetModel();
        m_suggestions = std::move(suggestions);
        m_suggestionsQuery = query;
        endResetModel();
        updateCompletionSuffix();
    }

    void OmniboxController::clearCompletionSuffix() {
        if (m_completionSuffix.isEmpty()) {
            return;
        }
        m_completionSuffix.clear();
        emit completionSuffixChanged();
    }

    void OmniboxController::updateCompletionSuffix() {
        QString suffix;
        const QString typed = m_query.trimmed();
        if (!typed.isEmpty() && !typed.contains(' ') && m_suggestionsQuery == m_query && !m_suggestions.isEmpty()) {
            const Suggestion &suggestion = m_suggestions.first();
            if (suggestion.kind != "search" && suggestion.kind != "remote") {
                QString candidate = suggestion.url.toDisplayString(QUrl::RemovePassword);
                QString simplified = candidate;
                const qsizetype schemeEnd = simplified.indexOf("://");
                if (schemeEnd >= 0) {
                    simplified.remove(0, schemeEnd + 3);
                }
                const QString withoutWww =
                    simplified.startsWith("www.", Qt::CaseInsensitive) ? simplified.sliced(4) : simplified;
                for (const QString &variant : {candidate, simplified, withoutWww}) {
                    if (variant.size() > typed.size() && variant.startsWith(typed, Qt::CaseInsensitive)) {
                        suffix = variant.sliced(typed.size());
                        break;
                    }
                }
            }
        }
        if (m_completionSuffix == suffix) {
            return;
        }
        m_completionSuffix = suffix;
        emit completionSuffixChanged();
    }

}
