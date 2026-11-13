#pragma once
#include <QString>

namespace core::constants {

// Supported media formats.
inline constexpr const char *kImageExtensions[] = {"jpg", "jpeg", "png", "webp", "avif", "gif", "bmp", "heic", "heif", "tif", "tiff"};
inline constexpr const char *kVideoExtensions[] = {"mp4", "mov", "mkv", "webm", "avi", "flv", "m4v", "ogg", "ogv", "wmv", "mpg", "mpeg", "3gp", "mts", "m2ts"};

inline bool isImageExtension(const QString &lowerExtension) {
  for (const char *ext : kImageExtensions)
    if (lowerExtension == QLatin1String(ext)) return true;
  return false;
}
inline bool isVideoExtension(const QString &lowerExtension) {
  for (const char *ext : kVideoExtensions)
    if (lowerExtension == QLatin1String(ext)) return true;
  return false;
}
// The extension as Rust's Path::extension gives it, lowercased; empty for dotfiles.
QString lowerExtension(const QString &path);
bool isMedia(const QString &path);  // Includes the random names of encrypted files.
bool isEncryptedName(const QString &path);
bool isImage(const QString &path);
bool isVideo(const QString &path);

}  // namespace core::constants
