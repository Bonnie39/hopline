#include "app/IconButton.h"

#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>

namespace hopline {
namespace {

constexpr int kButtonSize = 34;
constexpr double kGlyphExtent = 15.0;

const QColor kGlyph(232, 232, 232);

// Normalized (0..1) point within the glyph box.
QPointF at(const QRectF& box, double nx, double ny)
{
    return { box.left() + nx * box.width(), box.top() + ny * box.height() };
}

QPainterPath leftTriangle(const QRectF& box, double x0, double x1)
{
    QPainterPath path;
    path.moveTo(at(box, x1, 0.14));
    path.lineTo(at(box, x1, 0.86));
    path.lineTo(at(box, x0, 0.5));
    path.closeSubpath();
    return path;
}

QPainterPath rightTriangle(const QRectF& box, double x0, double x1)
{
    QPainterPath path;
    path.moveTo(at(box, x0, 0.14));
    path.lineTo(at(box, x0, 0.86));
    path.lineTo(at(box, x1, 0.5));
    path.closeSubpath();
    return path;
}

QPainterPath glyphPath(IconButton::Glyph glyph, const QRectF& box)
{
    QPainterPath path;
    switch (glyph) {
    case IconButton::Glyph::Play:
        path = rightTriangle(box, 0.16, 0.84);
        break;
    case IconButton::Glyph::Pause:
        path.addRoundedRect(QRectF(at(box, 0.22, 0.14), at(box, 0.40, 0.86)), 1.5, 1.5);
        path.addRoundedRect(QRectF(at(box, 0.60, 0.14), at(box, 0.78, 0.86)), 1.5, 1.5);
        break;
    case IconButton::Glyph::Rewind:
        path = leftTriangle(box, 0.06, 0.52);
        path.addPath(leftTriangle(box, 0.50, 0.96));
        break;
    case IconButton::Glyph::Forward:
        path = rightTriangle(box, 0.04, 0.50);
        path.addPath(rightTriangle(box, 0.48, 0.94));
        break;
    case IconButton::Glyph::SkipBack:
        path.addRect(QRectF(at(box, 0.10, 0.15), at(box, 0.22, 0.85)));
        path.addPath(leftTriangle(box, 0.30, 0.92));
        break;
    }
    return path;
}

}  // namespace

IconButton::IconButton(Glyph glyph, QWidget* parent)
    : QAbstractButton(parent)
    , m_glyph(glyph)
{
}

void IconButton::setGlyph(Glyph glyph)
{
    if (m_glyph != glyph) {
        m_glyph = glyph;
        update();
    }
}

QSize IconButton::sizeHint() const { return { kButtonSize, kButtonSize }; }

void IconButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor fill(0, 0, 0, 0);
    if (isDown()) {
        fill = QColor(255, 255, 255, 50);
    } else if (underMouse()) {
        fill = QColor(255, 255, 255, 28);
    }
    if (fill.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(rect(), 6, 6);
    }

    QRectF box(0, 0, kGlyphExtent, kGlyphExtent);
    box.moveCenter(QRectF(rect()).center());
    painter.setPen(Qt::NoPen);
    painter.setBrush(kGlyph);
    painter.drawPath(glyphPath(m_glyph, box));
}

void IconButton::enterEvent(QEnterEvent*) { update(); }
void IconButton::leaveEvent(QEvent*) { update(); }

}  // namespace hopline
