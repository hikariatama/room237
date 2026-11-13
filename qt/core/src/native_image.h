#pragma once
// In-process image work on macOS (ImageIO + libwebp). Every entry point returns nullopt when it cannot produce the
// same artifact, and the caller then uses the FFmpeg path unchanged.
#include <QByteArray>
#include <QString>
#include <optional>

namespace core::native_image {

bool metadataEnabled();
bool thumbnailsEnabled();
bool hashDecodeEnabled();
std::optional<std::pair<quint32, quint32>> dimensions(const QString &path);  // Display dimensions.
struct Thumbnail {
  QByteArray main, blur;
};
std::optional<Thumbnail> thumbnail(const QString &path, quint32 maxDim, float quality, int method);
// The same from bytes in memory, for encrypted originals. Falls back to Qt decoders off macOS.
std::optional<std::pair<quint32, quint32>> dimensionsFromData(const QByteArray &bytes);
std::optional<Thumbnail> thumbnailFromData(const QByteArray &bytes, quint32 maxDim, float quality, int method);
// A 16x16 preview of an image, as the gallery shows before the thumbnail arrives.
std::optional<QByteArray> blurFromImage(const QByteArray &bytes);

// libwebp, all platforms.
std::optional<QByteArray> encodeWebp(const QByteArray &pixels, int width, int height, bool alpha, float quality, int method);
// RGBA8 pixels of a WebP image; used for perceptual hashes so every platform decodes previews identically.
bool decodeWebp(const QByteArray &bytes, QByteArray &rgba, int &width, int &height);

}  // namespace core::native_image
