#include "image_hash.h"

#include "fs.h"
#include "util.h"
#include "vault.h"

#include <QImage>
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>
#include <webp/decode.h>

namespace core::image_hash {

namespace {
constexpr float kPi = 3.14159265358979323846f;

// --- image 0.23 resampling, on single-channel u8 buffers ---------------------------------------------------------

struct Filter {
  std::function<float(float)> kernel;
  float support;
};
float sinc(float t) {
  const float a = t * kPi;
  return t == 0.0f ? 1.0f : std::sin(a) / a;
}
float lanczos(float x, float t) { return std::fabs(x) < t ? sinc(x) * sinc(x / t) : 0.0f; }
float cubic(float x, float b, float c) {
  const float a = std::fabs(x);
  float k;
  if (a < 1.0f) k = (12.0f - 9.0f * b - 6.0f * c) * (a * a * a) + (-18.0f + 12.0f * b + 6.0f * c) * (a * a) + (6.0f - 2.0f * b);
  else if (a < 2.0f) k = (-b - 6.0f * c) * (a * a * a) + (6.0f * b + 30.0f * c) * (a * a) + (-12.0f * b - 48.0f * c) * a + (8.0f * b + 24.0f * c);
  else k = 0.0f;
  return k / 6.0f;
}
Filter filterFor(ResizeFilter filter) {
  switch (filter) {
  case ResizeFilter::Triangle: return {[](float x) { return std::fabs(x) < 1.0f ? 1.0f - std::fabs(x) : 0.0f; }, 1.0f};
  case ResizeFilter::Catmullrom: return {[](float x) { return cubic(x, 0.0f, 0.5f); }, 2.0f};
  case ResizeFilter::Lanczos3: return {[](float x) { return lanczos(x, 3.0f); }, 3.0f};
  default: return {[](float) { return 1.0f; }, 0.0f};
  }
}
quint8 floatNearest(float value) {
  const float clamped = std::clamp(value, 0.0f, 255.0f);
  return quint8(std::round(clamped));
}

struct Gray {
  int width = 0, height = 0;
  std::vector<quint8> data;
  quint8 at(int x, int y) const { return data[size_t(y) * size_t(width) + size_t(x)]; }
};

// One axis of image::imageops::sample; `vertical` samples rows, as vertical_sample does.
Gray sample(const Gray &image, int target, const Filter &filter, bool vertical) {
  const int length = vertical ? image.height : image.width;
  Gray out;
  out.width = vertical ? image.width : target;
  out.height = vertical ? target : image.height;
  out.data.resize(size_t(out.width) * size_t(out.height));
  const float ratio = float(length) / float(target);
  const float sratio = ratio < 1.0f ? 1.0f : ratio;
  const float srcSupport = filter.support * sratio;
  std::vector<float> ws;
  for (int o = 0; o < target; ++o) {
    float input = (float(o) + 0.5f) * ratio;
    const qint64 left = std::clamp<qint64>(qint64(std::floor(input - srcSupport)), 0, qint64(length) - 1);
    const qint64 right = std::clamp<qint64>(qint64(std::ceil(input + srcSupport)), left + 1, qint64(length));
    input -= 0.5f;
    ws.clear();
    float sum = 0.0f;
    for (qint64 i = left; i < right; ++i) {
      const float w = filter.kernel((float(i) - input) / sratio);
      ws.push_back(w);
      sum += w;
    }
    const int across = vertical ? image.width : image.height;
    for (int a = 0; a < across; ++a) {
      float t = 0.0f;
      for (size_t i = 0; i < ws.size(); ++i) {
        const int index = int(left) + int(i);
        t += float(vertical ? image.at(a, index) : image.at(index, a)) * ws[i];
      }
      const quint8 value = floatNearest(t / sum);
      if (vertical) out.data[size_t(o) * size_t(out.width) + size_t(a)] = value;
      else out.data[size_t(a) * size_t(out.width) + size_t(o)] = value;
    }
  }
  return out;
}

Gray resize(const Gray &image, int width, int height, ResizeFilter filter) {
  const Filter f = filterFor(filter);
  return sample(sample(image, height, f, true), width, f, false);
}

Gray grayscale(const Pixels &image) {
  Gray out;
  out.width = image.width;
  out.height = image.height;
  out.data.resize(size_t(image.width) * size_t(image.height));
  const auto *p = reinterpret_cast<const quint8 *>(image.data.constData());
  for (size_t i = 0; i < out.data.size(); ++i) {
    const quint8 *px = p + i * size_t(image.channels);
    const float l = 0.2126f * float(px[0]) + 0.7152f * float(px[1]) + 0.0722f * float(px[2]);
    out.data[i] = quint8(l);
  }
  return out;
}

QByteArray fromBools(const std::vector<bool> &bits) {
  QByteArray out;
  for (size_t i = 0; i < bits.size(); i += 8) {
    quint8 byte = 0;
    for (size_t n = 0; n < 8 && i + n < bits.size(); ++n) byte |= quint8(bits[i + n]) << n;
    out.append(char(byte));
  }
  return out;
}

void gradientRows(const std::vector<quint8> &luma, int rowstride, std::vector<bool> &out) {
  for (size_t row = 0; row < luma.size(); row += size_t(rowstride)) {
    const size_t end = std::min(luma.size(), row + size_t(rowstride));
    for (size_t i = row + 1; i < end; ++i) out.push_back(luma[i - 1] < luma[i]);
  }
}
void gradientColumns(const std::vector<quint8> &luma, int rowstride, std::vector<bool> &out) {
  for (int column = 0; column < rowstride; ++column) {
    std::optional<quint8> last;
    for (size_t i = size_t(column); i < luma.size(); i += size_t(rowstride)) {
      if (last) out.push_back(*last < luma[i]);
      last = luma[i];
    }
  }
}

quint32 sumPixel(const quint8 *px, int channels) {
  if (channels == 4 && px[3] == 0) return 255 * 3;
  return quint32(px[0]) + px[1] + px[2];
}

template <typename T> T median(std::vector<T> values) {
  const size_t k = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(k), values.end());
  return values[k];
}

QByteArray blockhash(const Pixels &image, int hwidth, int hheight) {
  const int iwidth = image.width, iheight = image.height;
  const auto *pixels = reinterpret_cast<const quint8 *>(image.data.constData());
  const size_t groupLen = size_t(hwidth) * 4;
  std::vector<bool> bits;
  if (iwidth % hwidth == 0 && iheight % hheight == 0) {
    std::vector<quint32> blocks(size_t(hwidth) * size_t(hheight), 0);
    const int bw = iwidth / hwidth, bh = iheight / hheight;
    for (int y = 0; y < iheight; ++y)
      for (int x = 0; x < iwidth; ++x) blocks[size_t(y / bh) * size_t(hwidth) + size_t(x / bw)] += sumPixel(pixels + (size_t(y) * size_t(iwidth) + size_t(x)) * size_t(image.channels), image.channels);
    const quint32 cmp = quint32(255 * 3) * quint32(bw * bh) / 2;
    for (size_t g = 0; g < blocks.size(); g += groupLen) {
      std::vector<quint32> group(blocks.begin() + std::ptrdiff_t(g), blocks.begin() + std::ptrdiff_t(std::min(blocks.size(), g + groupLen)));
      const quint32 m = median(group);
      for (quint32 block : group) bits.push_back(block > m || (block == m && m > cmp));
    }
    return fromBools(bits);
  }
  std::vector<float> blocks(size_t(hwidth) * size_t(hheight), 0.0f);
  const float bw = float(iwidth) / float(hwidth), bh = float(iheight) / float(hheight);
  auto add = [&](quint32 bx, quint32 by, float value) { blocks[size_t(by) * size_t(hwidth) + size_t(bx)] += value; };
  for (int yi = 0; yi < iheight; ++yi) {
    for (int xi = 0; xi < iwidth; ++xi) {
      const float sum = float(sumPixel(pixels + (size_t(yi) * size_t(iwidth) + size_t(xi)) * size_t(image.channels), image.channels));
      const float x = float(xi), y = float(yi);
      const float blockX = x / bw, blockY = y / bh;
      // img_hash adds `1. % block_width`, not (x + 1) % block_width; the result must match it.
      const float xMod = x + std::fmod(1.0f, bw), yMod = y + std::fmod(1.0f, bh);
      const float weightLeft = xMod - std::trunc(xMod), weightRight = 1.0f - weightLeft;
      const float weightTop = yMod - std::trunc(yMod), weightBottom = 1.0f - weightTop;
      const quint32 left = quint32(std::floor(blockX)), top = quint32(std::floor(blockY));
      const quint32 right = std::trunc(xMod) == 0.0f ? quint32(std::ceil(blockX)) : left;
      const quint32 bottom = std::trunc(yMod) == 0.0f ? quint32(std::ceil(blockY)) : top;
      add(left, top, sum * weightLeft * weightTop);
      add(left, bottom, sum * weightLeft * weightBottom);
      add(right, top, sum * weightRight * weightTop);
      add(right, bottom, sum * weightRight * weightBottom);
    }
  }
  const float cmp = float(255 * 3) * (bw * bh) / 2.0f;
  for (size_t g = 0; g < blocks.size(); g += groupLen) {
    std::vector<float> group(blocks.begin() + std::ptrdiff_t(g), blocks.begin() + std::ptrdiff_t(std::min(blocks.size(), g + groupLen)));
    const float m = median(group);
    for (float block : group) bits.push_back(block > m || (std::fabs(block - m) < 0.001f && m > cmp));
  }
  return fromBools(bits);
}
}  // namespace

QByteArray hash(const Pixels &image, const HashConfig &config) {
  int width = config.width, height = config.height;
  if (config.alg == HashAlgorithm::Phash) {
    width = (width + 1) & ~1;
    height = (height + 1) & ~1;
  } else if (config.alg == HashAlgorithm::Blockhash) {
    width = (width + 3) & ~3;
    height = (height + 3) & ~3;
    return blockhash(image, width, height);
  }
  const Gray gray = grayscale(image);
  std::vector<bool> bits;
  if (config.alg == HashAlgorithm::Dhash) {
    const Gray resized = resize(gray, width + 1, height, config.resizeFilter);
    gradientRows(resized.data, width + 1, bits);
  } else {
    const int w = width / 2 + 1, h = height / 2 + 1;
    const Gray resized = resize(gray, w, h, config.resizeFilter);
    gradientRows(resized.data, w, bits);
    gradientColumns(resized.data, w, bits);
  }
  return fromBools(bits);
}

std::optional<Pixels> decodeWebpRgb(const QByteArray &bytes, quint64 pixelBudget) {
  const auto *data = reinterpret_cast<const uint8_t *>(bytes.constData());
  int w = 0, h = 0;
  if (!WebPGetInfo(data, size_t(bytes.size()), &w, &h) || quint64(w) * quint64(h) > pixelBudget) return std::nullopt;
  WebPBitstreamFeatures features {};
  const bool transparent = WebPGetFeatures(data, size_t(bytes.size()), &features) == VP8_STATUS_OK && features.has_alpha;
  Pixels out;
  out.width = w;
  out.height = h;
  out.channels = 3;
  out.data.resize(qsizetype(w) * h * 3);
  auto *rgb = reinterpret_cast<uint8_t *>(out.data.data());
  if (transparent) {
    // What lies under alpha 0 is not part of the picture: an encoder may keep it (FFmpeg) or clear it (ImageIO).
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    if (!WebPDecodeRGBAInto(data, size_t(bytes.size()), rgba.data(), rgba.size(), w * 4)) return std::nullopt;
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
      const uint8_t *px = &rgba[i * 4];
      for (int c = 0; c < 3; ++c) rgb[i * 3 + size_t(c)] = px[3] == 0 ? 0 : px[c];
    }
  } else if (!WebPDecodeRGBInto(data, size_t(bytes.size()), rgb, size_t(out.data.size()), w * 3)) {
    return std::nullopt;
  }
  return out;
}

Pixels loadImage(const QString &path, const HashConfig &) {
  if (vault::isEncryptedFile(path)) {
    const QByteArray bytes = vault::readPlain(path, qint64(1) << 30);
    DecodePermit permit;
    if (auto rgb = decodeWebpRgb(bytes, 8000000)) return *rgb;
    QImage image;
    if (!image.loadFromData(bytes)) fail(QStringLiteral("%1: cannot decode encrypted image").arg(path));
    if (quint64(image.width()) * quint64(image.height()) > 8000000) fail(QStringLiteral("Direct decode exceeds image memory budget; using thumbnail fallback"));
    image = image.convertToFormat(QImage::Format_RGBA8888);
    Pixels out{image.width(), image.height(), 4, {}};
    out.data.resize(qsizetype(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) memcpy(out.data.data() + qsizetype(y) * out.width * 4, image.constScanLine(y), size_t(out.width) * 4);
    return out;
  }
  if (path::hasExtension(path, {"webp"})) {
    const QByteArray bytes = readAll(path);
    DecodePermit permit;
    if (auto rgb = decodeWebpRgb(bytes, 8000000)) return *rgb;
  }
  QImageReader reader(path);
  reader.setAutoTransform(false);
  const QByteArray format = reader.format().toLower();
  if (format != "jpeg" && format != "jpg" && format != "png") fail(QStringLiteral("The image format could not be determined"));
  const QSize size = reader.size();
  if (!size.isValid()) fail(QStringLiteral("%1: %2").arg(path, reader.errorString()));
  if (quint64(size.width()) * quint64(size.height()) > 8000000) fail(QStringLiteral("Direct decode exceeds image memory budget; using thumbnail fallback"));
  DecodePermit permit;
  QImage image = reader.read();
  if (image.isNull()) fail(QStringLiteral("%1: %2").arg(path, reader.errorString()));
  image = image.convertToFormat(QImage::Format_RGBA8888);
  Pixels out;
  out.width = image.width();
  out.height = image.height();
  out.channels = 4;
  out.data.resize(qsizetype(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y) memcpy(out.data.data() + qsizetype(y) * out.width * 4, image.constScanLine(y), size_t(out.width) * 4);
  return out;
}

}  // namespace core::image_hash
