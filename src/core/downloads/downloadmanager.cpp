#include "core/downloads/downloadmanager.h"

#include "core/profiles/sqlcipherdatabase.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

#include <sqlcipher/sqlite3.h>

namespace eden::core {

    DownloadManager::DownloadManager(const QString &databasePath, QObject *parent)
        : QAbstractListModel(parent),
          m_databasePath(databasePath) {}

    DownloadManager::~DownloadManager() {
        if (m_database) {
            sqlite3_close_v2(m_database);
        }
    }

    bool DownloadManager::initialize(const QByteArray &key) {
        if (m_databasePath.isEmpty()) {
            m_initialized = true;
            return true;
        }
        if (m_database) {
            return m_initialized;
        }
        m_database = SqlCipherDatabase::open(m_databasePath, key, false);
        if (!m_database ||
            !SqlCipherDatabase::execute(
                m_database,
                "CREATE TABLE IF NOT EXISTS downloads("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,file_name TEXT NOT NULL,source_url TEXT NOT NULL,"
                "target_path TEXT NOT NULL,received_bytes INTEGER NOT NULL,total_bytes INTEGER NOT NULL,"
                "state TEXT NOT NULL,started_at INTEGER NOT NULL,updated_at INTEGER NOT NULL)"
            ) ||
            !SqlCipherDatabase::execute(
                m_database,
                "UPDATE downloads SET state='interrupted',updated_at=unixepoch('subsec')*1000 "
                "WHERE state='downloading'"
            )) {
            if (m_database) {
                sqlite3_close_v2(m_database);
                m_database = nullptr;
            }
            return false;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "SELECT id,file_name,source_url,target_path,received_bytes,total_bytes,state,started_at,updated_at "
                "FROM downloads ORDER BY started_at DESC,id DESC",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return false;
        }
        beginResetModel();
        m_downloads.clear();
        while (sqlite3_step(statement) == SQLITE_ROW) {
            Download download;
            download.persistentId = sqlite3_column_int64(statement, 0);
            download.fileName = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 1)));
            download.sourceUrl =
                QUrl(QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 2))));
            download.targetPath = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 3)));
            download.receivedBytes = sqlite3_column_int64(statement, 4);
            download.totalBytes = sqlite3_column_int64(statement, 5);
            download.state = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 6)));
            download.startedAt = sqlite3_column_int64(statement, 7);
            download.updatedAt = sqlite3_column_int64(statement, 8);
            download.persistedAt = download.updatedAt;
            m_downloads.append(std::move(download));
        }
        sqlite3_finalize(statement);
        endResetModel();
        SqlCipherDatabase::restrictFiles(m_databasePath);
        m_initialized = true;
        return true;
    }

    bool DownloadManager::isInitialized() const {
        return m_initialized;
    }

    int DownloadManager::rowCount(const QModelIndex &parent) const {
        return parent.isValid() ? 0 : m_downloads.size();
    }

    QVariant DownloadManager::data(const QModelIndex &index, int role) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_downloads.size()) {
            return {};
        }
        const Download &download = m_downloads.at(index.row());
        if (role == FileNameRole) {
            return download.fileName;
        }
        if (role == SourceUrlRole) {
            return download.sourceUrl;
        }
        if (role == TargetPathRole) {
            return download.targetPath;
        }
        if (role == ReceivedBytesRole) {
            return download.receivedBytes;
        }
        if (role == TotalBytesRole) {
            return download.totalBytes;
        }
        if (role == StateRole) {
            return download.state;
        }
        return {};
    }

    QHash<int, QByteArray> DownloadManager::roleNames() const {
        return {
            {FileNameRole, "fileName"},
            {SourceUrlRole, "sourceUrl"},
            {TargetPathRole, "targetPath"},
            {ReceivedBytesRole, "receivedBytes"},
            {TotalBytesRole, "totalBytes"},
            {StateRole, "downloadState"}
        };
    }

    void DownloadManager::beginDownload(
        quint64 id,
        const QString &fileName,
        const QUrl &sourceUrl,
        const QString &targetPath,
        qint64 totalBytes
    ) {
        for (Download &download : m_downloads) {
            if (download.runtimeId == id && download.state == "downloading") {
                return;
            }
        }
        Download download;
        download.runtimeId = id;
        download.fileName = fileName;
        download.sourceUrl = sourceUrl;
        download.targetPath = targetPath;
        download.totalBytes = totalBytes;
        download.state = "downloading";
        download.startedAt = QDateTime::currentMSecsSinceEpoch();
        download.updatedAt = download.startedAt;
        download.persistedAt = download.startedAt;
        if (m_database) {
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(
                    m_database,
                    "INSERT INTO downloads(file_name,source_url,target_path,received_bytes,total_bytes,state,"
                    "started_at,updated_at) VALUES(?,?,?,?,?,?,?,?)",
                    -1,
                    &statement,
                    nullptr
                ) == SQLITE_OK) {
                const QByteArray fileNameUtf8 = fileName.toUtf8();
                const QByteArray sourceUrlUtf8 = sourceUrl.toString().toUtf8();
                const QByteArray targetPathUtf8 = targetPath.toUtf8();
                const QByteArray stateUtf8 = download.state.toUtf8();
                sqlite3_bind_text(statement, 1, fileNameUtf8.constData(), fileNameUtf8.size(), SQLITE_TRANSIENT);
                sqlite3_bind_text(statement, 2, sourceUrlUtf8.constData(), sourceUrlUtf8.size(), SQLITE_TRANSIENT);
                sqlite3_bind_text(statement, 3, targetPathUtf8.constData(), targetPathUtf8.size(), SQLITE_TRANSIENT);
                sqlite3_bind_int64(statement, 4, download.receivedBytes);
                sqlite3_bind_int64(statement, 5, totalBytes);
                sqlite3_bind_text(statement, 6, stateUtf8.constData(), stateUtf8.size(), SQLITE_TRANSIENT);
                sqlite3_bind_int64(statement, 7, download.startedAt);
                sqlite3_bind_int64(statement, 8, download.updatedAt);
                if (sqlite3_step(statement) == SQLITE_DONE) {
                    download.persistentId = sqlite3_last_insert_rowid(m_database);
                }
            }
            sqlite3_finalize(statement);
            SqlCipherDatabase::restrictFiles(m_databasePath);
        }
        beginInsertRows({}, 0, 0);
        m_downloads.prepend(std::move(download));
        endInsertRows();
    }

    void DownloadManager::updateDownload(quint64 id, qint64 receivedBytes, qint64 totalBytes, const QString &state) {
        for (int row = 0; row < m_downloads.size(); ++row) {
            Download &download = m_downloads[row];
            if (download.runtimeId != id || download.state != "downloading") {
                continue;
            }
            download.receivedBytes = receivedBytes;
            download.totalBytes = totalBytes;
            download.state = state;
            download.updatedAt = QDateTime::currentMSecsSinceEpoch();
            if (state != "downloading" || download.updatedAt - download.persistedAt >= 500) {
                persistUpdate(download);
            }
            emit dataChanged(index(row), index(row), {ReceivedBytesRole, TotalBytesRole, StateRole});
            return;
        }
    }

    int DownloadManager::activeCount() const {
        return static_cast<int>(std::count_if(m_downloads.cbegin(), m_downloads.cend(), [](const Download &download) {
            return download.state == "downloading";
        }));
    }

    void DownloadManager::clearFinished() {
        if (m_database) {
            SqlCipherDatabase::execute(m_database, "DELETE FROM downloads WHERE state!='downloading'");
            SqlCipherDatabase::restrictFiles(m_databasePath);
        }
        for (int row = m_downloads.size() - 1; row >= 0; --row) {
            if (m_downloads.at(row).state == "downloading") {
                continue;
            }
            beginRemoveRows({}, row, row);
            m_downloads.removeAt(row);
            endRemoveRows();
        }
    }

    void DownloadManager::openDownload(const QString &targetPath) const {
        if (!targetPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
        }
    }

    void DownloadManager::showInFolder(const QString &targetPath) const {
        if (!targetPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(targetPath).absoluteDir().absolutePath()));
        }
    }

    void DownloadManager::persistUpdate(Download &download) {
        if (!m_database || download.persistentId <= 0) {
            return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(
                m_database,
                "UPDATE downloads SET received_bytes=?,total_bytes=?,state=?,updated_at=? WHERE id=?",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            return;
        }
        const QByteArray stateUtf8 = download.state.toUtf8();
        sqlite3_bind_int64(statement, 1, download.receivedBytes);
        sqlite3_bind_int64(statement, 2, download.totalBytes);
        sqlite3_bind_text(statement, 3, stateUtf8.constData(), stateUtf8.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 4, download.updatedAt);
        sqlite3_bind_int64(statement, 5, download.persistentId);
        if (sqlite3_step(statement) == SQLITE_DONE) {
            download.persistedAt = download.updatedAt;
        }
        sqlite3_finalize(statement);
        SqlCipherDatabase::restrictFiles(m_databasePath);
    }

}
