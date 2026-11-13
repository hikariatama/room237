#include "native_image.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <QFile>
#include <vector>

namespace core::native_image {

namespace {
// Each switch defaults to on; ROOM237_NATIVE_<X>=0 selects the FFmpeg path.
bool enabled(const char *name) { return qEnvironmentVariable(name) != QLatin1String("0"); }

struct Owned {
  CFTypeRef ref = nullptr;
  explicit Owned(CFTypeRef value) : ref(value) {}
  ~Owned() {
    if (ref) CFRelease(ref);
  }
  Owned(const Owned &) = delete;
  Owned &operator=(const Owned &) = delete;
};

CGImageSourceRef openSource(const QString &path) {
  const QByteArray bytes = QFile::encodeName(path);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()), CFIndex(bytes.size()), false);
  if (!url) return nullptr;
  CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  return source;
}

std::optional<QByteArray> drawRgb(CGImageRef image, size_t w, size_t h, CGInterpolationQuality quality) {
  std::vector<uint8_t> rgba(w * h * 4);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (!context) return std::nullopt;
  const CGRect rect = CGRectMake(0, 0, CGFloat(w), CGFloat(h));
  CGContextSetRGBFillColor(context, 0, 0, 0, 1);
  CGContextFillRect(context, rect);
  CGContextSetInterpolationQuality(context, quality);
  CGContextDrawImage(context, rect, image);
  CGContextRelease(context);
  QByteArray rgb;
  rgb.reserve(qsizetype(w * h * 3));
  for (size_t i = 0; i < w * h; ++i) rgb.append(reinterpret_cast<const char *>(&rgba[i * 4]), 3);
  return rgb;
}

// Straight-alpha RGBA when the image has any transparency, like the FFmpeg path keeps it.
std::optional<QByteArray> drawRgba(CGImageRef image, size_t w, size_t h) {
  std::vector<uint8_t> rgba(w * h * 4);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
  CGColorSpaceRelease(space);
  if (!context) return std::nullopt;
  CGContextDrawImage(context, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), image);
  CGContextRelease(context);
  bool opaque = true;
  for (size_t i = 0; i < w * h && opaque; ++i) opaque = rgba[i * 4 + 3] == 255;
  if (opaque) return std::nullopt;
  for (size_t i = 0; i < w * h; ++i) {
    const uint32_t alpha = rgba[i * 4 + 3];
    if (alpha > 0 && alpha < 255)
      for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = uint8_t(std::min<uint32_t>((uint32_t(rgba[i * 4 + c]) * 255 + alpha / 2) / alpha, 255));
  }
  return QByteArray(reinterpret_cast<const char *>(rgba.data()), qsizetype(rgba.size()));
}
}  // namespace

bool metadataEnabled() {
  static const bool on = enabled("ROOM237_NATIVE_META");
  return on;
}
bool thumbnailsEnabled() {
  static const bool on = enabled("ROOM237_NATIVE_THUMB");
  return on;
}
bool hashDecodeEnabled() {
  static const bool on = enabled("ROOM237_NATIVE_HASH");
  return on;
}

static std::optional<std::pair<quint32, quint32>> dimensionsOf(CGImageSourceRef source);
static std::optional<Thumbnail> thumbnailOf(CGImageSourceRef source, quint32 maxDim, float quality, int method);
static CGImageSourceRef sourceFromData(const QByteArray &bytes) {
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()), CFIndex(bytes.size()));
  if (!data) return nullptr;
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  return source;
}

std::optional<std::pair<quint32, quint32>> dimensions(const QString &path) {
  CGImageSourceRef source = openSource(path);
  if (!source) return std::nullopt;
  Owned sourceOwner(source);
  return dimensionsOf(source);
}
std::optional<std::pair<quint32, quint32>> dimensionsFromData(const QByteArray &bytes) {
  CGImageSourceRef source = sourceFromData(bytes);
  if (!source) return std::nullopt;
  Owned sourceOwner(source);
  return dimensionsOf(source);
}
std::optional<Thumbnail> thumbnail(const QString &path, quint32 maxDim, float quality, int method) {
  CGImageSourceRef source = openSource(path);
  if (!source) return std::nullopt;
  Owned sourceOwner(source);
  return thumbnailOf(source, maxDim, quality, method);
}
std::optional<Thumbnail> thumbnailFromData(const QByteArray &bytes, quint32 maxDim, float quality, int method) {
  CGImageSourceRef source = sourceFromData(bytes);
  if (!source) return std::nullopt;
  Owned sourceOwner(source);
  return thumbnailOf(source, maxDim, quality, method);
}
std::optional<QByteArray> blurFromImage(const QByteArray &bytes) {
  CGImageSourceRef source = sourceFromData(bytes);
  if (!source) return std::nullopt;
  Owned sourceOwner(source);
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  if (!image) return std::nullopt;
  const auto blur = drawRgb(image, 16, 16, kCGInterpolationHigh);
  CGImageRelease(image);
  if (!blur) return std::nullopt;
  return encodeWebp(*blur, 16, 16, false, 20.0f, 0);
}

static std::optional<std::pair<quint32, quint32>> dimensionsOf(CGImageSourceRef source) {
  const void *keys[] = {kCGImageSourceShouldCache};
  const void *values[] = {kCFBooleanFalse};
  Owned options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(source, 0, CFDictionaryRef(options.ref));
  if (!props) return std::nullopt;
  Owned propsOwner(props);
  auto read = [&](CFStringRef key) -> std::optional<quint32> {
    auto number = CFNumberRef(CFDictionaryGetValue(props, key));
    if (!number) return std::nullopt;
    int64_t out = 0;
    if (!CFNumberGetValue(number, kCFNumberSInt64Type, &out) || out <= 0) return std::nullopt;
    return quint32(out);
  };
  const auto width = read(kCGImagePropertyPixelWidth), height = read(kCGImagePropertyPixelHeight);
  if (!width || !height) return std::nullopt;
  // Orientations 5-8 turn the picture by 90 degrees; everything downstream works in display dimensions.
  if (read(kCGImagePropertyOrientation).value_or(1) >= 5) return std::make_pair(*height, *width);
  return std::make_pair(*width, *height);
}

static std::optional<Thumbnail> thumbnailOf(CGImageSourceRef source, quint32 maxDim, float quality, int method) {
  const int64_t max = maxDim;
  Owned number(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &max));
  const void *keys[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform, kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceShouldCache};
  const void *values[] = {kCFBooleanTrue, kCFBooleanTrue, number.ref, kCFBooleanFalse};
  Owned options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
  CGImageRef image = CGImageSourceCreateThumbnailAtIndex(source, 0, CFDictionaryRef(options.ref));
  if (!image) return std::nullopt;
  const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
  std::optional<QByteArray> preview;
  bool alpha = false;
  if (w > 0 && h > 0) {
    preview = drawRgba(image, w, h);
    alpha = preview.has_value();
    if (!preview) preview = drawRgb(image, w, h, kCGInterpolationDefault);
  }
  const auto blur = drawRgb(image, 16, 16, kCGInterpolationHigh);
  CGImageRelease(image);
  if (!preview || !blur) return std::nullopt;
  auto main = encodeWebp(*preview, int(w), int(h), alpha, quality, method);
  auto small = encodeWebp(*blur, 16, 16, false, 20.0f, 0);
  if (!main || !small) return std::nullopt;
  return Thumbnail{*main, *small};
}

}  // namespace core::native_image
