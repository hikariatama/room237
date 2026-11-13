#pragma once
#include "entry.h"
#include "settings.h"

#include <QJsonArray>
#include <QJsonObject>

namespace core::duplicates {

using Bits = std::vector<quint64>;
bool withinThreshold(const quint64 *a, const quint64 *b, size_t words, quint32 threshold);
std::optional<Bits> computeHashForPath(const AlbumMeta &meta, const QString &path, const HashConfig &config, const AdvancedSettings &settings);
std::optional<Bits> computeHashForFile(const QString &path, const HashConfig &config, const AdvancedSettings &settings);
std::optional<Bits> computeHashFromEntry(const QString &path, const HashConfig &config, const AdvancedSettings &settings, const std::optional<FileMetaEntry> &entry);
void markNonDuplicates(const QString &dir, const QStringList &files);

// Disk-backed similarity index (disk_duplicates.cpp).
QJsonArray findDuplicates(const QString &dir);  // Groups of the first page, for the legacy command.
QJsonObject findDuplicatePage(const QString &dir, quint64 pageIndex);
QJsonObject findDuplicateGroups(const QString &dir, quint64 offset, const std::optional<QString> &signature);
QJsonArray duplicateGroupMembers(const QString &dir, const QString &root, const QString &signature, quint64 offset);
QJsonObject markDuplicateGroupDifferent(const QString &dir, const QString &root, const QString &signature, std::optional<quint64> offset);
quint64 deleteDuplicateGroups(const QString &dir, const QString &signature);

}  // namespace core::duplicates
