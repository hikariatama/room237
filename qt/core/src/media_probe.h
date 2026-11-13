#pragma once
// Container parsing that needs no decoder: QuickTime movie dates and EXIF dates and orientation.
#include <QIODevice>
#include <QString>
#include <optional>

namespace core::probe {

inline constexpr qint64 kQuickTimeEpochOffset = 2082844800;
std::optional<qint64> quickTimeCreationTime(const QString &path);
std::optional<qint64> quickTimeCreationTime(QIODevice &input, quint64 length);

struct Exif {
  std::optional<QString> dateTimeOriginal, dateTime;
  std::optional<quint32> orientation;
};
// JPEG, TIFF, PNG, WebP and HEIF containers. Returns nullopt when no EXIF block is found.
std::optional<Exif> readExif(const QString &path);
std::optional<Exif> readExif(QIODevice &input);
std::optional<qint64> datetimeOriginal(const QString &path);
bool exifTurnsPicture(const QString &path);

}  // namespace core::probe
