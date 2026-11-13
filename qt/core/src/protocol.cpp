#include "album.h"
#include "catalog.h"
#include "clipboard.h"
#include "core.h"
#include "duplicates.h"
#include "host.h"
#include "json.h"
#include "maintenance.h"
#include "media_delete.h"
#include "metadata.h"
#include "preload.h"
#include "settings.h"
#include "update.h"
#include "vault.h"
#include "vault_ops.h"
#include "util.h"

#include <QJsonArray>
#include <mutex>
#include <thread>

namespace core {

namespace {
// --- Startup: FFmpeg ships next to the application; nothing is downloaded at run time. ---------------------------
std::mutex startupMutex;
QJsonObject startupStatus{{QStringLiteral("phase"), QStringLiteral("checking")}};
bool startupRunning = false;

void publishStartup(const QJsonObject &status) {
  {
    std::lock_guard lock(startupMutex);
    startupStatus = status;
  }
  host::emitEvent(QStringLiteral("startup-status"), status);
}

void beginStartup() {
  {
    std::lock_guard lock(startupMutex);
    if (startupRunning || startupStatus.value(QLatin1String("phase")).toString() == QLatin1String("ready")) return;
    startupRunning = true;
    startupStatus = {{QStringLiteral("phase"), QStringLiteral("checking")}};
  }
  std::thread([] {
    publishStartup({{QStringLiteral("phase"), QStringLiteral("checking")}});
    QJsonObject status{{QStringLiteral("phase"), QStringLiteral("ready")}};
    const QString ffmpeg = ffmpegPath();
    try {
      const ProcessResult result = runProcess(ffmpeg, {QStringLiteral("-version")}, 20000);
      if (!result.success) status = {{QStringLiteral("phase"), QStringLiteral("error")}, {QStringLiteral("message"), QStringLiteral("%1: exit Some(%2)").arg(ffmpeg).arg(result.exitCode)}};
    } catch (const Error &error) {
      status = {{QStringLiteral("phase"), QStringLiteral("error")}, {QStringLiteral("message"), QStringLiteral("%1: %2").arg(ffmpeg, error.message())}};
    }
    {
      std::lock_guard lock(startupMutex);
      startupRunning = false;
    }
    publishStartup(status);
  }).detach();
}

// --- Arguments ---------------------------------------------------------------------------------------------------

QJsonValue at(const QJsonObject &params, const char *name) { return params.value(QLatin1String(name)); }
QString str(const QJsonObject &params, const char *name) {
  try {
    return json::stringField(at(params, name), name);
  } catch (const Error &error) {
    fail(QStringLiteral("%1: %2").arg(QLatin1String(name), error.message()));
  }
}
std::optional<QString> optStr(const QJsonObject &params, const char *name) {
  const QJsonValue value = at(params, name);
  if (value.isNull() || value.isUndefined()) return std::nullopt;
  return str(params, name);
}
quint64 uint(const QJsonObject &params, const char *name) {
  try {
    return json::unsignedField(at(params, name), name, ~0ull);
  } catch (const Error &error) {
    fail(QStringLiteral("%1: %2").arg(QLatin1String(name), error.message()));
  }
}
std::optional<quint64> optUint(const QJsonObject &params, const char *name) {
  const QJsonValue value = at(params, name);
  if (value.isNull() || value.isUndefined()) return std::nullopt;
  return uint(params, name);
}
qint64 sint(const QJsonObject &params, const char *name) {
  try {
    return json::signedField(at(params, name), name);
  } catch (const Error &error) {
    fail(QStringLiteral("%1: %2").arg(QLatin1String(name), error.message()));
  }
}
bool flag(const QJsonObject &params, const char *name) {
  try {
    return json::boolField(at(params, name), name);
  } catch (const Error &error) {
    fail(QStringLiteral("%1: %2").arg(QLatin1String(name), error.message()));
  }
}
double real(const QJsonObject &params, const char *name) {
  try {
    return json::floatField(at(params, name), name);
  } catch (const Error &error) {
    fail(QStringLiteral("%1: %2").arg(QLatin1String(name), error.message()));
  }
}
QStringList strings(const QJsonObject &params, const char *name) {
  const QJsonValue value = at(params, name);
  if (!value.isArray()) fail(QStringLiteral("%1: invalid type, expected a sequence").arg(QLatin1String(name)));
  QStringList out;
  for (const QJsonValue &item : value.toArray()) {
    if (!item.isString()) fail(QStringLiteral("%1: invalid type, expected a string").arg(QLatin1String(name)));
    out << item.toString();
  }
  return out;
}
QJsonObject object(const QJsonObject &params, const char *name) {
  const QJsonValue value = at(params, name);
  if (!value.isObject()) fail(QStringLiteral("%1: invalid type, expected a map").arg(QLatin1String(name)));
  return value.toObject();
}
catalog::PageQuery query(const QJsonObject &params) {
  try {
    return catalog::PageQuery::fromJson(at(params, "query"));
  } catch (const Error &error) {
    fail(QStringLiteral("query: %1").arg(error.message()));
  }
}
std::vector<metadata::ObservedMedia> observed(const QJsonObject &params) {
  try {
    return metadata::ObservedMedia::fromJson(at(params, "observed"));
  } catch (const Error &error) {
    fail(QStringLiteral("observed: %1").arg(error.message()));
  }
}
QJsonArray stringArray(const QStringList &items) { return QJsonArray::fromStringList(items); }
QJsonValue optional(const std::optional<catalog::PageItem> &item) { return item ? QJsonValue(item->toJson()) : QJsonValue(); }

// Every object with a packed "meta" also carries its decoded fields.
void enrich(QJsonValue &value) {
  if (value.isArray()) {
    QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
      QJsonValue item = array.at(i);
      enrich(item);
      array[i] = item;
    }
    value = array;
  } else if (value.isObject()) {
    QJsonObject o = value.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
      QJsonValue child = it.value();
      if (child.isArray() || child.isObject()) {
        enrich(child);
        it.value() = child;
      }
    }
    const QJsonValue meta = o.value(QLatin1String("meta"));
    if (meta.isString() && validPackedMeta(meta.toString())) {
      const DetachedFileMeta m = unpackFileMeta(meta.toString());
      o.insert(QStringLiteral("width"), m.w ? QJsonValue(qint64(*m.w)) : QJsonValue());
      o.insert(QStringLiteral("height"), m.h ? QJsonValue(qint64(*m.h)) : QJsonValue());
      o.insert(QStringLiteral("dateAdded"), m.a ? QJsonValue(qint64(*m.a)) : QJsonValue());
      o.insert(QStringLiteral("dateTaken"), m.s ? QJsonValue(*m.s) : QJsonValue());
      o.insert(QStringLiteral("isVideo"), m.v);
    }
    value = o;
  }
}

// Media added to an unlocked encrypted root are encrypted by a background pass.
void encryptArrivals(const QString &album) {
  const auto root = vault::vaultRoot(album);
  if (root && vault::keysFor(*root)) vault_ops::encrypt(*root);
}

QJsonValue run(const QString &method, const QJsonObject &p) {
  const QByteArray m = method.toUtf8();
  if (m == "get_startup_status") {
    std::lock_guard lock(startupMutex);
    return startupStatus;
  }
  if (m == "retry_startup") {
    beginStartup();
    return QJsonValue();
  }
  if (m == "get_file_metadata") return metadata::getFileMetadata(str(p, "path"));
  if (m == "get_album_media") return album::getAlbumMedia(str(p, "dir"));
  if (m == "get_media_page") return catalog::getMediaPage(query(p)).toJson();
  if (m == "get_inventory_print") return catalog::getInventoryPrint(query(p));
  if (m == "create_media_selection") return qint64(catalog::createMediaSelection(query(p), str(p, "token")));
  if (m == "consume_media_selection") return catalog::consumeMediaSelection(str(p, "rootDir"), str(p, "token"), strings(p, "paths"));
  if (m == "prune_media_selection") return catalog::pruneMediaSelection(str(p, "rootDir"), str(p, "token"));
  if (m == "drop_media_selection") {
    catalog::dropMediaSelection(str(p, "rootDir"), str(p, "token"));
    return QJsonValue();
  }
  if (m == "media_selection_flags") {
    QJsonArray flags;
    for (bool f : catalog::mediaSelectionFlags(str(p, "rootDir"), str(p, "token"), strings(p, "paths"))) flags.append(f);
    return flags;
  }
  if (m == "media_selection_batch") {
    QJsonArray items;
    for (const catalog::PageItem &item : catalog::mediaSelectionBatch(str(p, "rootDir"), str(p, "token"), uint(p, "offset"))) items.append(item.toJson());
    return items;
  }
  if (m == "copy_media_selection") {
    catalog::copyMediaSelection(str(p, "rootDir"), str(p, "token"), strings(p, "excluded"), strings(p, "extra"));
    return QJsonValue();
  }
  if (m == "get_viewer_media") return optional(catalog::getViewerMedia(query(p), uint(p, "index")));
  if (m == "get_media_neighbor") return optional(catalog::getMediaNeighbor(query(p), str(p, "currentPath"), int(sint(p, "delta"))));
  if (m == "begin_gallery_scroll") {
    catalog::beginGalleryScroll(str(p, "rootDir"), str(p, "session"));
    return QJsonValue();
  }
  if (m == "save_gallery_scroll") {
    const QJsonValue marks = at(p, "marks");
    if (!marks.isArray()) fail(QStringLiteral("marks: invalid type, expected a sequence"));
    catalog::saveGalleryScroll(str(p, "rootDir"), str(p, "session"), marks.toArray());
    return QJsonValue();
  }
  if (m == "read_gallery_scroll") return catalog::readGalleryScroll(str(p, "rootDir"), str(p, "session"), real(p, "y"));
  if (m == "refresh_catalog_paths") {
    catalog::refreshCatalogPaths(str(p, "rootDir"), strings(p, "paths"));
    return QJsonValue();
  }
  if (m == "rescan_inventory") {
    catalog::rescanInventory(str(p, "rootDir"), optStr(p, "albumId"));
    return QJsonValue();
  }
  if (m == "reconcile_album") return catalog::reconcileAlbum(str(p, "rootDir"), str(p, "albumId"));
  if (m == "update_check") return update::check(str(p, "manifest"), str(p, "signature"), str(p, "currentVersion"), str(p, "platform"));
  if (m == "update_stage") return update::stage(object(p, "plan"));
  if (m == "update_launch_helper") {
    update::launchHelper(object(p, "plan"));
    return QJsonValue();
  }
  if (m == "update_cleanup") {
    update::cleanup(str(p, "install"));
    return QJsonValue();
  }
  if (m == "favorite_counts") return catalog::favoriteCounts(str(p, "rootDir"));
  if (m == "get_album_size") return qint64(album::getAlbumSize(str(p, "dir")));
  if (m == "get_albums_detached") return album::getAlbumsDetached(str(p, "rootDir"));
  if (m == "move_media") return album::moveMedia(str(p, "source"), str(p, "target"), str(p, "media"));
  if (m == "move_media_batch") return stringArray(album::moveMediaBatch(str(p, "source"), str(p, "target"), strings(p, "media")));
  if (m == "is_preloading") return preload::isPreloading();
  if (m == "lock_until_preloaded") return preload::lockUntilPreloaded();
  if (m == "set_allow_open") {
    preload::setAllowOpen(flag(p, "allow"));
    return QJsonValue();
  }
  if (m == "rebuild_thumbnails") return qint64(maintenance::rebuildThumbnails(str(p, "rootDir")));
  if (m == "rebuild_metadata") return qint64(maintenance::rebuildMetadata(str(p, "rootDir")));
  if (m == "register_new_media") {
    const QJsonObject result = album::registerNewMedia(str(p, "albumPath"), str(p, "mediaName")).toJson();
    encryptArrivals(str(p, "albumPath"));
    return result;
  }
  if (m == "find_duplicates") return duplicates::findDuplicates(str(p, "dir"));
  if (m == "find_duplicate_page") return duplicates::findDuplicatePage(str(p, "dir"), uint(p, "pageIndex"));
  if (m == "find_duplicate_groups") return duplicates::findDuplicateGroups(str(p, "dir"), uint(p, "offset"), optStr(p, "signature"));
  if (m == "duplicate_group_members") return duplicates::duplicateGroupMembers(str(p, "dir"), str(p, "root"), str(p, "signature"), uint(p, "offset"));
  if (m == "mark_duplicate_group_different") return duplicates::markDuplicateGroupDifferent(str(p, "dir"), str(p, "root"), str(p, "signature"), optUint(p, "offset"));
  if (m == "delete_duplicate_groups") return qint64(duplicates::deleteDuplicateGroups(str(p, "dir"), str(p, "signature")));
  if (m == "mark_non_duplicates") {
    duplicates::markNonDuplicates(str(p, "dir"), strings(p, "files"));
    return QJsonValue();
  }
  if (m == "reset_duplicates") return qint64(maintenance::resetDuplicates(str(p, "rootDir")));
  if (m == "add_media_files") {
    const QJsonObject result = album::addMediaFiles(str(p, "dir"), album::IncomingFile::fromJson(at(p, "files"))).toJson();
    encryptArrivals(str(p, "dir"));
    return result;
  }
  if (m == "clear_room237_artifacts") return qint64(maintenance::clearRoom237Artifacts(str(p, "rootDir")));
  if (m == "set_media_favorite") return metadata::setMediaFavorite(str(p, "path"), flag(p, "favorite")).toJson();
  if (m == "delete_media_if_unchanged") {
    metadata::deleteMediaIfUnchanged(str(p, "path"), str(p, "revision"), observed(p));
    return QJsonValue();
  }
  if (m == "delete_media") {
    media_delete::deleteMedia(str(p, "path"));
    return QJsonValue();
  }
  if (m == "delete_media_batch") return media_delete::deleteMediaBatch(str(p, "albumPath"), media_delete::DeleteTarget::fromJson(at(p, "targets")), observed(p)).toJson();
  if (m == "validate_media_revisions") {
    metadata::validateMediaRevisions(observed(p));
    return QJsonValue();
  }
  if (m == "list_favorites") return album::listFavorites(str(p, "rootDir"));
  if (m == "delete_album") {
    album::deleteAlbum(str(p, "rootDir"), str(p, "albumId"));
    return QJsonValue();
  }
  if (m == "rename_album") return album::renameAlbum(str(p, "rootDir"), str(p, "albumId"), str(p, "newName")).toJson();
  if (m == "move_album") return album::moveAlbum(str(p, "rootDir"), str(p, "albumId"), optStr(p, "newParentId")).toJson();
  if (m == "set_media_timestamp") {
    QJsonArray out;
    for (const DetachedMediaEntry &entry : metadata::setMediaTimestamp(str(p, "albumPath"), strings(p, "names"), sint(p, "timestamp"))) out.append(entry.toJson());
    return out;
  }
  if (m == "get_file_manager_name") return fileManagerName();
  if (m == "canonicalize_directory") return canonicalizeDirectory(str(p, "directory"));
  if (m == "get_settings") return SettingsStore::get().toJson();
  if (m == "update_settings") {
    AdvancedSettings next;
    try {
      next = AdvancedSettings::fromJson(object(p, "settings"));
    } catch (const Error &error) {
      fail(QStringLiteral("settings: %1").arg(error.message()));
    }
    return SettingsStore::update(next).toJson();
  }
  if (m == "reset_settings") return SettingsStore::reset().toJson();
  if (m == "set_clipboard_files") {
    clipboard::setClipboardFiles(strings(p, "paths"));
    return QJsonValue();
  }
  if (m == "native_geometry") return catalog::nativeGeometry(query(p));
  if (m == "native_default_settings") return defaultSettings().toJson();
  if (m == "native_delete_media_batch") return media_delete::nativeDeleteBatch(str(p, "albumPath"), media_delete::DeleteTarget::fromJson(at(p, "targets"))).toJson();
  if (m == "vault_status") return vault_ops::status(str(p, "root"));
  if (m == "vault_factors") return vault_ops::factors();
  if (m == "vault_setup")
    return vault_ops::setup(str(p, "root"), str(p, "password"), strings(p, "factors"), optStr(p, "decoyPassword").value_or(QString()), optStr(p, "reason").value_or(QString()), optStr(p, "pin").value_or(QString()));
  if (m == "vault_unlock")
    return vault_ops::unlock(str(p, "root"), str(p, "password"), str(p, "kind"), optStr(p, "reason").value_or(QString()), optStr(p, "pin").value_or(QString()), optStr(p, "recoveryKey").value_or(QString()));
  if (m == "vault_resume") return vault_ops::resume(str(p, "root"), str(p, "kind"), optStr(p, "reason").value_or(QString()), optStr(p, "pin").value_or(QString()));
  if (m == "vault_lock") {
    vault_ops::lock(str(p, "root"));
    return QJsonValue();
  }
  if (m == "vault_lock_all") {
    vault_ops::lockAll();
    return QJsonValue();
  }
  if (m == "vault_encrypt") {
    vault_ops::encrypt(str(p, "root"));
    return QJsonValue();
  }
  if (m == "vault_decrypt") {
    vault_ops::decrypt(str(p, "root"));
    return QJsonValue();
  }
  if (m == "vault_add_factor") {
    vault_ops::addFactor(str(p, "root"), str(p, "kind"), optStr(p, "reason").value_or(QString()), optStr(p, "pin").value_or(QString()));
    return QJsonValue();
  }
  if (m == "vault_verify_password") return vault_ops::verifyPassword(str(p, "root"), str(p, "password"));
  if (m == "ping") return QJsonObject{{QStringLiteral("protocol"), 1}, {QStringLiteral("engine"), QStringLiteral("native")}, {QStringLiteral("webview"), false}};
  fail(QStringLiteral("Unknown command: %1").arg(method));
}
}  // namespace

void initialize(const QString &profile, const QString &legacyProfile) {
  if (!path::isAbsolute(profile)) fail(QStringLiteral("Profile must be absolute"));
  if (!legacyProfile.isEmpty()) maintenance::migrateProfile(profile, legacyProfile);
  SettingsStore::load(profile);
}

QJsonValue dispatch(const QString &method, const QJsonObject &params) {
  QJsonValue result = run(method, params);
  enrich(result);
  return result;
}

}  // namespace core
