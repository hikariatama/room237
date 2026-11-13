// Similarity index and group membership are disk backed. Working memory is independent of the number of
// photographs, candidates and duplicate groups.
#include "album_db.h"
#include "album_tree.h"
#include "constants.h"
#include "db.h"
#include "duplicates.h"
#include "media_delete.h"
#include "metadata.h"
#include "thumb.h"
#include "util.h"

#include <QCryptographicHash>
#include <QFuture>
#include <QHash>
#include <QSet>
#include <QThreadPool>
#include <QtConcurrent>
#include <atomic>
#include <list>
#include <mutex>
#include <unordered_map>

namespace core::duplicates {

namespace {
constexpr int kPage = 128;
constexpr size_t kHashBlockFiles = 1024;
constexpr size_t kHashCacheBytes = 1024 * 1024;
constexpr size_t kCandidateHashPage = 64;
std::atomic<quint64> request {0};
std::mutex scanMutex;

void check(quint64 id) {
  if (request.load(std::memory_order_relaxed) != id) fail(QStringLiteral("Duplicate scan superseded"));
}
db::Connection open(const QString &p) {
  db::Connection conn(p);
  conn.busyTimeout(30000);
  conn.executeBatch(QStringLiteral("PRAGMA cache_size=-2048; PRAGMA temp_store=FILE; PRAGMA temp.cache_size=-2048; PRAGMA mmap_size=0;"));
  return conn;
}
QByteArray bytesOf(const quint64 *bits, size_t words) {
  QByteArray out(qsizetype(words * 8), Qt::Uninitialized);
  for (size_t i = 0; i < words; ++i)
    for (int b = 0; b < 8; ++b) out[qsizetype(i * 8 + size_t(b))] = char((bits[i] >> (8 * b)) & 0xff);
  return out;
}
std::vector<quint64> decodeWords(const QByteArray &data) {
  if (data.size() % 8 != 0) fail(QStringLiteral("Invalid MIH integer block"));
  std::vector<quint64> out(size_t(data.size() / 8));
  for (size_t i = 0; i < out.size(); ++i) {
    quint64 word = 0;
    for (int b = 0; b < 8; ++b) word |= quint64(quint8(data[qsizetype(i * 8 + size_t(b))])) << (8 * b);
    out[i] = word;
  }
  return out;
}
qint64 integer(quint64 value) {
  if (value > quint64(std::numeric_limits<qint64>::max())) fail(QStringLiteral("out of range integral type conversion attempted"));
  return qint64(value);
}

// Fixed-capacity LRU pages, shared by hash verification and MIH postings.
template <typename K, typename Hash = std::hash<K>> class Pages {
public:
  explicit Pages(size_t capacity) : capacity_(std::max<size_t>(capacity, 1)) {}
  const std::vector<quint64> &get(const K &key, const std::function<std::vector<quint64>()> &load) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      order_.splice(order_.begin(), order_, it->second);
      return it->second->second;
    }
    std::vector<quint64> values = load();
    if (order_.size() >= capacity_) {
      map_.erase(order_.back().first);
      order_.pop_back();
    }
    order_.emplace_front(key, std::move(values));
    map_[key] = order_.begin();
    return order_.front().second;
  }
private:
  size_t capacity_;
  std::list<std::pair<K, std::vector<quint64>>> order_;
  std::unordered_map<K, typename std::list<std::pair<K, std::vector<quint64>>>::iterator, Hash> map_;
};
struct PairHash {
  size_t operator()(const std::pair<size_t, quint64> &key) const { return std::hash<quint64>()(quint64(key.first) * 0x9e3779b97f4a7c15ULL ^ key.second); }
};

// --- Inventory and fingerprints ----------------------------------------------------------------------------------

struct Item {
  QString name;
  QString source;    // Stamp of the original alone; previews are regenerated without changing it.
  QString revision;  // Original and preview together, as mediaRevision reports it.
};

std::vector<Item> inventory(const QString &dir, quint64 id) {
  QStringList names;
  const QSet<QString> hidden = metadata::hiddenHeicNames(dir);
  for (const DirEntry &file : readDir(dir)) {
    check(id);
    if (!file.isFile || !metadata::isImageMedia(dir, file.name) || hidden.contains(file.name)) continue;
    names << file.name;
  }
  // Stamps are read on a small pool: on network and external volumes each stat is a round trip.
  QThreadPool pool;
  pool.setMaxThreadCount(8);
  const QList<std::optional<Item>> items = QtConcurrent::blockingMapped(&pool, names, [&](const QString &name) -> std::optional<Item> {
    if (request.load(std::memory_order_relaxed) != id) return std::nullopt;
    const auto [source, preview] = metadata::mediaRevisionParts(path::join(dir, name));
    return Item{name, source, source + QLatin1Char('-') + preview};
  });
  check(id);
  std::vector<Item> out;
  for (const auto &item : items)
    if (item) out.push_back(*item);
  return out;
}

// Keep the v3 fingerprint compatible with already computed results.
QByteArray sourceFingerprint(const std::vector<Item> &items, const AdvancedSettings &settings) {
  QByteArray data("disk-duplicate-groups-v3");
  data += settings.hashConfig().hashVersion.toUtf8();
  const quint32 threshold = settings.duplicates.effectiveThreshold();
  for (int b = 0; b < 4; ++b) data += char((threshold >> (8 * b)) & 0xff);
  for (const Item &item : items) {
    data += item.name.toUtf8();
    data += '\0';
    data += item.revision.toUtf8();
    data += '\0';
  }
  return data;
}
QString finishFingerprint(QByteArray data, db::Connection &conn, const QString &table) {
  // table is a caller-owned literal, never input from the interface.
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT a,b FROM %1 ORDER BY a,b").arg(table));
  while (stmt.step()) {
    data += stmt.text(0).toUtf8();
    data += '\0';
    data += stmt.text(1).toUtf8();
    data += '\0';
  }
  return sha256Hex(data);
}
QString fingerprint(const std::vector<Item> &items, const QString &dir, const AdvancedSettings &settings) {
  db::Connection conn = open(path::join(dir, QStringLiteral(".room237-metadata/album.sqlite")));
  return finishFingerprint(sourceFingerprint(items, settings), conn, QStringLiteral("ignored_pairs"));
}

// --- Hash blocks -------------------------------------------------------------------------------------------------

// Fixed memory budget, fully used at each hash length. MIH reads fine pages with SQLite substr(), so a sparse
// candidate does not decode 1024 hashes.
class HashBlocks {
public:
  HashBlocks(size_t words, size_t files) : words(words), files(files), cached_(kHashCacheBytes / (files * words * 8)) {}
  const std::vector<quint64> &get(db::Connection &conn, size_t block) {
    return cached_.get(block, [&] {
      const size_t sourceBlock = block * files / kHashBlockFiles;
      const size_t start = (block * files % kHashBlockFiles) * words * 8 + 1;
      const size_t size = files * words * 8;
      db::Statement stmt = conn.prepare(QStringLiteral("SELECT substr(data,?2,?3) FROM hash_blocks WHERE block=?1"));
      stmt.bind(1, qint64(sourceBlock)).bind(2, qint64(start)).bind(3, qint64(size));
      if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
      const QByteArray data = stmt.blob(0);
      if (data.isEmpty() || size_t(data.size()) % (words * 8) != 0 || size_t(data.size()) > size) fail(QStringLiteral("Invalid duplicate hash block"));
      return decodeWords(data);
    });
  }
  size_t words, files;
private:
  Pages<size_t> cached_;
};

void merge(db::Connection &conn, const QString &a, const QString &b) {
  const QString lookup = QStringLiteral("SELECT n.root,c.restricted FROM nodes n JOIN components c ON c.root=n.root WHERE n.name=?1");
  auto find = [&](const QString &name) {
    db::Statement stmt = conn.prepare(lookup);
    stmt.bind(1, name);
    if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
    return std::make_pair(stmt.text(0), stmt.integer(1) != 0);
  };
  const auto [left, leftRestricted] = find(a);
  const auto [right, rightRestricted] = find(b);
  if (left == right) return;
  // An exclusion can cross the components only if both contain endpoints.
  bool blocked = false;
  if (leftRestricted && rightRestricted) {
    db::Statement stmt = conn.prepare(QStringLiteral(
        "SELECT EXISTS(SELECT 1 FROM nodes a INDEXED BY roots CROSS JOIN ignored i CROSS JOIN nodes b WHERE a.root=?1 AND i.a=a.name AND b.name=i.b AND b.root=?2 "
        "UNION ALL SELECT 1 FROM nodes a INDEXED BY roots CROSS JOIN ignored i INDEXED BY ignored_b CROSS JOIN nodes b WHERE a.root=?1 AND i.b=a.name AND b.name=i.a AND b.root=?2)"));
    stmt.bind(1, left).bind(2, right);
    stmt.step();
    blocked = stmt.integer(0) != 0;
  }
  if (blocked) return;
  const QString &keep = left < right ? left : right;
  const QString &remove = left < right ? right : left;
  conn.execute(QStringLiteral("UPDATE nodes SET root=?1 WHERE root=?2"), {keep, remove});
  conn.execute(QStringLiteral("UPDATE components SET restricted=?2 WHERE root=?1"), {keep, qint64(leftRestricted || rightRestricted)});
  conn.execute(QStringLiteral("DELETE FROM components WHERE root=?1"), {remove});
}

QString nameForId(db::Connection &conn, quint64 id) {
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT name FROM hash_names WHERE id=?1"));
  stmt.bind(1, integer(id));
  if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
  return stmt.text(0);
}

void verifyCandidates(db::Connection &conn, HashBlocks &cache, const quint64 *bits, const QString &name, const std::vector<quint64> &ids, quint32 threshold, quint64 id, qint64 &comparisons) {
  const size_t files = cache.files, words = cache.words;
  size_t first = 0;
  while (first < ids.size()) {
    check(id);
    const size_t block = size_t(ids[first]) / files;
    size_t end = first + 1;
    while (end < ids.size() && size_t(ids[end]) / files == block) ++end;
    const std::vector<quint64> hashes = cache.get(conn, block);
    for (size_t i = first; i < end; ++i) {
      const size_t offset = (size_t(ids[i]) % files) * words;
      if (offset + words > hashes.size()) fail(QStringLiteral("Invalid MIH candidate ID"));
      ++comparisons;
      if (withinThreshold(bits, hashes.data() + offset, words, threshold)) merge(conn, name, nameForId(conn, ids[i]));
    }
    first = end;
  }
}

// --- Multi-index Hamming search ----------------------------------------------------------------------------------

namespace mih {
constexpr size_t kDirectoryBudget = 12 * 1024 * 1024;
constexpr size_t kPostingIds = 512;
constexpr size_t kPostingPages = 1024;  // 4 MiB of decoded IDs.
constexpr size_t kCandidateIds = 8192;  // 64 KiB; excess candidates spill to SQLite.
constexpr quint64 kMaxProbes = 4096;

struct Part {
  size_t offset;
  quint8 bits, radius;
};
quint64 volume(quint8 bits, quint8 radius) {
  quint64 term = 1, sum = 1;
  for (quint64 k = 1; k <= std::min(radius, bits); ++k) {
    term = term * (bits - k + 1) / k;
    sum += term;
  }
  return sum;
}
struct Plan {
  std::vector<Part> parts;
  quint64 probes = 0;
  static Plan partition(size_t bits, size_t tables, quint32 threshold) {
    const size_t small = bits / tables, longParts = bits % tables;
    const size_t radius = threshold / tables, high = threshold % tables + 1;
    size_t offset = 0;
    Plan plan;
    for (size_t table = 0; table < tables; ++table) {
      const size_t width = small + (table < longParts ? 1 : 0);
      std::optional<size_t> r;
      if (table < high) r = radius;
      else if (radius >= 1) r = radius - 1;
      if (r) plan.parts.push_back({offset, quint8(width), quint8(std::min(*r, width))});
      offset += width;
    }
    for (const Part &p : plan.parts) plan.probes += volume(p.bits, p.radius);
    return plan;
  }
  size_t directoryBytes() const {
    size_t total = 0;
    for (const Part &p : parts) total += ((size_t(1) << p.bits) + 1) * 8;
    return total;
  }
  static std::optional<Plan> choose(size_t count, size_t bits, quint32 threshold) {
    if (count < 512 || threshold == 0) return std::nullopt;
    const double pairs = double(count) * double(count - 1) / 2.0;
    std::optional<std::pair<double, Plan>> best;
    const size_t minimum = (bits + 15) / 16;
    const size_t maximum = std::min(bits, std::max(size_t(threshold) + 1, minimum));
    for (size_t tables = minimum; tables <= maximum; ++tables) {
      Plan plan = partition(bits, tables, threshold);
      if (plan.probes > kMaxProbes || plan.directoryBytes() > kDirectoryBudget) continue;
      double noHit = 1.0;
      for (const Part &part : plan.parts) noHit *= 1.0 - double(volume(part.bits, part.radius)) / double(quint64(1) << part.bits);
      // A candidate costs more than a tight scan comparison: its posting must be fetched, deduplicated, sorted and
      // verified. Real bucket populations are guarded again at query time.
      const double work = double(count) * double(plan.probes) + pairs * (1.0 - noHit) * 4.0;
      if (work < pairs * 0.6 && (!best || work < best->first)) best.emplace(work, plan);
    }
    if (!best) return std::nullopt;
    return best->second;
  }
};
quint16 extract(const quint64 *words, const Part &part) {
  const size_t word = part.offset / 64, shift = part.offset % 64;
  quint64 value = words[word] >> shift;
  if (shift + part.bits > 64) value |= words[word + 1] << (64 - shift);
  return quint16(value & ((quint64(1) << part.bits) - 1));
}
// Gosper's hack over every mask of weight 0..radius.
std::vector<quint16> masks(quint8 bits, quint8 radius) {
  std::vector<quint16> out;
  radius = std::min(radius, bits);
  quint8 weight = 0;
  std::optional<quint32> next = 0;
  while (next) {
    const quint32 value = *next;
    quint32 following;
    if (value == 0) following = 1;
    else {
      const quint32 low = value & (~value + 1);
      const quint32 ripple = value + low;
      following = (((ripple ^ value) >> 2) / low) | ripple;
    }
    if (value == 0 || following >= (quint32(1) << bits)) {
      ++weight;
      next = weight <= radius ? std::optional<quint32>((quint32(1) << weight) - 1) : std::nullopt;
    } else {
      next = following;
    }
    out.push_back(quint16(value));
  }
  return out;
}

struct Candidates {
  bool disk = false;
  std::vector<quint64> memory;
  static std::vector<quint64> page(db::Connection &conn, std::optional<quint64> after) {
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT id FROM mih_candidates WHERE id>?1 ORDER BY id LIMIT 256"));
    stmt.bind(1, after ? integer(*after) : qint64(-1));
    std::vector<quint64> ids;
    while (stmt.step()) ids.push_back(quint64(stmt.integer(0)));
    return ids;
  }
};

class Collector {
public:
  void push(db::Connection &conn, quint64 id) {
    values_.push_back(id);
    if (values_.size() == kCandidateIds) {
      flush(conn);
      spilled_ = true;
    }
  }
  Candidates finish(db::Connection &conn) {
    Candidates out;
    if (spilled_) {
      flush(conn);
      out.disk = true;
    } else {
      std::sort(values_.begin(), values_.end());
      values_.erase(std::unique(values_.begin(), values_.end()), values_.end());
      out.memory = std::move(values_);
    }
    return out;
  }
private:
  void flush(db::Connection &conn) {
    std::sort(values_.begin(), values_.end());
    values_.erase(std::unique(values_.begin(), values_.end()), values_.end());
    for (size_t at = 0; at < values_.size(); at += 128) {
      const size_t n = std::min<size_t>(128, values_.size() - at);
      QStringList placeholders;
      for (size_t i = 0; i < n; ++i) placeholders << QStringLiteral("(?%1)").arg(i + 1);
      db::Statement stmt = conn.prepare(QStringLiteral("INSERT OR IGNORE INTO mih_candidates VALUES %1").arg(placeholders.join(QLatin1Char(','))));
      for (size_t i = 0; i < n; ++i) stmt.bind(int(i + 1), integer(values_[at + i]));
      stmt.execute();
    }
    values_.clear();
  }
  std::vector<quint64> values_;
  bool spilled_ = false;
};

class Index {
public:
  static Index build(db::Connection &conn, const Plan &plan, size_t count, size_t words, const std::function<void()> &cancel) {
    {
      db::Transaction tx(conn);
      conn.executeBatch(QStringLiteral(
          "CREATE TABLE mih_plan(part INTEGER PRIMARY KEY,offset INTEGER,bits INTEGER,radius INTEGER);"
          "CREATE TABLE mih_directories(part INTEGER PRIMARY KEY,data BLOB NOT NULL);"
          "CREATE TABLE mih_postings(part INTEGER,page INTEGER,data BLOB NOT NULL,PRIMARY KEY(part,page)) WITHOUT ROWID;"
          "CREATE TEMP TABLE mih_keys(part INTEGER,key INTEGER,id INTEGER);"
          "CREATE TEMP TABLE mih_candidates(id INTEGER PRIMARY KEY) WITHOUT ROWID;"));
      QStringList values;
      for (size_t n = 0; n < plan.parts.size(); ++n) values << QStringLiteral("(?%1,?%2,?%3)").arg(n * 3 + 1).arg(n * 3 + 2).arg(n * 3 + 3);
      db::Statement insert = conn.prepare(QStringLiteral("INSERT INTO mih_keys VALUES %1").arg(values.join(QLatin1Char(','))));
      size_t id = 0;
      while (id < count) {
        cancel();
        db::Statement block = conn.prepare(QStringLiteral("SELECT data FROM hash_blocks WHERE block=?1"));
        block.bind(1, qint64(id / 1024));
        if (!block.step()) fail(QStringLiteral("Query returned no rows"));
        const std::vector<quint64> data = decodeWords(block.blob(0));
        if (data.empty() || data.size() % words != 0 || data.size() > 1024 * words) fail(QStringLiteral("Invalid source hash block for MIH"));
        for (size_t at = 0; at + words <= data.size(); at += words) {
          if (id >= count) break;
          if (id % 256 == 0) cancel();
          insert.reset();
          for (size_t part = 0; part < plan.parts.size(); ++part) {
            insert.bind(int(part * 3 + 1), qint64(part));
            insert.bind(int(part * 3 + 2), qint64(extract(data.data() + at, plan.parts[part])));
            insert.bind(int(part * 3 + 3), integer(id));
          }
          insert.execute();
          ++id;
        }
      }
      conn.executeBatch(QStringLiteral("CREATE INDEX temp.mih_keys_order ON mih_keys(part,key,id);"));
      for (size_t part = 0; part < plan.parts.size(); ++part) {
        cancel();
        const Part &p = plan.parts[part];
        std::vector<quint64> directory((size_t(1) << p.bits) + 1, 0);
        size_t key = 0;
        quint64 offset = 0, page = 0;
        std::vector<quint64> ids;
        auto publish = [&] {
          conn.execute(QStringLiteral("INSERT INTO mih_postings VALUES(?1,?2,?3)"), {qint64(part), integer(page), bytesOf(ids.data(), ids.size())});
          ++page;
          ids.clear();
        };
        db::Statement rows = conn.prepare(QStringLiteral("SELECT key,id FROM mih_keys WHERE part=?1 ORDER BY key,id"));
        rows.bind(1, qint64(part));
        while (rows.step()) {
          if (offset % 256 == 0) cancel();
          const size_t next = size_t(rows.integer(0));
          const quint64 member = quint64(rows.integer(1));
          while (key <= next) directory[key++] = offset;
          ids.push_back(member);
          ++offset;
          if (ids.size() == kPostingIds) publish();
        }
        if (!ids.empty()) publish();
        while (key < directory.size()) directory[key++] = offset;
        conn.execute(QStringLiteral("INSERT INTO mih_directories VALUES(?1,?2)"), {qint64(part), bytesOf(directory.data(), directory.size())});
        conn.execute(QStringLiteral("INSERT INTO mih_plan VALUES(?1,?2,?3,?4)"), {qint64(part), qint64(p.offset), qint64(p.bits), qint64(p.radius)});
      }
      conn.executeBatch(QStringLiteral("DROP TABLE mih_keys;"));
      tx.commit();
    }
    Index index;
    index.plan_ = plan;
    for (size_t part = 0; part < plan.parts.size(); ++part) {
      db::Statement stmt = conn.prepare(QStringLiteral("SELECT data FROM mih_directories WHERE part=?1"));
      stmt.bind(1, qint64(part));
      if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
      index.directories_.push_back(decodeWords(stmt.blob(0)));
    }
    return index;
  }

  std::optional<Candidates> query(db::Connection &conn, const quint64 *bits, quint64 before, const std::function<void()> &cancel) {
    cancel();
    if (before < plan_.probes * 2) {
      ++fallbacks;
      return std::nullopt;
    }
    conn.execute(QStringLiteral("DELETE FROM mih_candidates"));
    Collector collector;
    quint64 hits = 0;
    const quint64 budget = before / 2;
    for (size_t part = 0; part < plan_.parts.size(); ++part) {
      const Part &p = plan_.parts[part];
      const quint16 value = extract(bits, p);
      for (quint16 mask : masks(p.bits, p.radius)) {
        if (probes % 32 == 0) cancel();
        ++probes;
        const size_t key = size_t(value ^ mask);
        const quint64 start = directories_[part][key], end = directories_[part][key + 1];
        quint64 position = start;
        while (position < end) {
          const quint64 page = position / kPostingIds;
          const std::vector<quint64> &ids = postings_.get({part, page}, [&] {
            db::Statement stmt = conn.prepare(QStringLiteral("SELECT data FROM mih_postings WHERE part=?1 AND page=?2"));
            stmt.bind(1, qint64(part)).bind(2, integer(page));
            if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
            const QByteArray data = stmt.blob(0);
            if (data.isEmpty() || size_t(data.size()) > kPostingIds * 8) fail(QStringLiteral("Invalid MIH posting page"));
            return decodeWords(data);
          });
          const size_t offset = size_t(position % kPostingIds);
          if (offset >= ids.size()) fail(QStringLiteral("Invalid MIH posting range"));
          const size_t length = size_t(std::min<quint64>(end - position, ids.size() - offset));
          const std::vector<quint64> slice(ids.begin() + std::ptrdiff_t(offset), ids.begin() + std::ptrdiff_t(offset + length));
          bool finished = false;
          for (quint64 member : slice) {
            if (member >= before) {
              finished = true;
              break;
            }
            ++hits;
            if (hits % 256 == 0) cancel();
            if (hits > budget) {
              ++fallbacks;
              return std::nullopt;
            }
            collector.push(conn, member);
          }
          if (finished) break;
          position += length;
        }
      }
    }
    return collector.finish(conn);
  }

  quint64 probes = 0, fallbacks = 0;
private:
  Plan plan_;
  std::vector<std::vector<quint64>> directories_;
  Pages<std::pair<size_t, quint64>, PairHash> postings_ {kPostingPages};
};
}  // namespace mih

// --- Deletion-only repair ----------------------------------------------------------------------------------------

QString configuration(const AdvancedSettings &settings) { return settings.hashConfig().hashVersion + QLatin1Char(':') + QString::number(settings.duplicates.effectiveThreshold()); }

void rememberConfiguration(db::Connection &conn, const AdvancedSettings &settings) {
  // Upgrade a validated legacy index without throwing away its expensive search.
  conn.executeBatch(QStringLiteral("CREATE TABLE IF NOT EXISTS configuration(value TEXT NOT NULL)"));
  conn.execute(QStringLiteral("INSERT INTO configuration SELECT ?1 WHERE NOT EXISTS(SELECT 1 FROM configuration)"), {configuration(settings)});
}

bool queryBool(db::Connection &conn, const QString &sql) {
  db::Statement stmt = conn.prepare(sql);
  stmt.step();
  return stmt.integer(0) != 0;
}

bool pruneDeletions(const QString &dir, db::Connection &conn, const AdvancedSettings &settings, const QString &signature, const std::vector<Item> &items, quint64 id) {
  std::optional<QString> known;
  try {
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT value FROM configuration"));
    if (stmt.step()) known = stmt.text(0);
  } catch (const Error &) {
  }
  if (known != configuration(settings)) return false;
  auto metadataGuard = metadata::lockMetadataForWrite({dir});
  {
    db::Connection album = open(path::join(dir, QStringLiteral(".room237-metadata/album.sqlite")));
    if (finishFingerprint({}, album, QStringLiteral("ignored_pairs")) != finishFingerprint({}, conn, QStringLiteral("ignored"))) return false;
  }
  db::Transaction tx(conn);
  conn.executeBatch(QStringLiteral("CREATE TEMP TABLE live(name TEXT PRIMARY KEY,source TEXT NOT NULL,revision TEXT NOT NULL)"));
  {
    db::Statement insert = conn.prepare(QStringLiteral("INSERT INTO live VALUES(?1,?2,?3)"));
    for (const Item &item : items) {
      check(id);
      insert.reset();
      insert.bind(1, item.name).bind(2, item.source).bind(3, item.revision).execute();
    }
  }
  // Only the original decides whether a hash still describes the file. A regenerated preview must not throw away the
  // index.
  if (queryBool(conn, QStringLiteral("SELECT EXISTS(SELECT 1 FROM live l LEFT JOIN nodes n ON n.name=l.name WHERE n.name IS NULL OR substr(n.revision,1,length(l.source)+1)<>l.source||'-')"))) return false;
  // Members are published with these stamps and checked before a deletion.
  conn.executeBatch(QStringLiteral("UPDATE nodes SET revision=(SELECT revision FROM live WHERE live.name=nodes.name) WHERE name IN(SELECT n.name FROM nodes n JOIN live l ON l.name=n.name WHERE n.revision<>l.revision)"));
  if (!queryBool(conn, QStringLiteral("SELECT EXISTS(SELECT 1 FROM nodes WHERE name NOT IN(SELECT name FROM live))"))) {
    conn.execute(QStringLiteral("UPDATE state SET signature=?1"), {signature});
    tx.commit();
    return true;
  }
  conn.executeBatch(QStringLiteral("CREATE TEMP TABLE affected(root TEXT PRIMARY KEY); INSERT INTO affected SELECT DISTINCT root FROM nodes WHERE name NOT IN(SELECT name FROM live);"));
  // With user exclusions, removing an endpoint can change the greedy merge order across components.
  if (queryBool(conn, QStringLiteral("SELECT EXISTS(SELECT 1 FROM nodes n JOIN affected a ON a.root=n.root WHERE EXISTS(SELECT 1 FROM ignored i WHERE i.a=n.name OR i.b=n.name))"))) return false;
  conn.executeBatch(QStringLiteral(
      "DELETE FROM nodes WHERE name NOT IN(SELECT name FROM live);"
      "CREATE TEMP TABLE repair AS SELECT name,hash,root AS old_root FROM nodes WHERE root IN(SELECT root FROM affected);"
      "CREATE INDEX repair_hashes ON repair(old_root,hash,name);"
      "CREATE TEMP TABLE representatives AS SELECT old_root,hash,min(name) AS name FROM repair GROUP BY old_root,hash;"
      "CREATE INDEX representative_names ON representatives(old_root,name);"
      "CREATE TABLE IF NOT EXISTS components(root TEXT PRIMARY KEY,restricted INTEGER NOT NULL);"
      "DELETE FROM components WHERE root IN(SELECT root FROM affected);"
      "UPDATE nodes SET root=(SELECT r.name FROM representatives r JOIN repair p ON p.old_root=r.old_root AND p.hash=r.hash WHERE p.name=nodes.name) WHERE name IN(SELECT name FROM repair);"
      "INSERT INTO components SELECT DISTINCT root,0 FROM nodes WHERE name IN(SELECT name FROM repair);"));
  // Identical hashes are folded in SQL. Only distinct hashes in affected components need distance checks.
  struct Representative {
    QString root, name;
    std::vector<quint64> bits;
  };
  std::vector<Representative> representatives;
  {
    db::Statement stmt = conn.prepare(QStringLiteral("SELECT old_root,name,hash FROM representatives ORDER BY old_root,name"));
    while (stmt.step()) representatives.push_back({stmt.text(0), stmt.text(1), decodeWords(stmt.blob(2).left(stmt.blob(2).size() / 8 * 8))});
  }
  const quint32 threshold = settings.duplicates.effectiveThreshold();
  for (const Representative &rep : representatives) {
    check(id);
    db::Statement others = conn.prepare(QStringLiteral("SELECT name,hash FROM representatives WHERE old_root=?1 AND name<?2 ORDER BY name"));
    others.bind(1, rep.root).bind(2, rep.name);
    std::vector<std::pair<QString, std::vector<quint64>>> matches;
    while (others.step()) {
      check(id);
      const QByteArray hash = others.blob(1);
      matches.emplace_back(others.text(0), decodeWords(hash.left(hash.size() / 8 * 8)));
    }
    for (const auto &[other, bits] : matches)
      if (withinThreshold(rep.bits.data(), bits.data(), std::min(rep.bits.size(), bits.size()), threshold)) merge(conn, rep.name, other);
  }
  conn.executeBatch(QStringLiteral("DELETE FROM groups; INSERT INTO groups SELECT root,count(*),row_number() OVER(ORDER BY root) FROM nodes GROUP BY root HAVING count(*)>1;"));
  conn.execute(QStringLiteral("UPDATE state SET signature=?1"), {signature});
  tx.commit();
  return true;
}

// --- Build -------------------------------------------------------------------------------------------------------

QString build(const QString &dir, const AdvancedSettings &settings, quint64 id) {
  metadata::prepareDatabase(dir);
  const QString service = path::join(dir, metadata::kMetaDir);
  const QString target = path::join(service, QStringLiteral("duplicates.sqlite"));
  const std::vector<Item> items = inventory(dir, id);
  const QString signature = fingerprint(items, dir, settings);
  if (isFile(target)) {
    std::optional<db::Connection> existing;
    try {
      existing.emplace(open(target));
    } catch (const Error &) {
    }
    if (existing) {
      std::optional<QString> current;
      try {
        db::Statement stmt = existing->prepare(QStringLiteral("SELECT signature FROM state"));
        if (stmt.step()) current = stmt.text(0);
      } catch (const Error &) {
      }
      if (current == signature) {
        rememberConfiguration(*existing, settings);
        return target;
      }
      if (pruneDeletions(dir, *existing, settings, signature, items, id)) return target;
    }
  }
  TempFile temporary(service, QStringLiteral(".room237-duplicates-"));
  {
    db::Connection conn = open(temporary.path());
    conn.executeBatch(QStringLiteral(
        "CREATE TABLE nodes(name TEXT PRIMARY KEY,hash BLOB,root TEXT NOT NULL,revision TEXT NOT NULL DEFAULT '',visited INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX roots ON nodes(root,name); CREATE INDEX hashes ON nodes(hash,visited);"
        "CREATE TABLE hash_names(id INTEGER PRIMARY KEY,name TEXT NOT NULL UNIQUE);"
        "CREATE TABLE hash_blocks(block INTEGER PRIMARY KEY,data BLOB NOT NULL);"
        "CREATE TABLE ignored(a TEXT,b TEXT,PRIMARY KEY(a,b)); CREATE INDEX ignored_b ON ignored(b,a);"
        "CREATE TABLE restricted(name TEXT PRIMARY KEY);"
        "CREATE TABLE components(root TEXT PRIMARY KEY,restricted INTEGER NOT NULL);"
        "CREATE TABLE state(signature TEXT NOT NULL,comparisons INTEGER NOT NULL DEFAULT 0,exact_folds INTEGER NOT NULL DEFAULT 0,search_method TEXT NOT NULL DEFAULT 'scan',mih_probes INTEGER NOT NULL DEFAULT 0,mih_fallbacks INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE groups(root TEXT PRIMARY KEY,count INTEGER NOT NULL,ordinal INTEGER UNIQUE NOT NULL);"));
    conn.execute(QStringLiteral("ATTACH DATABASE ?1 AS metadata"), {path::join(service, QStringLiteral("album.sqlite"))});
    conn.execute(QStringLiteral("INSERT INTO ignored SELECT a,b FROM metadata.ignored_pairs"));
    const bool haveIgnores = queryBool(conn, QStringLiteral("SELECT EXISTS(SELECT 1 FROM ignored)"));
    conn.executeBatch(QStringLiteral("INSERT OR IGNORE INTO restricted SELECT a FROM ignored; INSERT OR IGNORE INTO restricted SELECT b FROM ignored;"));
    // Inventory is streamed into disk storage before the deterministic ordered scan.
    {
      db::Transaction tx(conn);
      const QSet<QString> hidden = metadata::hiddenHeicNames(dir);
      db::Statement insert = conn.prepare(QStringLiteral("INSERT INTO nodes(name,root) VALUES(?1,?1)"));
      for (const DirEntry &file : readDir(dir)) {
        check(id);
        if (file.isFile && metadata::isImageMedia(dir, file.name) && !hidden.contains(file.name)) {
          insert.reset();
          insert.bind(1, file.name).execute();
        }
      }
      tx.commit();
    }
    conn.executeBatch(QStringLiteral("INSERT INTO components SELECT name,EXISTS(SELECT 1 FROM restricted r WHERE r.name=nodes.name) FROM nodes;"));
    const HashConfig config = settings.hashConfig();
    const size_t words = config.bits / 64;
    album_db::FileReader reader(path::join(service, QStringLiteral("album.sqlite")));
    QString last;
    size_t count = 0;
    QByteArray packed;
    QThreadPool hashPool;
    hashPool.setMaxThreadCount(int(std::clamp<quint32>(settings.preload.hashWorkers, 1, 8)));
    // First stream the cached/computed hashes into compact disk blocks. IDs follow filename order so exclusions retain
    // the previous merge order.
    for (;;) {
      check(id);
      QStringList names;
      {
        db::Statement stmt = conn.prepare(QStringLiteral("SELECT name FROM nodes WHERE name>?1 ORDER BY name LIMIT 128"));
        stmt.bind(1, last);
        while (stmt.step()) names << stmt.text(0);
      }
      if (names.isEmpty()) break;
      std::vector<std::optional<FileMetaEntry>> entries;
      for (const QString &name : names) {
        check(id);
        entries.push_back(reader.get(name));
      }
      // Missing hashes decode originals or previews; run those concurrently (decoders stay bounded by DecodePermit).
      QList<int> indexes;
      for (int i = 0; i < names.size(); ++i) indexes << i;
      const QList<std::pair<std::optional<Bits>, QString>> computed = QtConcurrent::blockingMapped(&hashPool, indexes, [&](int i) -> std::pair<std::optional<Bits>, QString> {
        if (request.load(std::memory_order_relaxed) != id) return {std::nullopt, QString()};
        const QString file = path::join(dir, names[i]);
        std::optional<Bits> bits;
        try {
          bits = computeHashFromEntry(file, config, settings, entries[size_t(i)]);
        } catch (const std::exception &) {
        }
        // Hashing can create the preview; take the stamp afterwards.
        return {bits, metadata::mediaRevision(file)};
      });
      check(id);
      db::Transaction tx(conn);
      for (int i = 0; i < names.size(); ++i) {
        last = names[i];
        const auto &[bits, revision] = computed[i];
        if (!bits || bits->size() != words) {
          conn.execute(QStringLiteral("UPDATE nodes SET revision=?1 WHERE name=?2"), {revision, names[i]});
          continue;
        }
        const QByteArray hash = bytesOf(bits->data(), bits->size());
        conn.execute(QStringLiteral("UPDATE nodes SET hash=?1,revision=?3 WHERE name=?2"), {hash, names[i], revision});
        conn.execute(QStringLiteral("INSERT INTO hash_names VALUES(?1,?2)"), {qint64(count), names[i]});
        packed += hash;
        ++count;
        if (count % kHashBlockFiles == 0) {
          conn.execute(QStringLiteral("INSERT INTO hash_blocks VALUES(?1,?2)"), {qint64(count / kHashBlockFiles - 1), packed});
          packed.clear();
        }
      }
      tx.commit();
    }
    if (!packed.isEmpty()) conn.execute(QStringLiteral("INSERT INTO hash_blocks VALUES(?1,?2)"), {qint64(count / kHashBlockFiles), packed});
    const bool forceScan = qaRun() && qEnvironmentVariableIsSet("ROOM237_FORCE_DUPLICATE_SCAN");
    auto edgeHash = [&](const char *order) -> std::optional<QByteArray> {
      db::Statement stmt = conn.prepare(QStringLiteral("SELECT hash FROM nodes WHERE hash IS NOT NULL ORDER BY hash %1 LIMIT 1").arg(QLatin1String(order)));
      if (!stmt.step()) return std::nullopt;
      return stmt.blob(0);
    };
    const auto firstHash = edgeHash(""), lastHash = edgeHash("DESC");
    std::optional<mih::Index> index;
    if (!forceScan && firstHash != lastHash)
      if (auto plan = mih::Plan::choose(count, config.bits, config.effectiveThreshold)) index.emplace(mih::Index::build(conn, *plan, count, words, [&] { check(id); }));
    HashBlocks cache(words, index ? kCandidateHashPage : kHashBlockFiles);
    size_t next = 0;
    qint64 comparisons = 0, exactFolds = 0;
    for (;;) {
      check(id);
      std::vector<std::pair<size_t, QString>> names;
      {
        db::Statement stmt = conn.prepare(QStringLiteral("SELECT id,name FROM hash_names WHERE id>=?1 ORDER BY id LIMIT 128"));
        stmt.bind(1, qint64(next));
        while (stmt.step()) names.emplace_back(size_t(stmt.integer(0)), stmt.text(1));
      }
      if (names.empty()) break;
      db::Transaction tx(conn);
      for (const auto &[member, name] : names) {
        check(id);
        next = member + 1;
        const size_t block = member / cache.files;
        const size_t position = (member % cache.files) * words;
        const std::vector<quint64> current(cache.get(conn, block).begin() + std::ptrdiff_t(position), cache.get(conn, block).begin() + std::ptrdiff_t(position + words));
        const QByteArray hash = bytesOf(current.data(), words);
        std::optional<QString> exact;
        {
          db::Statement stmt = haveIgnores ? conn.prepare(QStringLiteral("SELECT n.name FROM nodes n JOIN components c ON c.root=n.root WHERE n.hash=?1 AND n.visited=1 AND c.restricted=0 AND (SELECT restricted FROM components WHERE root=?2)=0 LIMIT 1"))
                                           : conn.prepare(QStringLiteral("SELECT name FROM nodes WHERE hash=?1 AND visited=1 LIMIT 1"));
          stmt.bind(1, hash);
          if (haveIgnores) stmt.bind(2, name);
          if (stmt.step()) exact = stmt.text(0);
        }
        conn.execute(QStringLiteral("UPDATE nodes SET visited=1 WHERE name=?1"), {name});
        if (exact) {
          merge(conn, name, *exact);
          ++exactFolds;
          continue;
        }
        // At a zero threshold the full-hash index is already exact; do not run a quadratic distance scan.
        if (config.effectiveThreshold == 0) {
          QString previous;
          for (;;) {
            check(id);
            QStringList matches;
            {
              db::Statement stmt = conn.prepare(QStringLiteral("SELECT name FROM nodes WHERE hash=?1 AND visited=1 AND name<?2 AND name>?3 ORDER BY name LIMIT 256"));
              stmt.bind(1, hash).bind(2, name).bind(3, previous);
              while (stmt.step()) matches << stmt.text(0);
            }
            if (matches.isEmpty()) break;
            for (const QString &other : matches) {
              previous = other;
              merge(conn, name, other);
            }
          }
          continue;
        }
        if (index) {
          if (auto candidates = index->query(conn, current.data(), member, [&] { check(id); })) {
            if (!candidates->disk) {
              for (size_t at = 0; at < candidates->memory.size(); at += 256) {
                const std::vector<quint64> chunk(candidates->memory.begin() + std::ptrdiff_t(at), candidates->memory.begin() + std::ptrdiff_t(std::min(candidates->memory.size(), at + 256)));
                verifyCandidates(conn, cache, current.data(), name, chunk, config.effectiveThreshold, id, comparisons);
              }
            } else {
              std::optional<quint64> after;
              for (;;) {
                check(id);
                const std::vector<quint64> ids = mih::Candidates::page(conn, after);
                if (ids.empty()) break;
                after = ids.back();
                verifyCandidates(conn, cache, current.data(), name, ids, config.effectiveThreshold, id, comparisons);
              }
            }
            continue;
          }
        }
        for (size_t previousBlock = 0; previousBlock <= block; ++previousBlock) {
          check(id);
          const std::vector<quint64> hashes = cache.get(conn, previousBlock);
          const size_t limit = previousBlock == block ? member % cache.files : hashes.size() / words;
          for (size_t offset = 0; offset < limit && (offset + 1) * words <= hashes.size(); ++offset) {
            if (offset % 256 == 0) check(id);
            ++comparisons;
            if (withinThreshold(current.data(), hashes.data() + offset * words, words, config.effectiveThreshold)) merge(conn, name, nameForId(conn, previousBlock * cache.files + offset));
          }
        }
      }
      tx.commit();
    }
    check(id);
    conn.execute(QStringLiteral("INSERT INTO groups SELECT root,count(*),row_number() OVER(ORDER BY root) FROM nodes GROUP BY root HAVING count(*)>1"));
    // Hashing can create previews; record the final revision after publication.
    const QString finalSignature = fingerprint(inventory(dir, id), dir, settings);
    conn.execute(QStringLiteral("INSERT INTO state(signature,comparisons,exact_folds,search_method,mih_probes,mih_fallbacks) VALUES(?1,?2,?3,?4,?5,?6)"),
                 {finalSignature, comparisons, exactFolds, index ? QStringLiteral("mih") : QStringLiteral("scan"), integer(index ? index->probes : 0), integer(index ? index->fallbacks : 0)});
    rememberConfiguration(conn, settings);
    conn.close();
  }
  temporary.sync();
  temporary.persist(target);
  return target;
}

QString defaultPackedMeta() {
  DetachedFileMeta meta;
  meta.i = true;
  return meta.pack();
}

DetachedMediaEntry duplicateEntry(const QString &dir, const QString &name, const QString &revision, album_db::FileReader &reader) {
  const FileMetaEntry entry = reader.get(name).value_or(FileMetaEntry());
  DetachedMediaEntry out;
  out.blurPreview = entry.blurPreview;
  out.name = name;
  out.revision = revision;
  out.thumbnailName = thumb::detachedThumbnailName(path::join(dir, name));
  out.meta = entry.meta.value_or(defaultPackedMeta());
  out.favorite = entry.favorite;
  out.displayName = entry.vaultName;
  return out;
}

QJsonObject page(const QString &dir, quint64 pageIndex, const AdvancedSettings &settings, quint64 id) {
  auto tree = album_tree::read({dir});
  auto lease = FileLock::acquire(path::join(dir, QStringLiteral(".room237-duplicates.lock")), std::chrono::seconds(30), std::chrono::milliseconds(10));
  db::Connection conn = open(build(dir, settings, id));
  db::Statement count = conn.prepare(QStringLiteral("SELECT count(*) FROM nodes WHERE root<>name"));
  count.step();
  const qint64 total = count.integer(0);
  const quint64 pageNumber = std::min(pageIndex, (total > 0 ? quint64(total) - 1 : 0) / kPage);
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT root,name FROM nodes WHERE root<>name ORDER BY root,name LIMIT ?1 OFFSET ?2"));
  stmt.bind(1, qint64(kPage)).bind(2, qint64(pageNumber * kPage));
  std::vector<QStringList> groups;
  while (stmt.step()) {
    const QString root = stmt.text(0), name = stmt.text(1);
    if (!groups.empty() && groups.back().first() == root) groups.back() << name;
    else groups.push_back({root, name});
  }
  QJsonArray groupsJson, items;
  for (const QStringList &group : groups) {
    groupsJson.append(QJsonArray::fromStringList(group));
    for (const QString &name : group) {
      const QString file = path::join(dir, name);
      const FileMetaEntry entry = metadata::readFileEntry(file).value_or(FileMetaEntry());
      DetachedMediaEntry out;
      out.blurPreview = entry.blurPreview;
      out.revision = metadata::mediaRevision(file);
      out.thumbnailName = thumb::detachedThumbnailName(file);
      out.name = name;
      out.meta = entry.meta.value_or(defaultPackedMeta());
      out.favorite = entry.favorite;
      out.displayName = entry.vaultName;
      items.append(out.toJson());
    }
  }
  return {{QStringLiteral("groups"), groupsJson}, {QStringLiteral("items"), items}, {QStringLiteral("total"), total}, {QStringLiteral("page"), qint64(pageNumber)}, {QStringLiteral("pageSize"), kPage}};
}

db::Connection checkedIndex(const QString &dir, const QString &signature) {
  db::Connection conn = open(path::join(dir, QStringLiteral(".room237-metadata/duplicates.sqlite")));
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT signature FROM state"));
  if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
  if (stmt.text(0) != signature) fail(QStringLiteral("Duplicate index changed; refresh results"));
  return conn;
}

QJsonArray groupMembers(db::Connection &conn, const QString &dir, const QString &root, quint64 offset, qint64 limit) {
  album_db::FileReader reader(path::join(dir, QStringLiteral(".room237-metadata/album.sqlite")));
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT name,revision FROM nodes WHERE root=?1 ORDER BY name LIMIT ?2 OFFSET ?3"));
  stmt.bind(1, root).bind(2, limit).bind(3, integer(offset));
  QJsonArray items;
  while (stmt.step()) items.append(duplicateEntry(dir, stmt.text(0), stmt.text(1), reader).toJson());
  return items;
}

QJsonObject readGroups(db::Connection &conn, const QString &dir, quint64 offset) {
  auto scalar = [&](const char *sql) {
    db::Statement stmt = conn.prepare(QString::fromLatin1(sql));
    if (!stmt.step()) fail(QStringLiteral("Query returned no rows"));
    return stmt;
  };
  db::Statement signatureRow = scalar("SELECT signature FROM state");
  const QString signature = signatureRow.text(0);
  const qint64 total = scalar("SELECT coalesce(max(ordinal),0) FROM groups").integer(0);
  const qint64 filesTotal = scalar("SELECT coalesce(sum(count),0) FROM groups").integer(0);
  const quint64 clamped = std::min(offset, total > 0 ? quint64(total) - 1 : 0);
  db::Statement stmt = conn.prepare(QStringLiteral("SELECT root,count FROM groups WHERE ordinal>?1 ORDER BY ordinal LIMIT 64"));
  stmt.bind(1, integer(clamped));
  std::vector<std::pair<QString, qint64>> rows;
  while (stmt.step()) rows.emplace_back(stmt.text(0), stmt.integer(1));
  QJsonArray groups;
  for (const auto &[root, count] : rows) groups.append(QJsonObject{{QStringLiteral("root"), root}, {QStringLiteral("count"), count}, {QStringLiteral("items"), groupMembers(conn, dir, root, 0, 6)}});
  return {{QStringLiteral("groups"), groups}, {QStringLiteral("total"), total}, {QStringLiteral("filesTotal"), filesTotal}, {QStringLiteral("offset"), qint64(clamped)}, {QStringLiteral("signature"), signature}};
}
}  // namespace

QJsonArray findDuplicates(const QString &dir) {
  std::lock_guard scan(scanMutex);
  return page(dir, 0, readSettings(), request.load()).value(QLatin1String("groups")).toArray();
}

QJsonObject findDuplicatePage(const QString &dir, quint64 pageIndex) {
  const quint64 id = request.fetch_add(1) + 1;
  std::lock_guard scan(scanMutex);
  check(id);
  return page(dir, pageIndex, readSettings(), id);
}

QJsonObject findDuplicateGroups(const QString &dir, quint64 offset, const std::optional<QString> &signature) {
  const quint64 id = signature ? request.load() : request.fetch_add(1) + 1;
  std::lock_guard scan(scanMutex);
  auto tree = album_tree::read({dir});
  auto lease = FileLock::acquire(path::join(dir, QStringLiteral(".room237-duplicates.lock")), std::chrono::seconds(1800), std::chrono::milliseconds(20));
  if (signature) {
    db::Connection conn = checkedIndex(dir, *signature);
    return readGroups(conn, dir, offset);
  }
  db::Connection conn = open(build(dir, readSettings(), id));
  return readGroups(conn, dir, offset);
}

QJsonArray duplicateGroupMembers(const QString &dir, const QString &root, const QString &signature, quint64 offset) {
  auto tree = album_tree::read({dir});
  db::Connection conn = checkedIndex(dir, signature);
  return groupMembers(conn, dir, root, offset, 256);
}

QJsonObject markDuplicateGroupDifferent(const QString &dir, const QString &root, const QString &signature, std::optional<quint64> offset) {
  std::lock_guard scan(scanMutex);
  const quint64 id = request.load();
  auto tree = album_tree::read({dir});
  auto lease = FileLock::acquire(path::join(dir, QStringLiteral(".room237-duplicates.lock")), std::chrono::seconds(1800), std::chrono::milliseconds(20));
  auto guard = metadata::lockMetadataForWrite({dir});
  db::Connection conn = checkedIndex(dir, signature);
  conn.execute(QStringLiteral("ATTACH DATABASE ?1 AS album"), {path::join(dir, QStringLiteral(".room237-metadata/album.sqlite"))});
  const AdvancedSettings settings = readSettings();
  const QByteArray before = sourceFingerprint(inventory(dir, id), settings);
  {
    db::Transaction tx(conn, db::Behavior::Immediate);
    if (finishFingerprint(before, conn, QStringLiteral("album.ignored_pairs")) != signature) fail(QStringLiteral("Album changed; refresh duplicate results before marking this group"));
    db::Statement ordinalRow = conn.prepare(QStringLiteral("SELECT ordinal FROM groups WHERE root=?1"));
    ordinalRow.bind(1, root);
    if (!ordinalRow.step()) fail(QStringLiteral("Query returned no rows"));
    const qint64 ordinal = ordinalRow.integer(0);
    // Persist the user's decision and remove only its connected component. Cross-database rollback journals make both
    // changes commit together.
    conn.execute(QStringLiteral("INSERT OR IGNORE INTO album.ignored_pairs SELECT a.name,b.name FROM nodes a JOIN nodes b ON b.root=a.root AND a.name<b.name WHERE a.root=?1"), {root});
    conn.execute(QStringLiteral("INSERT OR IGNORE INTO ignored SELECT a,b FROM album.ignored_pairs"));
    conn.execute(QStringLiteral("UPDATE nodes SET root=name WHERE root=?1"), {root});
    conn.execute(QStringLiteral("DELETE FROM groups WHERE root=?1"), {root});
    // Avoid UNIQUE conflicts regardless of SQLite's UPDATE traversal order.
    conn.execute(QStringLiteral("UPDATE groups SET ordinal=-ordinal WHERE ordinal>?1"), {ordinal});
    conn.execute(QStringLiteral("UPDATE groups SET ordinal=-ordinal-1 WHERE ordinal<0"));
    const QString next = finishFingerprint(before, conn, QStringLiteral("album.ignored_pairs"));
    conn.execute(QStringLiteral("UPDATE state SET signature=?1"), {next});
    tx.commit();
  }
  return readGroups(conn, dir, offset.value_or(0));
}

quint64 deleteDuplicateGroups(const QString &dir, const QString &signature) {
  std::lock_guard scan(scanMutex);
  auto tree = album_tree::read({dir});
  auto lease = FileLock::acquire(path::join(dir, QStringLiteral(".room237-duplicates.lock")), std::chrono::seconds(1800), std::chrono::milliseconds(20));
  db::Connection conn = checkedIndex(dir, signature);
  QString lastRoot, lastName;
  quint64 deleted = 0;
  for (;;) {
    std::vector<media_delete::DeleteTarget> targets;
    QHash<QString, QString> anchors;
    {
      db::Statement stmt = conn.prepare(QStringLiteral("SELECT n.root,n.name,n.revision,a.revision FROM nodes n JOIN nodes a ON a.name=n.root WHERE n.root<>n.name AND (n.root>?1 OR (n.root=?1 AND n.name>?2)) ORDER BY n.root,n.name LIMIT 128"));
      stmt.bind(1, lastRoot).bind(2, lastName);
      while (stmt.step()) {
        lastRoot = stmt.text(0);
        lastName = stmt.text(1);
        targets.push_back({lastName, stmt.text(2)});
        anchors.insert(lastRoot, stmt.text(3));
      }
    }
    if (targets.empty()) break;
    std::vector<metadata::ObservedMedia> observed;
    for (auto it = anchors.cbegin(); it != anchors.cend(); ++it) observed.push_back({path::join(dir, it.key()), it.value()});
    const media_delete::DeleteBatchResult result = media_delete::deleteMediaBatch(dir, targets, observed);
    deleted += quint64(result.deleted.size());
    if (result.error) fail(QStringLiteral("Deleted %1 files; %2").arg(deleted).arg(*result.error));
  }
  return deleted;
}

}  // namespace core::duplicates
