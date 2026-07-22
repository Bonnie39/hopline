#include "app/ToolboxWidget.h"

#include <QButtonGroup>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QToolButton>
#include <QVBoxLayout>

namespace hopline {
namespace {

const QColor kToolColor(212, 214, 220);

QPixmap toolIcon(int tool)
{
    constexpr int s = 22;
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QPen stroke(kToolColor, 2);
    stroke.setJoinStyle(Qt::RoundJoin);
    stroke.setCapStyle(Qt::RoundCap);
    p.setPen(Qt::NoPen);
    p.setBrush(kToolColor);

    switch (tool) {
    case ToolboxWidget::Select: {
        QPainterPath path;  // pointer arrow
        path.moveTo(5, 3);
        path.lineTo(5, 18);
        path.lineTo(9, 14);
        path.lineTo(12, 20);
        path.lineTo(14, 19);
        path.lineTo(11, 13.5);
        path.lineTo(16, 13);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case ToolboxWidget::Razor: {
        QPainterPath blade;  // angled blade
        blade.moveTo(4, 4);
        blade.lineTo(15, 5);
        blade.lineTo(13, 11);
        blade.lineTo(4, 11);
        blade.closeSubpath();
        p.drawPath(blade);
        p.setPen(stroke);  // handle
        p.setBrush(Qt::NoBrush);
        p.drawLine(6, 12, 6, 19);
        break;
    }
    case ToolboxWidget::Hand: {
        for (int i = 0; i < 4; ++i)  // fingers
            p.drawRoundedRect(QRectF(5.0 + i * 3.0, 4, 2.4, 8), 1, 1);
        p.drawRoundedRect(QRectF(4, 10, 12, 8), 3, 3);  // palm
        p.drawRoundedRect(QRectF(2, 11, 3, 5), 1.5, 1.5);  // thumb
        break;
    }
    case ToolboxWidget::Zoom: {
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(9, 9), 5, 5);
        p.drawLine(12.5, 12.5, 19, 19);
        break;
    }
    }
    return pm;
}

const char* kTips[] = { "Selection", "Razor", "Hand", "Zoom" };

}  // namespace

ToolboxWidget::ToolboxWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 5, 3, 5);
    layout->setSpacing(3);

    auto* group = new QButtonGroup(this);
    group->setExclusive(true);

    for (int i = 0; i <= Zoom; ++i) {
        auto* button = new QToolButton(this);
        button->setIcon(QIcon(toolIcon(i)));
        button->setIconSize(QSize(22, 22));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolTip(kTips[i]);
        if (i == Select) button->setChecked(true);
        group->addButton(button, i);
        layout->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, i] { emit toolSelected(i); });
    }
    layout->addStretch(1);

    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

}  // namespace hopline
