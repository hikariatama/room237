#pragma once
// Perceptual hashes as img_hash 3.2 computes them over image 0.23 buffers, bit for bit, so hashes cached by earlier
// releases stay comparable with new ones.
#include "settings.h"

#include <QByteArray>
#include <optional>

namespace core::image_hash {

struct Pixels {
  int width = 0, height = 0, channels = 3;  // 3 (RGB8) or 4 (RGBA8, as image::DynamicImage presents every pixel).
  QByteArray data;
};

// The hash bytes in img_hash's packing: bit n of byte k is the hash bit 8k+n.
QByteArray hash(const Pixels &image, const HashConfig &config);
// WebP previews through libwebp; pixels under alpha 0 become black, as the earlier decoder produced them.
std::optional<Pixels> decodeWebpRgb(const QByteArray &bytes, quint64 pixelBudget);
// Originals through the formats the earlier decoder supported (JPEG, PNG, WebP).
Pixels loadImage(const QString &path, const HashConfig &config);

}  // namespace core::image_hash
