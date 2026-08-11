#include "core/downloads/downloadmanager.h"

namespace eden::core {

DownloadManager::DownloadManager(QObject *parent)
    : QAbstractListModel(parent) {}

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
    return {{FileNameRole, "fileName"},           {SourceUrlRole, "sourceUrl"},   {TargetPathRole, "targetPath"},
            {ReceivedBytesRole, "receivedBytes"}, {TotalBytesRole, "totalBytes"}, {StateRole, "downloadState"}};
}

void DownloadManager::beginDownload(int id, const QString &fileName, const QUrl &sourceUrl, const QString &targetPath, qint64 totalBytes) {
    const int row = m_downloads.size();
    beginInsertRows({}, row, row);
    m_downloads.append({id, fileName, sourceUrl, targetPath, 0, totalBytes, "downloading"});
    endInsertRows();
}

void DownloadManager::updateDownload(int id, qint64 receivedBytes, qint64 totalBytes, const QString &state) {
    for (int row = 0; row < m_downloads.size(); ++row) {
        Download &download = m_downloads[row];
        if (download.id != id) {
            continue;
        }
        download.receivedBytes = receivedBytes;
        download.totalBytes = totalBytes;
        download.state = state;
        emit dataChanged(index(row), index(row), {ReceivedBytesRole, TotalBytesRole, StateRole});
        return;
    }
}

void DownloadManager::clearFinished() {
    for (int row = m_downloads.size() - 1; row >= 0; --row) {
        if (m_downloads.at(row).state == "downloading") {
            continue;
        }
        beginRemoveRows({}, row, row);
        m_downloads.removeAt(row);
        endRemoveRows();
    }
}

}
