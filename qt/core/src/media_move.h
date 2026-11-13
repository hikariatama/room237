#pragma once
// Crash-safe media moves between albums: an append-only journal in the source album and a locator in the target.
#include "metadata.h"

#include <QFile>

namespace core::media_move {

struct Header {
  int version = 1;
  QString source, target, sourceId, targetId;
  bool operator==(const Header &) const = default;
  void validate(const QString &album) const;
};

struct FileRecord {
  QString source, target, revision;
  bool operator==(const FileRecord &) const = default;
};

class MoveJournal {
public:
  static std::unique_ptr<MoveJournal> create(const QString &source, const QString &target);
  void recordFiles(const std::vector<FileRecord> &files) { recordFilesWithArtifacts(files, false); }
  void recordFilesWithArtifacts(const std::vector<FileRecord> &files, bool moveArtifacts);
  void recordMetadata(const metadata::MoveMetadataUndo &undo);
  void commit(const std::function<void()> &checkpoint);
  void finish(const std::function<void(const QString &)> &checkpoint) const;
  void rollback(const metadata::MetadataWriteGuard *guard);
  const QString &staging() const { return staging_; }
private:
  void append(const QString &line, bool terminal);
  void sync();
  Header header_;
  QString staging_, peer_;
  std::unique_ptr<QFile> events_;
  bool failed_ = false;
};

void recoverAlbum(const QString &album);
void recoverAlbumWithHook(const QString &album, const std::function<void(const QString &)> &checkpoint);
bool hasJournal(const QString &album);

}  // namespace core::media_move
