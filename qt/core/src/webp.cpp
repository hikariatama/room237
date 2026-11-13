#include "native_image.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <webp/decode.h>
#include <webp/encode.h>

namespace core::native_image {

std::optional<QByteArray> encodeWebp(const QByteArray &pixels, int width, int height, bool alpha, float quality, int method) {
  WebPConfig config;
  if (!WebPConfigInitInternal(&config, WEBP_PRESET_DEFAULT, 75.0f, WEBP_ENCODER_ABI_VERSION)) return std::nullopt;
  config.quality = quality;
  config.method = method;
  WebPPicture picture;
  if (!WebPPictureInitInternal(&picture, WEBP_ENCODER_ABI_VERSION)) return std::nullopt;
  picture.width = width;
  picture.height = height;
  picture.use_argb = 1;
  const auto *data = reinterpret_cast<const uint8_t *>(pixels.constData());
  const int imported = alpha ? WebPPictureImportRGBA(&picture, data, width * 4) : WebPPictureImportRGB(&picture, data, width * 3);
  if (!imported) return std::nullopt;
  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  picture.writer = WebPMemoryWrite;
  picture.custom_ptr = &writer;
  const int ok = WebPEncode(&config, &picture);
  WebPPictureFree(&picture);
  std::optional<QByteArray> out;
  if (ok) out = QByteArray(reinterpret_cast<const char *>(writer.mem), qsizetype(writer.size));
  WebPMemoryWriterClear(&writer);
  return out;
}

bool decodeWebp(const QByteArray &bytes, QByteArray &rgba, int &width, int &height) {
  int w = 0, h = 0;
  if (!WebPGetInfo(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()), &w, &h)) return false;
  rgba.resize(qsizetype(w) * h * 4);
  if (!WebPDecodeRGBAInto(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()), reinterpret_cast<uint8_t *>(rgba.data()), size_t(rgba.size()), w * 4)) return false;
  width = w;
  height = h;
  return true;
}

#ifndef Q_OS_MACOS
std::optional<std::pair<quint32, quint32>> dimensionsFromData(const QByteArray &bytes) {
  QByteArray copy = bytes;
  QBuffer buffer(&copy);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  reader.setAutoTransform(true);
  QSize size = reader.size();
  if (!size.isValid()) return std::nullopt;
  if (reader.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
  return std::make_pair(quint32(size.width()), quint32(size.height()));
}
static QByteArray rgb(const QImage &image, bool alpha) {
  const QImage converted = image.convertToFormat(alpha ? QImage::Format_RGBA8888 : QImage::Format_RGB888);
  QByteArray out;
  const int row = converted.width() * (alpha ? 4 : 3);
  for (int y = 0; y < converted.height(); ++y) out.append(reinterpret_cast<const char *>(converted.constScanLine(y)), row);
  return out;
}
std::optional<Thumbnail> thumbnailFromData(const QByteArray &bytes, quint32 maxDim, float quality, int method) {
  QByteArray copy = bytes;
  QBuffer buffer(&copy);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  reader.setAutoTransform(true);
  QImage image = reader.read();
  if (image.isNull()) return std::nullopt;
  if (quint32(image.width()) > maxDim || quint32(image.height()) > maxDim) image = image.scaled(int(maxDim), int(maxDim), Qt::KeepAspectRatio, Qt::SmoothTransformation);
  const bool alpha = image.hasAlphaChannel();
  auto main = encodeWebp(rgb(image, alpha), image.width(), image.height(), alpha, quality, method);
  auto small = encodeWebp(rgb(image.scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), false), 16, 16, false, 20.0f, 0);
  if (!main || !small) return std::nullopt;
  return Thumbnail{*main, *small};
}
std::optional<QByteArray> blurFromImage(const QByteArray &bytes) {
  QImage image;
  if (!image.loadFromData(bytes)) return std::nullopt;
  return encodeWebp(rgb(image.scaled(16, 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), false), 16, 16, false, 20.0f, 0);
}
bool metadataEnabled() { return false; }
bool thumbnailsEnabled() { return false; }
bool hashDecodeEnabled() { return false; }
std::optional<std::pair<quint32, quint32>> dimensions(const QString &) { return std::nullopt; }
std::optional<Thumbnail> thumbnail(const QString &, quint32, float, int) { return std::nullopt; }
#endif

}  // namespace core::native_image
