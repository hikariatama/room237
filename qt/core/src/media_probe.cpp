#include "media_probe.h"

#include "fs.h"

#include <QDateTime>
#include <QFile>
#include <QTimeZone>
#include <functional>
#include <map>

namespace core::probe {

namespace {

bool readExact(QIODevice &in, char *buffer, qint64 size) {
  qint64 done = 0;
  while (done < size) {
    const qint64 got = in.read(buffer + done, size - done);
    if (got <= 0) return false;
    done += got;
  }
  return true;
}
quint32 be32(const unsigned char *p) { return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3]; }
quint64 be64(const unsigned char *p) { return (quint64(be32(p)) << 32) | be32(p + 4); }
quint16 be16(const unsigned char *p) { return quint16((p[0] << 8) | p[1]); }

constexpr int kMaxAtoms = 1024;

std::optional<std::pair<quint64, quint64>> findAtom(QIODevice &in, quint64 start, quint64 end, const char *wanted, int &budget) {
  while (start < end) {
    if (--budget < 0) return std::nullopt;
    if (end - start < 8) return std::nullopt;
    if (!in.seek(qint64(start))) return std::nullopt;
    unsigned char header[8];
    if (!readExact(in, reinterpret_cast<char *>(header), 8)) return std::nullopt;
    quint64 headerSize = 8, size = be32(header);
    if (size == 0) size = end - start;
    else if (size == 1) {
      if (end - start < 16) return std::nullopt;
      unsigned char extended[8];
      if (!readExact(in, reinterpret_cast<char *>(extended), 8)) return std::nullopt;
      headerSize = 16;
      size = be64(extended);
    }
    if (size < headerSize) return std::nullopt;
    if (start > ~quint64(0) - size) return std::nullopt;
    const quint64 next = start + size;
    if (next > end) return std::nullopt;
    if (memcmp(header + 4, wanted, 4) == 0) return std::make_pair(start + headerSize, next);
    start = next;
  }
  return std::nullopt;
}

// --- TIFF / EXIF -------------------------------------------------------------------------------------------------

struct Tiff {
  QByteArray data;
  bool little = false;
  quint16 u16(qint64 at) const {
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData()) + at;
    return little ? quint16(p[0] | (p[1] << 8)) : quint16((p[0] << 8) | p[1]);
  }
  quint32 u32(qint64 at) const {
    const auto *p = reinterpret_cast<const unsigned char *>(data.constData()) + at;
    return little ? (quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24)) : be32(p);
  }
  bool has(qint64 at, qint64 size) const { return at >= 0 && size >= 0 && at <= data.size() && size <= data.size() - at; }
};

struct Field {
  quint16 tag = 0, type = 0;
  quint32 count = 0;
  qint64 offset = 0;  // Of the value bytes inside the TIFF data.
  bool primary = false;
};

int typeSize(quint16 type) {
  switch (type) {
  case 1: case 2: case 6: case 7: return 1;
  case 3: case 8: return 2;
  case 4: case 9: case 11: return 4;
  case 5: case 10: case 12: return 8;
  default: return 0;
  }
}

bool parseIfd(const Tiff &tiff, quint32 offset, bool primary, std::vector<Field> &fields, int depth, quint32 *next) {
  if (depth > 4 || !tiff.has(offset, 2)) return false;
  const quint16 count = tiff.u16(offset);
  if (!tiff.has(offset + 2, qint64(count) * 12 + 4)) return false;
  for (quint16 i = 0; i < count; ++i) {
    const qint64 at = offset + 2 + qint64(i) * 12;
    Field field;
    field.tag = tiff.u16(at);
    field.type = tiff.u16(at + 2);
    field.count = tiff.u32(at + 4);
    field.primary = primary;
    const int unit = typeSize(field.type);
    const qint64 bytes = qint64(unit) * field.count;
    field.offset = unit && bytes <= 4 ? at + 8 : qint64(tiff.u32(at + 8));
    if (unit && !tiff.has(field.offset, bytes)) continue;
    if ((field.tag == 0x8769 || field.tag == 0x8825 || field.tag == 0xa005) && (field.type == 4 || field.type == 13) && field.count == 1) {
      parseIfd(tiff, tiff.u32(at + 8), primary, fields, depth + 1, nullptr);
      continue;
    }
    fields.push_back(field);
  }
  if (next) *next = tiff.u32(offset + 2 + qint64(count) * 12);
  return true;
}

std::optional<Exif> parseTiff(const QByteArray &data) {
  Tiff tiff{data};
  if (data.size() < 8) return std::nullopt;
  if (data.startsWith("II")) tiff.little = true;
  else if (!data.startsWith("MM")) return std::nullopt;
  if (tiff.u16(2) != 42) return std::nullopt;
  std::vector<Field> fields;
  quint32 next = 0;
  if (!parseIfd(tiff, tiff.u32(4), true, fields, 0, &next)) return std::nullopt;
  if (next) parseIfd(tiff, next, false, fields, 0, nullptr);
  auto ascii = [&](const Field &field) -> std::optional<QString> {
    if (field.type != 2) return std::nullopt;
    QByteArray bytes = data.mid(field.offset, field.count);
    const int nul = bytes.indexOf('\0');
    if (field.count == 0) return std::nullopt;
    if (nul >= 0) bytes.truncate(nul);
    QString text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes) return std::nullopt;
    while (text.endsWith(QChar(0))) text.chop(1);
    return text.trimmed();
  };
  auto find = [&](quint16 tag) -> std::optional<QString> {
    for (const Field &field : fields)
      if (field.tag == tag && field.primary) return ascii(field);
    for (const Field &field : fields)
      if (field.tag == tag) return ascii(field);
    return std::nullopt;
  };
  Exif exif;
  exif.dateTimeOriginal = find(0x9003);
  exif.dateTime = find(0x0132);
  for (const Field &field : fields) {
    if (field.tag != 0x0112 || !field.primary || field.count == 0) continue;
    if (field.type == 3) exif.orientation = tiff.u16(field.offset);
    else if (field.type == 4) exif.orientation = tiff.u32(field.offset);
    else if (field.type == 1) exif.orientation = quint8(data[field.offset]);
    break;
  }
  return exif;
}

// --- Containers --------------------------------------------------------------------------------------------------

std::optional<QByteArray> jpegExif(QIODevice &in) {
  in.seek(2);
  for (int segments = 0; segments < 4096; ++segments) {
    unsigned char marker[2];
    if (!readExact(in, reinterpret_cast<char *>(marker), 2)) return std::nullopt;
    if (marker[0] != 0xff) return std::nullopt;
    while (marker[1] == 0xff)
      if (!readExact(in, reinterpret_cast<char *>(marker) + 1, 1)) return std::nullopt;
    const unsigned char code = marker[1];
    if (code == 0xd8 || (code >= 0xd0 && code <= 0xd7) || code == 0x01) continue;
    if (code == 0xd9 || code == 0xda) return std::nullopt;
    unsigned char length[2];
    if (!readExact(in, reinterpret_cast<char *>(length), 2)) return std::nullopt;
    const int size = be16(length);
    if (size < 2) return std::nullopt;
    if (code == 0xe1) {
      QByteArray body = in.read(size - 2);
      if (body.size() != size - 2) return std::nullopt;
      if (body.startsWith(QByteArray("Exif\0\0", 6))) return body.mid(6);
    } else if (!in.seek(in.pos() + size - 2)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<QByteArray> pngExif(QIODevice &in) {
  in.seek(8);
  for (int chunks = 0; chunks < 100000; ++chunks) {
    unsigned char header[8];
    if (!readExact(in, reinterpret_cast<char *>(header), 8)) return std::nullopt;
    const quint32 length = be32(header);
    if (memcmp(header + 4, "eXIf", 4) == 0) {
      QByteArray body = in.read(length);
      return body.size() == qint64(length) ? std::optional(body) : std::nullopt;
    }
    if (memcmp(header + 4, "IEND", 4) == 0) return std::nullopt;
    if (!in.seek(in.pos() + qint64(length) + 4)) return std::nullopt;
  }
  return std::nullopt;
}

std::optional<QByteArray> webpExif(QIODevice &in) {
  in.seek(12);
  for (int chunks = 0; chunks < 100000; ++chunks) {
    unsigned char header[8];
    if (!readExact(in, reinterpret_cast<char *>(header), 8)) return std::nullopt;
    const quint32 length = quint32(header[4]) | (quint32(header[5]) << 8) | (quint32(header[6]) << 16) | (quint32(header[7]) << 24);
    if (memcmp(header, "EXIF", 4) == 0) {
      QByteArray body = in.read(length);
      if (body.size() != qint64(length)) return std::nullopt;
      if (body.startsWith(QByteArray("Exif\0\0", 6))) body.remove(0, 6);
      return body;
    }
    if (!in.seek(in.pos() + qint64(length) + (length & 1))) return std::nullopt;
  }
  return std::nullopt;
}

struct Box {
  quint64 start = 0, end = 0;  // Payload range.
  char type[4] = {};
};
std::vector<Box> children(QIODevice &in, quint64 start, quint64 end, int &budget) {
  std::vector<Box> boxes;
  while (start + 8 <= end && --budget > 0) {
    if (!in.seek(qint64(start))) break;
    unsigned char header[16];
    if (!readExact(in, reinterpret_cast<char *>(header), 8)) break;
    quint64 size = be32(header), headerSize = 8;
    if (size == 1) {
      if (!readExact(in, reinterpret_cast<char *>(header) + 8, 8)) break;
      size = be64(header + 8);
      headerSize = 16;
    } else if (size == 0) {
      size = end - start;
    }
    if (size < headerSize || size > end - start) break;
    Box box;
    box.start = start + headerSize;
    box.end = start + size;
    memcpy(box.type, header + 4, 4);
    boxes.push_back(box);
    start += size;
  }
  return boxes;
}

std::optional<QByteArray> heifExif(QIODevice &in, quint64 length) {
  int budget = 4096;
  std::optional<Box> meta;
  for (const Box &box : children(in, 0, length, budget))
    if (!memcmp(box.type, "meta", 4)) meta = box;
  if (!meta || meta->end - meta->start < 4) return std::nullopt;
  std::optional<quint32> exifItem;
  struct Extent {
    quint64 offset, length;
  };
  std::map<quint32, std::vector<Extent>> locations;
  std::map<quint32, quint64> baseOffsets;
  std::map<quint32, int> constructions;
  for (const Box &box : children(in, meta->start + 4, meta->end, budget)) {
    if (!in.seek(qint64(box.start))) continue;
    const QByteArray body = in.read(qint64(std::min<quint64>(box.end - box.start, 1 << 20)));
    const auto *p = reinterpret_cast<const unsigned char *>(body.constData());
    const qint64 n = body.size();
    if (!memcmp(box.type, "iinf", 4) && n >= 6) {
      const int version = p[0];
      qint64 at = 4;
      const quint32 count = version == 0 ? be16(p + at) : be32(p + at);
      at += version == 0 ? 2 : 4;
      for (quint32 i = 0; i < count && at + 8 <= n; ++i) {
        const quint64 size = be32(p + at);
        if (size < 8 || at + qint64(size) > n) break;
        if (!memcmp(p + at + 4, "infe", 4) && size >= 12) {
          const int infeVersion = p[at + 8];
          qint64 q = at + 12;
          quint32 id = 0;
          if (infeVersion >= 2) {
            if (infeVersion == 2 && q + 8 <= at + qint64(size)) {
              id = be16(p + q);
              q += 4;
            } else if (infeVersion == 3 && q + 10 <= at + qint64(size)) {
              id = be32(p + q);
              q += 6;
            } else {
              q = -1;
            }
            if (q > 0 && q + 4 <= at + qint64(size) && !memcmp(p + q, "Exif", 4)) exifItem = id;
          }
        }
        at += qint64(size);
      }
    } else if (!memcmp(box.type, "iloc", 4) && n >= 8) {
      const int version = p[0];
      const int offsetSize = p[4] >> 4, lengthSize = p[4] & 15, baseSize = p[5] >> 4, indexSize = version >= 1 ? (p[5] & 15) : 0;
      qint64 at = 6;
      auto read = [&](int size, quint64 &out) {
        if (size == 0) {
          out = 0;
          return true;
        }
        if (at + size > n || (size != 4 && size != 8)) return false;
        out = size == 4 ? be32(p + at) : be64(p + at);
        at += size;
        return true;
      };
      quint32 count = 0;
      if (version < 2) {
        if (at + 2 > n) continue;
        count = be16(p + at);
        at += 2;
      } else {
        if (at + 4 > n) continue;
        count = be32(p + at);
        at += 4;
      }
      for (quint32 i = 0; i < count; ++i) {
        quint32 id = 0;
        if (version < 2) {
          if (at + 2 > n) break;
          id = be16(p + at);
          at += 2;
        } else {
          if (at + 4 > n) break;
          id = be32(p + at);
          at += 4;
        }
        int construction = 0;
        if (version >= 1) {
          if (at + 2 > n) break;
          construction = be16(p + at) & 15;
          at += 2;
        }
        if (at + 2 > n) break;
        at += 2;  // data_reference_index
        quint64 base = 0;
        if (!read(baseSize, base)) break;
        if (at + 2 > n) break;
        const int extents = be16(p + at);
        at += 2;
        bool ok = true;
        for (int e = 0; e < extents; ++e) {
          quint64 index = 0, offset = 0, len = 0;
          if (indexSize && !read(indexSize, index)) ok = false;
          if (!ok || !read(offsetSize, offset) || !read(lengthSize, len)) {
            ok = false;
            break;
          }
          locations[id].push_back({offset, len});
        }
        if (!ok) break;
        baseOffsets[id] = base;
        constructions[id] = construction;
      }
    }
  }
  if (!exifItem || !locations.count(*exifItem) || constructions[*exifItem] != 0) return std::nullopt;
  QByteArray item;
  for (const Extent &extent : locations[*exifItem]) {
    const quint64 offset = baseOffsets[*exifItem] + extent.offset;
    const quint64 size = extent.length ? extent.length : (length > offset ? length - offset : 0);
    if (size > (64u << 20) || !in.seek(qint64(offset))) return std::nullopt;
    const QByteArray part = in.read(qint64(size));
    if (part.size() != qint64(size)) return std::nullopt;
    item += part;
  }
  if (item.size() < 4) return std::nullopt;
  const quint32 skip = be32(reinterpret_cast<const unsigned char *>(item.constData()));
  if (qint64(skip) > item.size() - 4) return std::nullopt;
  return item.mid(4 + skip);
}

}  // namespace

std::optional<qint64> quickTimeCreationTime(QIODevice &in, quint64 length) {
  int budget = kMaxAtoms;
  auto moov = findAtom(in, 0, length, "moov", budget);
  if (!moov) return std::nullopt;
  auto mvhd = findAtom(in, moov->first, moov->second, "mvhd", budget);
  if (!mvhd) return std::nullopt;
  const auto [start, end] = *mvhd;
  if (end - start < 4 || !in.seek(qint64(start))) return std::nullopt;
  unsigned char flags[4];
  if (!readExact(in, reinterpret_cast<char *>(flags), 4)) return std::nullopt;
  quint64 time = 0;
  if (flags[0] == 0 && end - start >= 8) {
    unsigned char bytes[4];
    if (!readExact(in, reinterpret_cast<char *>(bytes), 4)) return std::nullopt;
    time = be32(bytes);
  } else if (flags[0] == 1 && end - start >= 12) {
    unsigned char bytes[8];
    if (!readExact(in, reinterpret_cast<char *>(bytes), 8)) return std::nullopt;
    time = be64(bytes);
  } else {
    return std::nullopt;
  }
  // Zero denotes an unset timestamp in the media handled by FFmpeg.
  if (time == 0 || time > quint64(std::numeric_limits<qint64>::max())) return std::nullopt;
  return qint64(time) - kQuickTimeEpochOffset;
}

std::optional<qint64> quickTimeCreationTime(const QString &p) {
  if (!stat(p).isFile) return std::nullopt;
  QFile file(p);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) return std::nullopt;
  return quickTimeCreationTime(file, quint64(file.size()));
}

std::optional<Exif> readExif(QIODevice &in) {
  if (!in.seek(0)) return std::nullopt;
  const QByteArray head = in.read(16);
  if (head.size() < 4) return std::nullopt;
  std::optional<QByteArray> tiff;
  if (head.startsWith("II*\0") || head.startsWith(QByteArray("MM\0*", 4))) {
    in.seek(0);
    tiff = in.read(std::min<qint64>(in.size(), 64 << 20));
  } else if (uchar(head[0]) == 0xff && uchar(head[1]) == 0xd8) {
    tiff = jpegExif(in);
  } else if (head.startsWith("\x89PNG\r\n\x1a\n")) {
    tiff = pngExif(in);
  } else if (head.startsWith("RIFF") && head.mid(8, 4) == "WEBP") {
    tiff = webpExif(in);
  } else if (head.size() >= 12 && head.mid(4, 4) == "ftyp") {
    tiff = heifExif(in, quint64(in.size()));
  }
  if (!tiff) return std::nullopt;
  return parseTiff(*tiff);
}

std::optional<Exif> readExif(const QString &p) {
  QFile file(p);
  if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
  return readExif(file);
}

std::optional<qint64> datetimeOriginal(const QString &p) {
  const auto exif = readExif(p);
  if (!exif) return std::nullopt;
  const std::optional<QString> text = exif->dateTimeOriginal ? exif->dateTimeOriginal : exif->dateTime;
  if (!text) return std::nullopt;
  const QDateTime parsed = QDateTime::fromString(*text, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
  if (!parsed.isValid()) return std::nullopt;
  return QDateTime(parsed.date(), parsed.time(), QTimeZone::UTC).toSecsSinceEpoch();
}

bool exifTurnsPicture(const QString &p) {
  if (!path::hasExtension(p, {"jpg", "jpeg", "tif", "tiff"})) return false;
  const auto exif = readExif(p);
  return exif && exif->orientation && *exif->orientation >= 5;
}

}  // namespace core::probe
