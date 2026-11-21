#include "directory_move.h"
#include "album_tree.h"
#include "file_access.h"
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace core::directory_move {
namespace {
const QString kService = QStringLiteral(".room237-transactions");
const QString kPrefix = QStringLiteral("album-");
const QString kIntent = QStringLiteral("intent.json");
// Two native Windows long paths (32767 UTF-16 units each), worst-case JSON \uXXXX escaping, plus fixed fields and
// decimal device/file ids. This exceeds the native pathname contract on macOS/Linux; no valid old rename is capped.
constexpr qint64 kIntentBytes = 2 * 32767 * 6 + 4096;

QString relative(const QString &input) {
  if (input.isEmpty() || path::isAbsolute(input) || input.contains(QChar(0))) fail(QStringLiteral("Invalid directory transaction relative path"));
  const QString normalized = QDir::fromNativeSeparators(input);
  const QStringList components = normalized.split(QLatin1Char('/'));
  for (const auto &part : components) {
    if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..") || part.startsWith(QLatin1String(".room237-")))
      fail(QStringLiteral("Invalid directory transaction path component"));
#ifdef Q_OS_WIN
    if (part.contains(QLatin1Char(':'))) fail(QStringLiteral("Invalid directory transaction stream path"));
#endif
  }
  return components.join(kSeparator);
}
QString parentRelative(const QString &p) { return path::parent(p); }
QString idOf(const FileInfo &info) {
  if (!info.isDir || info.isSymlink) return {};
#ifdef Q_OS_WIN
  return QStringLiteral("%1-%2-%3").arg(info.dev).arg(info.ino >> 32).arg(info.ino & 0xffffffff);
#else
  return QStringLiteral("%1-%2").arg(info.dev).arg(info.ino);
#endif
}
struct Intent {
  QString transaction, root, source, target, sourceId, sourceParent, targetParent;
  QByteArray bytes() const {
    return QJsonDocument(QJsonObject{{"version", 1}, {"transaction", transaction}, {"root_id", root}, {"source", source},
        {"target", target}, {"source_id", sourceId}, {"source_parent_id", sourceParent}, {"target_parent_id", targetParent}}).toJson(QJsonDocument::Compact);
  }
};
Intent parse(const QByteArray &bytes, const QString &name) {
  if (bytes.size() > kIntentBytes) fail(QStringLiteral("Directory transaction intent exceeds native path bounds; preserving it"));
  QJsonParseError error;
  const auto doc = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) fail(QStringLiteral("Malformed directory transaction; preserving it"));
  const auto object = doc.object();
  if (object.size() != 8 || !object.value("version").isDouble() || object.value("version").toDouble() != 1.0) fail(QStringLiteral("Unsupported directory transaction; preserving it"));
  auto text = [&](const char *field) {
    const auto value = object.value(QLatin1String(field));
    if (!value.isString() || value.toString().isEmpty()) fail(QStringLiteral("Missing directory transaction field"));
    return value.toString();
  };
  Intent intent{text("transaction"), text("root_id"), relative(text("source")), relative(text("target")), text("source_id"), text("source_parent_id"), text("target_parent_id")};
  static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{32}$"));
  if (!uuid.match(intent.transaction).hasMatch() || name != kPrefix + intent.transaction || intent.source == intent.target || path::startsWith(intent.target, intent.source))
    fail(QStringLiteral("Directory transaction membership/path mismatch; preserving it"));
  return intent;
}
std::optional<Directory> service(const Directory &library, bool create) {
  auto directory = library.optionalChild(kService, true);
  if (!directory && create) directory.emplace(library.privateChild(kService));
  if (directory) directory->requirePrivate();
  return directory;
}
void cleanup(const Directory &parent, const QString &name, const Directory &stage) {
  // Validate before removing even the journal: unknown files must retain the only recovery record.
  const auto entries = stage.entries();
  if (entries.size() != 1 || entries.front().name != kIntent || !entries.front().isFile)
    fail(QStringLiteral("Unknown directory transaction entries; preserving journal"));
  stage.removeFileIfPresent(kIntent); stage.sync(); parent.removeEmpty(name, &stage); parent.sync();
}
void validateParents(const Directory &library, const Intent &intent, const Directory &source, const Directory &target) {
  if (library.identity() != intent.root || source.identity() != intent.sourceParent || target.identity() != intent.targetParent)
    fail(QStringLiteral("Directory transaction parent identity changed; preserving journal"));
  source.nativeHandle(); target.nativeHandle();
}
Directory publishedDirectory(const Directory &stage, const Directory &destination, const QString &name,
                             const Directory &admitted, const QString &identity) {
  const auto named = destination.info(name);
  if (idOf(named) != identity) {
    // POSIX rename cannot name an expected inode. If the private entry was exchanged immediately before it,
    // claim the published replacement back without clobbering anything; retain it and the intent on failure.
    if (named.exists) destination.claim(name, stage, "entry", named);
    fail(QStringLiteral("Directory publication changed; preserving claim and journal"));
  }
  return admitted.rebound(destination, name);
}
}  // namespace

void move(const QString &root, const Directory &library, const QString &sourceRelative, const QString &targetRelative,
          const Directory &admitted, const Hook &checkpoint) {
  const QString source = relative(sourceRelative), target = relative(targetRelative);
  if (source == target || path::startsWith(target, source)) fail(QStringLiteral("Invalid directory move destination"));
  auto sourceParent = library.descend(parentRelative(source)), targetParent = library.descend(parentRelative(target));
  const QString sourceName = path::fileName(source), targetName = path::fileName(target);
  if (idOf(sourceParent.info(sourceName)) != admitted.identity()) fail(QStringLiteral("Album changed before directory transaction"));
  auto transactions = service(library, true);
  const QString transaction = QUuid::createUuid().toString(QUuid::Id128), name = kPrefix + transaction;
  auto stage = transactions->privateChild(name);
  Intent intent{transaction, library.identity(), source, target, admitted.identity(), sourceParent.identity(), targetParent.identity()};
  const auto bytes = intent.bytes();
  if (bytes.size() > kIntentBytes) fail(QStringLiteral("Directory intent exceeds native path bounds"));
  stage.writeAtomic(kIntent, bytes); stage.sync(); transactions->sync(); library.sync();
  checkpoint(QStringLiteral("directory_intent"));
  validateParents(library, intent, sourceParent, targetParent);
  checkpoint(QStringLiteral("directory_before_claim"));
  QString error;
  if (!sourceParent.renameTo(sourceName, stage, "entry", false, &error, nullptr, &admitted)) fail(error);
  sourceParent.sync(); stage.sync();
  auto claimed = admitted.rebound(stage, "entry");  // A mismatch retains the exchanged entry and the durable intent.
  if (claimed.identity() != intent.sourceId) fail(QStringLiteral("Claimed album identity changed; preserving it"));
  checkpoint(QStringLiteral("directory_claimed"));
  checkpoint(QStringLiteral("directory_before_publication"));
  validateParents(library, intent, sourceParent, targetParent);
  if (!stage.renameTo("entry", targetParent, targetName, false, &error, nullptr, &claimed)) {
    checkpoint(QStringLiteral("directory_before_rollback"));
    QString rollback;
    if (!stage.renameTo("entry", sourceParent, sourceName, false, &rollback, nullptr, &claimed))
      fail(QStringLiteral("%1; directory rollback: %2; preserved with journal").arg(error, rollback));
    auto restored = publishedDirectory(stage, sourceParent, sourceName, claimed, intent.sourceId);
    if (restored.identity() != intent.sourceId) fail(QStringLiteral("Restored album identity changed; preserving journal"));
    sourceParent.sync(); stage.sync(); checkpoint(QStringLiteral("directory_rolled_back"));
    cleanup(*transactions, name, stage);
    fail(error);
  }
  auto published = publishedDirectory(stage, targetParent, targetName, claimed, intent.sourceId);
  if (published.identity() != intent.sourceId) fail(QStringLiteral("Published album changed; preserving journal"));
  targetParent.sync(); stage.sync(); checkpoint(QStringLiteral("directory_published"));
  if (auto *view = FileAccess::current(root)) view->bindDirectory(path::join(root, target), std::make_shared<Directory>(published.share()));
  cleanup(*transactions, name, stage);
}

void recover(const QString &requestedRoot, const Hook &checkpoint) {
  const QString root = canonical(requestedRoot);
  FileAccess access({root});
  auto library = access.directory(root);
  auto transactions = service(*library, false);
  if (!transactions) return;
  if (transactions->entries().empty()) return;
  // Same ordering as an album rename: initialize its marker first, then exclude producers before claiming anything.
  album_tree::initialize(root);
  auto tree = album_tree::write(root);
  for (const auto &entry : transactions->entries()) {
    if (!entry.name.startsWith(kPrefix)) fail(QStringLiteral("Unknown library transaction; preserving it"));
    if (!entry.isDir) fail(QStringLiteral("Directory transaction is not a regular directory; preserving it"));
    auto stage = transactions->childForRemoval(entry.name); stage.requirePrivate();
    if (!stage.info(kIntent).exists) {
      if (stage.info("entry").exists) fail(QStringLiteral("Claimed album has no intent; preserving it"));
      // A crash before intent publication cannot have moved media. Preserve incomplete files without following them.
      qWarning("Preserved incomplete directory transaction %s", qUtf8Printable(entry.name));
      continue;
    }
    auto file = stage.openRegular(kIntent);
    const QByteArray bytes = file->read(kIntentBytes + 1);
    const auto intent = parse(bytes, entry.name);
    auto sourceParent = library->descend(parentRelative(intent.source)), targetParent = library->descend(parentRelative(intent.target));
    validateParents(*library, intent, sourceParent, targetParent);
    checkpoint(QStringLiteral("directory_recovery_loaded"));
    const QString sourceName = path::fileName(intent.source), targetName = path::fileName(intent.target);
    const auto source = sourceParent.info(sourceName), target = targetParent.info(targetName), claimed = stage.info("entry");
    if (claimed.exists) {
      if (!claimed.isDir || idOf(claimed) != intent.sourceId || source.exists) fail(QStringLiteral("Directory recovery is ambiguous; preserving all entries"));
      auto admitted = stage.childForRemoval("entry");
      if (admitted.identity() != intent.sourceId) fail(QStringLiteral("Directory claim changed during recovery"));
      checkpoint(QStringLiteral("directory_recovery_before_restore"));
      validateParents(*library, intent, sourceParent, targetParent);
      QString error;
      if (!stage.renameTo("entry", sourceParent, sourceName, false, &error, nullptr, &admitted)) fail(error);
      auto restored = publishedDirectory(stage, sourceParent, sourceName, admitted, intent.sourceId);
      if (restored.identity() != intent.sourceId) fail(QStringLiteral("Recovered album identity changed; preserving journal"));
      sourceParent.sync(); stage.sync(); checkpoint(QStringLiteral("directory_recovery_restored"));
    } else if (!(idOf(source) == intent.sourceId || (idOf(target) == intent.sourceId && !source.exists))) {
      fail(QStringLiteral("Directory transaction cannot identify its album; preserving journal"));
    }
    checkpoint(QStringLiteral("directory_recovery_before_cleanup"));
    file.reset();  // Close the read handle before unlinking the journal on Windows.
    cleanup(*transactions, entry.name, stage);
  }
}
}  // namespace core::directory_move
