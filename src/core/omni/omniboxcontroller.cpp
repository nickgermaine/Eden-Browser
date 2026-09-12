#include "core/omni/omniboxcontroller.h"
#include "core/bookmarks/bookmarkstore.h"
#include "core/history/historystore.h"
#include "core/profiles/profilesettings.h"
#include "core/window/tabmodel.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

#include <libpsl.h>

#include <algorithm>

namespace eden::core {

    OmniboxController::OmniboxController(
        TabModel *tabs,
        HistoryStore *history,
        BookmarkStore *bookmarks,
        ProfileSettings *settings,
        QObject *parent,
        QNetworkAccessManager *network
    )
        : QAbstractListModel(parent),
          m_tabs(tabs),
          m_history(history),
          m_bookmarks(bookmarks),
          m_settings(settings),
          m_editor(new OmniboxInput(this)),
          m_network(network ? network : new QNetworkAccessManager(this)) {
        m_localTimer.setSingleShot(true);
        m_localTimer.setInterval(16);
        connect(&m_localTimer, &QTimer::timeout, this, &OmniboxController::rebuildLocalSuggestions);
        m_remoteTimer.setSingleShot(true);
        m_remoteTimer.setInterval(100);
        connect(&m_remoteTimer, &QTimer::timeout, this, &OmniboxController::requestRemoteSuggestions);
        if (history) {
            connect(history, &HistoryStore::changed, this, [this] {
                if (!m_query.isEmpty()) {
                    m_localTimer.start();
                }
            });
        }
        for (QAbstractItemModel *model :
             {static_cast<QAbstractItemModel *>(tabs), static_cast<QAbstractItemModel *>(bookmarks)}) {
            if (model) {
                const auto schedule = [this] {
                    if (!m_query.isEmpty()) {
                        m_localTimer.start();
                    }
                };
                connect(model, &QAbstractItemModel::modelReset, this, schedule);
                connect(model, &QAbstractItemModel::rowsInserted, this, schedule);
                connect(model, &QAbstractItemModel::rowsRemoved, this, schedule);
                connect(model, &QAbstractItemModel::rowsMoved, this, schedule);
                connect(model, &QAbstractItemModel::dataChanged, this, schedule);
            }
        }
        if (settings) {
            const auto searchSettingsChanged = [this] {
                ++m_generation;
                m_remoteSuggestions.clear();
                cancelRemoteSuggestions();
                rebuildLocalSuggestions();
                if (remoteSuggestionsAvailable() && queryEligibleForRemoteSuggestions(m_query)) {
                    m_remoteTimer.start();
                }
            };
            connect(settings, &ProfileSettings::searchEngineChanged, this, searchSettingsChanged);
            connect(settings, &ProfileSettings::searchUrlChanged, this, searchSettingsChanged);
            connect(settings, &ProfileSettings::searchSuggestionsChanged, this, searchSettingsChanged);
        }
    }

    OmniboxController::~OmniboxController() {
        ++m_generation;
        cancelRemoteSuggestions();
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

    OmniboxInput *OmniboxController::editor() const {
        return m_editor;
    }

    void OmniboxController::setQuery(const QString &query) {
        updateQuery(query, true);
    }

    void OmniboxController::updateQuery(const QString &query, bool allowCompletion) {
        if (m_query == query && m_allowCompletion == allowCompletion) {
            return;
        }
        const bool changed = m_query != query;
        if (changed) {
            m_remoteSuggestions.clear();
        }
        m_query = query;
        m_allowCompletion = allowCompletion;
        ++m_generation;
        cancelRemoteSuggestions();
        m_localTimer.stop();
        if (changed) {
            emit queryChanged();
        }
        rebuildLocalSuggestions();
        if (remoteSuggestionsAvailable() && queryEligibleForRemoteSuggestions(m_query)) {
            m_remoteTimer.start();
        }
    }

    QUrl OmniboxController::directUrl(const QString &text) {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty()) {
            return {};
        }
        if (trimmed.startsWith("//")) {
            const QUrl url("https:" + trimmed);
            return url.isValid() && !url.host().isEmpty() ? url : QUrl();
        }
        if (trimmed.startsWith('/')) {
            return QUrl::fromLocalFile(trimmed);
        }
        QHostAddress address;
        if (address.setAddress(trimmed) && address.protocol() == QAbstractSocket::IPv6Protocol) {
            QUrl url;
            url.setScheme("http");
            url.setHost(trimmed);
            return url;
        }
        static const QRegularExpression portHost(QStringLiteral(R"(^[^\s/:?#]+:[0-9]+(?:[/?#]|$))"));
        static const QRegularExpression scheme(QStringLiteral(R"(^[a-zA-Z][a-zA-Z0-9+.-]*:)"));
        const bool hasScheme = scheme.match(trimmed).hasMatch() && !portHost.match(trimmed).hasMatch();
        if (hasScheme) {
            QUrl url(trimmed);
            if (!url.isValid()) {
                return {};
            }
            if ((url.scheme() == "http" || url.scheme() == "https" || url.scheme() == "ftp") && url.host().isEmpty()) {
                return {};
            }
            const bool hasSpace = std::any_of(trimmed.cbegin(), trimmed.cend(), [](QChar c) { return c.isSpace(); });
            if (hasSpace && !trimmed.contains("://") && url.scheme() != "file" && url.scheme() != "javascript" &&
                url.scheme() != "data") {
                return {};
            }
            return url;
        }
        if (std::any_of(trimmed.cbegin(), trimmed.cend(), [](QChar c) { return c.isSpace(); }) ||
            trimmed.contains('@')) {
            return {};
        }
        QUrl url("https://" + trimmed);
        const QString host = url.host();
        if (!url.isValid() || host.isEmpty() || host.contains("..") || host.startsWith('.')) {
            return {};
        }
        const bool local = host == "localhost" || host.endsWith(".localhost") || address.setAddress(host);
        const bool hostAndPath = trimmed.contains('/') && !trimmed.startsWith('?');
        if (!host.contains('.') && !local && !hostAndPath && url.port() < 0) {
            return {};
        }
        if (local || url.port() == 80 || !host.contains('.')) {
            url.setScheme("http");
        }
        return url;
    }

    QUrl OmniboxController::searchDestination(const QString &text) const {
        return QUrl(searchUrlTemplate().arg(QString::fromUtf8(QUrl::toPercentEncoding(text.trimmed()))));
    }

    QUrl OmniboxController::destination(const QString &text, bool controlEnter) const {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty()) {
            return {};
        }
        static const QRegularExpression bareName(QStringLiteral(R"(^[\p{L}\p{N}](?:[\p{L}\p{N}-]*[\p{L}\p{N}])?$)"));
        if (controlEnter && bareName.match(trimmed).hasMatch()) {
            return QUrl("https://www." + trimmed + ".com");
        }
        const QUrl url = directUrl(trimmed);
        return url.isEmpty() ? searchDestination(trimmed) : url;
    }

    QString OmniboxController::suggestionText(int row) const {
        if (m_suggestionsQuery != m_query || row < 0 || row >= m_suggestions.size()) {
            return m_query;
        }
        const Suggestion &suggestion = m_suggestions.at(row);
        if (suggestion.kind == "search") {
            return m_query;
        }
        if (suggestion.kind == "remote") {
            return suggestion.title;
        }
        return suggestion.url.toDisplayString(QUrl::RemoveUserInfo);
    }

    int OmniboxController::suggestionTabIndex(int row) const {
        if (!m_tabs || m_suggestionsQuery != m_query || row < 0 || row >= m_suggestions.size()) {
            return -1;
        }
        return m_tabs->indexForTabId(m_suggestions.at(row).tabId);
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

    void OmniboxController::sortSuggestions(QList<Suggestion> &suggestions) {
        std::stable_sort(suggestions.begin(), suggestions.end(), [](const Suggestion &left, const Suggestion &right) {
            return left.score > right.score;
        });
    }

    QString OmniboxController::completionFor(const QString &query, const QUrl &url) {
        if (query.isEmpty() || query != query.trimmed() || query.contains('?') || query.contains('#') ||
            query.contains('@') || std::any_of(query.cbegin(), query.cend(), [](QChar c) { return c.isSpace(); }) ||
            (url.scheme() != "https" && url.scheme() != "http") || !url.userInfo().isEmpty()) {
            return {};
        }
        const QUrl typedUrl = directUrl(query);
        if (!typedUrl.isEmpty() && typedUrl.path().isEmpty()) {
            const QString host = typedUrl.host();
            const QByteArray suffix = QUrl::toAce(host.section('.', -1));
            if (host == url.host() || host == "localhost" || typedUrl.port() >= 0 || !QHostAddress(host).isNull() ||
                psl_is_public_suffix2(psl_builtin(), suffix.constData(), PSL_TYPE_ICANN | PSL_TYPE_NO_STAR_RULE) ||
                suffix == "test" || suffix == "example" || suffix == "invalid" || suffix == "local" ||
                suffix == "onion") {
                return {};
            }
        }
        const QString candidate = url.toDisplayString(QUrl::RemoveUserInfo);
        const QString simplified = candidate.sliced(candidate.indexOf("://") + 3);
        const QString withoutWww =
            simplified.startsWith("www.", Qt::CaseInsensitive) ? simplified.sliced(4) : simplified;
        for (const QString &variant : {candidate, simplified, withoutWww}) {
            if (variant.size() <= query.size() || !variant.startsWith(query, Qt::CaseInsensitive)) {
                continue;
            }
            const qsizetype authorityStart = variant.indexOf("://") >= 0 ? variant.indexOf("://") + 3 : 0;
            const qsizetype pathStart = variant.indexOf('/', authorityStart);
            if (pathStart >= 0 && query.size() > pathStart &&
                variant.sliced(pathStart, query.size() - pathStart) != query.sliced(pathStart)) {
                continue;
            }
            const qsizetype queryStart = variant.indexOf('?');
            const qsizetype fragmentStart = variant.indexOf('#');
            qsizetype end = variant.size();
            if (queryStart >= 0) {
                end = queryStart;
            }
            if (fragmentStart >= 0) {
                end = std::min(end, fragmentStart);
            }
            if (end > query.size()) {
                return query + variant.sliced(query.size(), end - query.size());
            }
        }
        return {};
    }

    void OmniboxController::rebuildLocalSuggestions() {
        QList<Suggestion> suggestions;
        const QString trimmed = m_query.trimmed();
        if (trimmed.isEmpty()) {
            replaceSuggestions({}, m_query);
            return;
        }
        const auto add = [&](const QString &title, const QUrl &url, const QString &kind, int bonus, quint64 tabId = 0) {
            if (!url.isValid() || url.isEmpty() || !url.userInfo().isEmpty()) {
                return;
            }
            const QString completion = m_allowCompletion ? completionFor(m_query, url) : QString();
            const int score = std::max(fuzzyScore(trimmed, title), fuzzyScore(trimmed, url.toDisplayString()));
            if (score >= 0 || !completion.isEmpty()) {
                suggestions.append(
                    {title.isEmpty() ? url.toDisplayString() : title,
                     url,
                     kind,
                     bonus + std::max(0, score) + (completion.isEmpty() ? 0 : 2000),
                     tabId,
                     completion}
                );
            }
        };
        if (m_tabs) {
            for (int row = 0; row < m_tabs->rowCount(); ++row) {
                const auto index = m_tabs->index(row);
                add(m_tabs->data(index, TabModel::TitleRole).toString(),
                    m_tabs->data(index, TabModel::UrlRole).toUrl(),
                    "tab",
                    300,
                    m_tabs->tabIdAt(row));
            }
        }
        if (m_history) {
            for (int row = 0; row < m_history->rowCount(); ++row) {
                const auto index = m_history->index(row);
                const qint64 visits = m_history->data(index, HistoryStore::VisitCountRole).toLongLong();
                add(m_history->data(index, HistoryStore::TitleRole).toString(),
                    m_history->data(index, HistoryStore::UrlRole).toUrl(),
                    "history",
                    200 + static_cast<int>(std::min<qint64>(visits, 50)));
            }
        }
        if (m_bookmarks) {
            for (int row = 0; row < m_bookmarks->rowCount(); ++row) {
                const auto index = m_bookmarks->index(row);
                add(m_bookmarks->data(index, BookmarkStore::TitleRole).toString(),
                    m_bookmarks->data(index, BookmarkStore::UrlRole).toUrl(),
                    "bookmark",
                    100);
            }
        }
        sortSuggestions(suggestions);
        const QUrl direct = directUrl(trimmed);
        Suggestion primary{
            direct.isEmpty() ? "Search " + searchEngineName() + " for " + trimmed : trimmed,
            destination(trimmed),
            direct.isEmpty() ? "search" : "url",
            10000
        };
        const Suggestion literal = primary;
        if (!suggestions.isEmpty() && !suggestions.first().completion.isEmpty()) {
            primary = suggestions.first();
            primary.url = QUrl(primary.url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment));
            primary.score = 10000;
            primary.tabId = 0;
            primary.kind = "url";
        }
        QList<Suggestion> unique{primary};
        const auto keyFor = [](QUrl url) {
            if (url.path() == "/") {
                url.setPath({});
            }
            if ((url.scheme() == "https" && url.port() == 443) || (url.scheme() == "http" && url.port() == 80)) {
                url.setPort(-1);
            }
            return url;
        };
        QSet<QUrl> urls{keyFor(primary.url)};
        if (!urls.contains(keyFor(literal.url))) {
            urls.insert(keyFor(literal.url));
            unique.append(literal);
        }
        const int limit = remoteSuggestionsAvailable() && queryEligibleForRemoteSuggestions(m_query) ? 8 : 12;
        for (const Suggestion &suggestion : std::as_const(suggestions)) {
            const QUrl key = keyFor(suggestion.url);
            if (!urls.contains(key)) {
                urls.insert(key);
                unique.append(suggestion);
            }
            if (unique.size() == limit) {
                break;
            }
        }
        for (const Suggestion &suggestion : std::as_const(m_remoteSuggestions)) {
            if (unique.size() == 12) {
                break;
            }
            unique.append(suggestion);
        }
        replaceSuggestions(std::move(unique), m_query);
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
        const QString trimmed = query.trimmed();
        return trimmed.size() >= 2 && trimmed.size() <= 2048 && directUrl(trimmed).isEmpty() &&
               !trimmed.contains('/') && !trimmed.contains('\\') && !trimmed.contains('@') && !trimmed.contains(':') &&
               !trimmed.contains('?') && !trimmed.contains('#');
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
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        QNetworkReply *reply = m_network->get(request);
        m_remoteReply = reply;
        reply->setReadBufferSize(65537);
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() > 65536) {
                reply->abort();
            }
        });
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
        if (generation != m_generation || payload.isEmpty() || payload.size() > 65536 ||
            !remoteSuggestionsAvailable() || !queryEligibleForRemoteSuggestions(m_query)) {
            return;
        }
        const qsizetype jsonStart = payload.indexOf('[');
        if (jsonStart < 0) {
            return;
        }
        const QJsonArray response = QJsonDocument::fromJson(payload.sliced(jsonStart)).array();
        if (!response.isEmpty() && response.first().isString() &&
            (response.size() < 2 || !response.at(1).isArray() || response.first().toString() != m_query.trimmed())) {
            return;
        }
        const QJsonArray values = response.size() > 1 && response.at(1).isArray() ? response.at(1).toArray() : response;
        QList<Suggestion> remote;
        for (const QJsonValue &value : values) {
            QString text;
            if (value.isString()) {
                text = value.toString();
            } else {
                text = value.toObject().value("phrase").toString();
            }
            text = text.trimmed();
            const bool duplicate = std::any_of(remote.cbegin(), remote.cend(), [&text](const Suggestion &suggestion) {
                return suggestion.title.compare(text, Qt::CaseInsensitive) == 0;
            });
            if (!text.isEmpty() && text.size() <= 2048 &&
                std::none_of(
                    text.cbegin(),
                    text.cend(),
                    [](QChar c) { return c.category() == QChar::Other_Control; }
                ) &&
                text.compare(m_query.trimmed(), Qt::CaseInsensitive) != 0 && !duplicate) {
                remote.append({text, searchDestination(text), "remote", 850});
            }
            if (remote.size() == 4) {
                break;
            }
        }
        m_remoteSuggestions = remote;
        QList<Suggestion> suggestions = m_suggestions;
        suggestions.erase(
            std::remove_if(
                suggestions.begin(),
                suggestions.end(),
                [](const Suggestion &suggestion) { return suggestion.kind == "remote"; }
            ),
            suggestions.end()
        );
        for (const Suggestion &suggestion : std::as_const(remote)) {
            if (suggestions.size() == 12) {
                break;
            }
            suggestions.append(suggestion);
        }
        replaceSuggestions(std::move(suggestions), m_query);
    }

    void OmniboxController::replaceSuggestions(QList<Suggestion> suggestions, const QString &query) {
        beginResetModel();
        m_suggestions = std::move(suggestions);
        m_suggestionsQuery = query;
        updateCompletionSuffix();
        endResetModel();
    }

    void OmniboxController::updateCompletionSuffix() {
        QString suffix;
        if (m_allowCompletion && m_suggestionsQuery == m_query && !m_suggestions.isEmpty()) {
            const QString completion = m_suggestions.first().completion;
            if (completion.startsWith(m_query)) {
                suffix = completion.sliced(m_query.size());
            }
        }
        if (m_completionSuffix == suffix) {
            return;
        }
        m_completionSuffix = suffix;
        emit completionSuffixChanged();
    }

}
