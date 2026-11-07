#pragma once
// Shared look: palette, type, cached icons and shadows. All chrome is drawn through this file.
#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QString>
class QWidget;
namespace Theme {
// Design tokens (oklch resolved to sRGB) and the Tailwind v4 palette values the interface uses.
inline const QColor background(23, 23, 23), sidebar(0x15, 0x14, 0x14), popover(23, 23, 23), secondary(38, 38, 38), toastFill(0x1c, 0x1b, 0x1b);
inline const QColor text(255, 255, 255), secondaryText(250, 250, 250), muted(99, 99, 99), border(255, 255, 255, 26);
inline const QColor primary(229, 229, 229), onPrimary(23, 23, 23);
inline const QColor hover(229, 229, 229, 26);
inline const QColor red500(0xfb, 0x2c, 0x36), red400(0xff, 0x64, 0x67), blue500(0x2b, 0x7f, 0xff), green500(0x00, 0xc9, 0x50), emerald500(0x00, 0xbc, 0x7d), emerald400(0x00, 0xd4, 0x92), amber400(0xff, 0xb9, 0x00);
// Corner radii sm to 3xl, derived from a base radius of 0.625rem.
constexpr int kRadiusSm = 6, kRadiusMd = 8, kRadiusLg = 10, kRadiusXl = 14, kRadius2xl = 16, kRadius3xl = 24;
// The platform UI font at the interface's default size; Theme::font gives every other size and weight.
void applyApplicationFont();
// The tiled carpet of the page background (bg.png at 128px wide), anchored to the window origin.
void paintPage(QPainter &painter, const QRect &rect, const QPoint &windowOffset, int whiteAlpha = 0);
// backdrop-blur: a blurred copy of what lies under rect, taken once when a surface opens.
QPixmap frostedCopy(QWidget *source, const QRect &rect, int radius);
QEasingCurve bezier(qreal x1, qreal y1, qreal x2, qreal y2);
QFont font(int pixelSize = 14, QFont::Weight weight = QFont::Normal);
// SVG icon rendered once per name, size, colour and device ratio.
QPixmap icon(const QByteArray &name, int size, const QColor &color, qreal ratio);
void drawIcon(QPainter &painter, const QByteArray &name, const QRectF &rect, const QColor &color);
// Soft shadow as a nine-patch, so a panel of any size costs a few blits.
void drawShadow(QPainter &painter, const QRect &rect, int radius, int blur, int alpha);
void drawPanel(QPainter &painter, const QRect &rect, int radius = kRadius2xl, int shadowAlpha = 70);
// Zero when the system asks for reduced motion; otherwise returns the duration unchanged.
int motion(int milliseconds);
bool reduceMotion();
void applyApplicationStyle();
QColor mix(const QColor &a, const QColor &b, qreal t);
QString elided(const QFont &font, const QString &text, int width);
}
