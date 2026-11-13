#include "theme.h"
#include "platform.h"
#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHash>
#include <QPainterPath>
#include <QPalette>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QWidget>
#include <iterator>
static const QHash<QByteArray, QByteArray> &iconSources() {
  static const QHash<QByteArray, QByteArray> table = [] {
    QHash<QByteArray, QByteArray> out{
#include "generated/icons_data.inc"
    };
    const auto custom = [&out](const char *name, const char *body) {
      out.insert(name, QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"@C\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">") + body + "</svg>");
    };
    // From @tabler/icons (MIT), like the generated set.
    custom("eye", "<path d=\"M10 12a2 2 0 1 0 4 0a2 2 0 0 0 -4 0\"/><path d=\"M21 12c-2.4 4 -5.4 6 -9 6c-3.6 0 -6.6 -2 -9 -6c2.4 -4 5.4 -6 9 -6c3.6 0 6.6 2 9 6\"/>");
    custom("eye-off", "<path d=\"M10.585 10.587a2 2 0 0 0 2.829 2.828\"/><path d=\"M16.681 16.673a8.717 8.717 0 0 1 -4.681 1.327c-3.6 0 -6.6 -2 -9 -6c1.272 -2.12 2.712 -3.678 4.32 -4.674m2.86 -1.146a9.055 9.055 0 0 1 1.82 -.18c3.6 0 6.6 2 9 6c-.666 1.11 -1.379 2.067 -2.138 2.87\"/><path d=\"M3 3l18 18\"/>");
    custom("key", "<path d=\"M16.555 3.843l3.602 3.602a2.877 2.877 0 0 1 0 4.069l-2.643 2.643a2.877 2.877 0 0 1 -4.069 0l-.301 -.301l-6.558 6.558a2 2 0 0 1 -1.239 .578l-.175 .008h-1.172a1 1 0 0 1 -.993 -.883l-.007 -.117v-1.172a2 2 0 0 1 .467 -1.284l.119 -.13l.414 -.414h2v-2h2v-2l2.144 -2.144l-.301 -.301a2.877 2.877 0 0 1 0 -4.069l2.643 -2.643a2.877 2.877 0 0 1 4.069 0z\"/><path d=\"M15 9h.01\"/>");
    custom("fingerprint", "<path d=\"M18.9 7a8 8 0 0 1 1.1 5v1a6 6 0 0 0 .8 3\"/><path d=\"M8 11a4 4 0 0 1 8 0v1a10 10 0 0 0 2 6\"/><path d=\"M12 11v2a14 14 0 0 0 2.5 8\"/><path d=\"M8 15a18 18 0 0 0 1.8 6\"/><path d=\"M4.9 19a22 22 0 0 1 -.9 -7v-1a8 8 0 0 1 12 -6.95\"/>");
    custom("lock-open", "<path d=\"M5 13a2 2 0 0 1 2 -2h10a2 2 0 0 1 2 2v6a2 2 0 0 1 -2 2h-10a2 2 0 0 1 -2 -2z\"/><path d=\"M11 16a1 1 0 1 0 2 0a1 1 0 0 0 -2 0\"/><path d=\"M8 11v-5a4 4 0 0 1 8 0\"/>");
    custom("layout-default", "<rect x=\"3.5\" y=\"3.5\" width=\"7\" height=\"7\" rx=\"2\"/><rect x=\"13.5\" y=\"3.5\" width=\"7\" height=\"7\" rx=\"2\"/><rect x=\"3.5\" y=\"13.5\" width=\"7\" height=\"7\" rx=\"2\"/><rect x=\"13.5\" y=\"13.5\" width=\"7\" height=\"7\" rx=\"2\"/>");
    custom("layout-masonry", "<rect x=\"3.5\" y=\"3.5\" width=\"7\" height=\"10\" rx=\"2\"/><rect x=\"3.5\" y=\"16.5\" width=\"7\" height=\"4\" rx=\"1.5\"/><rect x=\"13.5\" y=\"3.5\" width=\"7\" height=\"4\" rx=\"1.5\"/><rect x=\"13.5\" y=\"10.5\" width=\"7\" height=\"10\" rx=\"2\"/>");
    custom("layout-apple", "<g stroke-opacity=\".45\"><rect x=\"3.5\" y=\"3.5\" width=\"7\" height=\"7\" rx=\"1.5\"/><rect x=\"13.5\" y=\"3.5\" width=\"7\" height=\"7\" rx=\"1.5\"/><rect x=\"3.5\" y=\"13.5\" width=\"7\" height=\"7\" rx=\"1.5\"/><rect x=\"13.5\" y=\"13.5\" width=\"7\" height=\"7\" rx=\"1.5\"/></g><rect x=\"4.75\" y=\"5.5\" width=\"4.5\" height=\"3\" rx=\".75\" fill=\"@C\" stroke=\"none\"/><rect x=\"15.5\" y=\"4.75\" width=\"3\" height=\"4.5\" rx=\".75\" fill=\"@C\" stroke=\"none\"/><rect x=\"5.75\" y=\"15.25\" width=\"3\" height=\"3\" rx=\".75\" fill=\"@C\" stroke=\"none\"/><rect x=\"14.75\" y=\"15.5\" width=\"4.5\" height=\"3\" rx=\".75\" fill=\"@C\" stroke=\"none\"/>");
    return out;
  }();
  return table;
}
namespace Theme {
// The interface uses installed fonts; the app ships no font files. macOS: SF Pro Text when it is installed (the face
// 6.x bundled), otherwise the system font set to the same advances. Windows: Segoe UI. Linux: the desktop's font, then
// common sans families.
#ifdef Q_OS_MACOS
static bool hasSfProText() {
  static const bool has = QFontDatabase::hasFamily(QStringLiteral("SF Pro Text"));
  return has;
}
#endif
static QFont baseFont() {
  QFont out = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#if defined(Q_OS_MACOS)
  out.setFamilies({hasSfProText() ? QStringLiteral("SF Pro Text") : QStringLiteral(".AppleSystemUIFont")});
#elif defined(Q_OS_WIN)
  out.setFamilies({QStringLiteral("Segoe UI"), out.family()});
#else
  out.setFamilies({out.family(), QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
#endif
  return out;
}
#ifdef Q_OS_MACOS
// CoreText tracks the system font by size, and differently per platform plugin. The spacing that gives the advances of
// SF Pro Text is measured once per size against the width SF Pro Text has for a sample line (weight makes no difference
// to it within 0.01 px per character).
static qreal trackingCorrection(const QFont &font) {
  static QHash<int, qreal> cache;
  const int px = font.pixelSize();
  if (const auto it = cache.constFind(px); it != cache.cend()) return *it;
  static const QString sample = QStringLiteral("Room237 Albums Альбомы 2024 abcdefghijklmnopqrstuvwxyz");
  // SF Pro Text Regular, this sample, 9..32 px.
  static constexpr qreal reference[] = {268.062, 297.922, 327.672, 357.547, 387.344, 417.141, 446.984, 477.109, 506.594, 536.484, 566.234, 596.172,
                                        625.906, 655.703, 685.516, 715.500, 745.188, 775.078, 804.859, 834.703, 864.453, 894.312, 924.109, 954.312};
  QFont plain(font);
  plain.setWeight(QFont::Normal);
  plain.setLetterSpacing(QFont::AbsoluteSpacing, 0);
  const qreal target = px >= 9 && px <= 32 ? reference[px - 9] : px * 29.81;
  const qreal spacing = (target - QFontMetricsF(plain).horizontalAdvance(sample)) / sample.size();
  cache.insert(px, spacing);
  return spacing;
}
#endif
QFont font(int pixelSize, QFont::Weight weight) {
  static const QFont base = baseFont();
  QFont out(base);
  out.setPixelSize(pixelSize);
  out.setWeight(weight);
  out.setHintingPreference(QFont::PreferNoHinting);
#ifdef Q_OS_MACOS
  if (!hasSfProText()) {
    out.setVariableAxis(QFont::Tag("opsz"), 17);
    out.setLetterSpacing(QFont::AbsoluteSpacing, trackingCorrection(out));
  }
#endif
  return out;
}
void applyApplicationFont() { QApplication::setFont(font(14)); }
void paintPage(QPainter &painter, const QRect &rect, const QPoint &windowOffset, int whiteAlpha) {
  static QPixmap tile, tinted;
  static int tintedAlpha = -1;
  static qreal tileRatio = 0;
  const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1;
  if (tileRatio != ratio) {
    tileRatio = ratio;
    QImage source(QStringLiteral(":/bg.png"));
    const QSize size = QSize(128, qRound(128.0 * source.height() / source.width())) * ratio;
    tile = QPixmap::fromImage(source.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32));
    tile.setDevicePixelRatio(ratio);
    tintedAlpha = -1;
  }
  if (whiteAlpha > 0 && tintedAlpha != whiteAlpha) {
    // A card's translucent white, ready-made: a frame draws one texture instead of blending over another.
    tintedAlpha = whiteAlpha;
    tinted = tile;
    tinted.setDevicePixelRatio(ratio);
    QPainter over(&tinted);
    over.fillRect(QRect(QPoint(0, 0), tile.deviceIndependentSize().toSize()), QColor(255, 255, 255, whiteAlpha));
  }
  const QPixmap &texture = whiteAlpha > 0 ? tinted : tile;
  const int scale = qRound(ratio);
  const QSize cell = texture.size() / scale;
  // The gallery shows the page in many small gaps per frame: plain copies beat a textured fill for each one.
  // A large area is the opposite, and the review's cards are large.
  if (scale != ratio || rect.width() * rect.height() > cell.width() * cell.height()) return painter.drawTiledPixmap(rect, texture, windowOffset + rect.topLeft());
  const QPoint origin = windowOffset + rect.topLeft();
  for (int y = 0, sy = origin.y() % cell.height(); y < rect.height(); sy = 0) {
    const int h = qMin(cell.height() - sy, rect.height() - y);
    for (int x = 0, sx = origin.x() % cell.width(); x < rect.width(); sx = 0) {
      const int w = qMin(cell.width() - sx, rect.width() - x);
      painter.drawPixmap(QPointF(rect.x() + x, rect.y() + y), texture, QRectF(sx * scale, sy * scale, w * scale, h * scale));
      x += w;
    }
    y += h;
  }
}
// Three box passes approximate a gaussian (W3C filter effects); a quarter-size image keeps this in the millisecond range.
static void boxPass(QImage &image, int radius, bool vertical) {
  const int w = image.width(), h = image.height(), lines = vertical ? w : h, length = vertical ? h : w;
  const int step = vertical ? image.bytesPerLine() / 4 : 1, stride = vertical ? 1 : image.bytesPerLine() / 4;
  auto *bits = reinterpret_cast<quint32 *>(image.bits());
  const int window = 2 * radius + 1;
  QVector<quint32> line(length);
  for (int i = 0; i < lines; i++) {
    quint32 *start = bits + i * stride;
    for (int k = 0; k < length; k++) line[k] = start[k * step];
    int r = 0, g = 0, b = 0;
    for (int k = -radius; k <= radius; k++) {
      const quint32 p = line[qBound(0, k, length - 1)];
      r += qRed(p);
      g += qGreen(p);
      b += qBlue(p);
    }
    for (int k = 0; k < length; k++) {
      start[k * step] = qRgb(r / window, g / window, b / window);
      const quint32 in = line[qMin(k + radius + 1, length - 1)], out = line[qMax(k - radius, 0)];
      r += qRed(in) - qRed(out);
      g += qGreen(in) - qGreen(out);
      b += qBlue(in) - qBlue(out);
    }
  }
}
QPixmap frostedCopy(QWidget *source, const QRect &rect, int radius) {
  return frosted(source->grab(rect).toImage(), rect.size(), source->devicePixelRatioF(), radius);
}
QPixmap frosted(QImage image, const QSize &size, qreal ratio, int radius) {
  const QRect rect(QPoint(0, 0), size);
  if (image.isNull()) return {};
  // radius is the CSS blur() value, a standard deviation in points.
  constexpr int factor = 4;
  image = image.scaled(QSize(qMax(4, image.width() / factor), qMax(4, image.height() / factor)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
  const int box = qMax(1, qRound(radius * ratio / factor * 1.88 / 2));
  for (int pass = 0; pass < 3; pass++) {
    boxPass(image, box, false);
    boxPass(image, box, true);
  }
  image = image.scaled(rect.size() * ratio, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QPixmap out = QPixmap::fromImage(image);
  out.setDevicePixelRatio(ratio);
  return out;
}
QEasingCurve bezier(qreal x1, qreal y1, qreal x2, qreal y2) {
  QEasingCurve curve(QEasingCurve::BezierSpline);
  curve.addCubicBezierSegment(QPointF(x1, y1), QPointF(x2, y2), QPointF(1, 1));
  return curve;
}
QPixmap icon(const QByteArray &name, int size, const QColor &color, qreal ratio) {
  static QHash<QString, QPixmap> cache;
  const QString key = QString::fromLatin1(name) + QLatin1Char('|') + QString::number(size) + QLatin1Char('|') + QString::number(color.rgba(), 16) + QLatin1Char('|') + QString::number(ratio);
  auto it = cache.constFind(key);
  if (it != cache.cend()) return *it;
  QByteArray svg = iconSources().value(name);
  if (svg.isEmpty()) qWarning("missing icon %s", name.constData());
  svg.replace("@C", color.name(QColor::HexRgb).toLatin1());
  QSvgRenderer renderer(svg);
  QPixmap pixmap(QSize(size, size) * ratio);
  pixmap.fill(Qt::transparent);
  {
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(color.alphaF());
    renderer.render(&painter);
  }
  pixmap.setDevicePixelRatio(ratio);
  cache.insert(key, pixmap);
  return pixmap;
}
void drawIcon(QPainter &painter, const QByteArray &name, const QRectF &rect, const QColor &color) {
  const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1;
  const QPixmap pixmap = icon(name, qRound(qMin(rect.width(), rect.height())), color, ratio);
  const QSizeF logical = QSizeF(pixmap.size()) / ratio;
  painter.drawPixmap(QPointF(rect.x() + (rect.width() - logical.width()) / 2, rect.y() + (rect.height() - logical.height()) / 2), pixmap);
}
static QImage blurredAlpha(int side, int inner, int radius, int blur) {
  QImage image(side, side, QImage::Format_Alpha8);
  image.fill(0);
  {
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawRoundedRect(QRectF((side - inner) / 2.0, (side - inner) / 2.0, inner, inner), radius, radius);
  }
  const int r = qMax(1, blur / 2);
  QImage scratch = image;
  for (int pass = 0; pass < 3; pass++) {
    for (int y = 0; y < side; y++) {
      const uchar *in = image.constScanLine(y);
      uchar *out = scratch.scanLine(y);
      int sum = 0;
      for (int x = -r; x <= r; x++) sum += in[qBound(0, x, side - 1)];
      for (int x = 0; x < side; x++) {
        out[x] = uchar(sum / (2 * r + 1));
        sum += in[qMin(side - 1, x + r + 1)] - in[qMax(0, x - r)];
      }
    }
    for (int x = 0; x < side; x++) {
      int sum = 0;
      for (int y = -r; y <= r; y++) sum += scratch.constScanLine(qBound(0, y, side - 1))[x];
      for (int y = 0; y < side; y++) {
        image.scanLine(y)[x] = uchar(sum / (2 * r + 1));
        sum += scratch.constScanLine(qMin(side - 1, y + r + 1))[x] - scratch.constScanLine(qMax(0, y - r))[x];
      }
    }
  }
  return image;
}
void drawShadow(QPainter &painter, const QRect &rect, int radius, int blur, int alpha) {
  static QHash<quint64, QPixmap> cache;
  const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1;
  const int r = qRound(radius * ratio), b = qRound(blur * ratio), corner = r + b * 2, side = corner * 2 + 4;
  const quint64 key = (quint64(r) << 32) | (quint64(b) << 16) | quint64(alpha);
  auto it = cache.find(key);
  if (it == cache.end()) {
    const QImage mask = blurredAlpha(side, side - b * 2, r, b);
    QImage tinted(side, side, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < side; y++) {
      const uchar *in = mask.constScanLine(y);
      auto *out = reinterpret_cast<QRgb *>(tinted.scanLine(y));
      for (int x = 0; x < side; x++) out[x] = qRgba(0, 0, 0, in[x] * alpha / 255);
    }
    it = cache.insert(key, QPixmap::fromImage(tinted));
  }
  const QPixmap &p = *it;
  const qreal c = corner / ratio, grow = blur;
  const QRectF outer = QRectF(rect).adjusted(-grow, -grow, grow, grow);
  const qreal mx = outer.width() - 2 * c, my = outer.height() - 2 * c;
  if (mx <= 0 || my <= 0) return;
  const int cs = corner, mid = side - 2 * corner;
  const QRectF t[9] = {{outer.x(), outer.y(), c, c}, {outer.x() + c, outer.y(), mx, c}, {outer.right() - c, outer.y(), c, c}, {outer.x(), outer.y() + c, c, my}, {outer.x() + c, outer.y() + c, mx, my}, {outer.right() - c, outer.y() + c, c, my}, {outer.x(), outer.bottom() - c, c, c}, {outer.x() + c, outer.bottom() - c, mx, c}, {outer.right() - c, outer.bottom() - c, c, c}};
  const QRect s[9] = {{0, 0, cs, cs}, {cs, 0, mid, cs}, {cs + mid, 0, cs, cs}, {0, cs, cs, mid}, {cs, cs, mid, mid}, {cs + mid, cs, cs, mid}, {0, cs + mid, cs, cs}, {cs, cs + mid, mid, cs}, {cs + mid, cs + mid, cs, cs}};
  for (int i = 0; i < 9; i++)
    if (i != 4) painter.drawPixmap(t[i], p, s[i]);
}
void drawPanel(QPainter &painter, const QRect &rect, int radius, int shadowAlpha) {
  painter.save();
  drawShadow(painter, rect.translated(0, 4), radius, 12, shadowAlpha);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(QPen(border, 1));
  painter.setBrush(popover);
  painter.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
  painter.restore();
}
bool reduceMotion() {
  static const bool forced = qEnvironmentVariableIntValue("ROOM237_REDUCE_MOTION") != 0;
  return forced || Platform::reduceMotion();
}
int motion(int milliseconds) { return reduceMotion() ? 0 : milliseconds; }
QColor mix(const QColor &a, const QColor &b, qreal t) {
  return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t, a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}
QString elided(const QFont &font, const QString &text, int width) { return QFontMetrics(font).elidedText(text, Qt::ElideRight, width); }
void applyApplicationStyle() {
  QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  QPalette palette;
  palette.setColor(QPalette::Window, background);
  palette.setColor(QPalette::WindowText, text);
  palette.setColor(QPalette::Base, secondary);
  palette.setColor(QPalette::AlternateBase, sidebar);
  palette.setColor(QPalette::Text, text);
  palette.setColor(QPalette::PlaceholderText, muted);
  palette.setColor(QPalette::Button, secondary);
  palette.setColor(QPalette::ButtonText, text);
  palette.setColor(QPalette::Highlight, primary);
  palette.setColor(QPalette::HighlightedText, onPrimary);
  palette.setColor(QPalette::ToolTipBase, popover);
  palette.setColor(QPalette::ToolTipText, text);
  QApplication::setPalette(palette);
  qApp->setStyleSheet(QStringLiteral(
      "QToolTip{background:#171717;color:#ffffff;border:1px solid rgba(255,255,255,26);border-radius:6px;padding:3px 6px;}"
      "QLineEdit,QSpinBox{font-size:14px;background:transparent;color:#ffffff;border:1px solid rgba(255,255,255,26);border-radius:18px;padding:0 12px;min-height:34px;selection-background-color:#e5e5e5;selection-color:#171717;font-size:16px;}"
      "QLineEdit:focus,QSpinBox:focus{border:1px solid #737373;}"
      "QSpinBox::up-button,QSpinBox::down-button{width:0;border:none;}"
      "QScrollArea{background:transparent;border:none;}"
      "QScrollBar:vertical{background:transparent;width:10px;margin:1px;}"
      "QScrollBar::handle:vertical{background:rgba(255,255,255,26);border-radius:4px;min-height:24px;}"
      "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical,QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{height:0;background:none;}"));
}
}
