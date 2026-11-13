#pragma once
#include <QString>

namespace core::maintenance {

quint64 rebuildThumbnails(const QString &root);
quint64 rebuildMetadata(const QString &root);
quint64 resetDuplicates(const QString &root);
quint64 clearRoom237Artifacts(const QString &root);

struct MigrationOutcome {
  enum Kind { AlreadyDone, NoLegacyProfile, Imported } kind;
  bool settings = false, state = false;
};
MigrationOutcome migrateProfile(const QString &profile, const QString &legacy);

}  // namespace core::maintenance
