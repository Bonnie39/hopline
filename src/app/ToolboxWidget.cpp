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
    case ToolboxWidget::Blade: {
        p.save();  // an angled razor blade
        p.translate(11, 11);
        p.rotate(-30);
        p.drawRoundedRect(QRectF(-8, -5.5, 15, 8), 1.5, 1.5);   // blade body
        p.setBrush(QColor(24, 25, 27));
        p.drawEllipse(QPointF(-4.5, -1.5), 1.3, 1.3);           // mounting hole
        p.setPen(QPen(QColor(250, 250, 250), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(-8, 2.5), QPointF(7, 2.5));          // cutting edge
        p.restore();
        break;
    }
    case ToolboxWidget::Text: {
        p.drawRoundedRect(QRectF(4, 4, 14, 3.2), 1, 1);   // top bar of a "T"
        p.drawRoundedRect(QRectF(9.4, 4, 3.2, 14), 1, 1);  // stem
        break;
    }
    }
    return pm;
}

const char* kTips[] = { "Selection (V)", "Blade (C)", "Text (T)" };

}  // namespace

ToolboxWidget::ToolboxWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(3, 5, 3, 5);
    layout->setSpacing(3);

    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);

    for (int i = 0; i <= Text; ++i) {
        auto* button = new QToolButton(this);
        button->setIcon(QIcon(toolIcon(i)));
        button->setIconSize(QSize(22, 22));
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolTip(kTips[i]);
        if (i == Select) button->setChecked(true);
        m_group->addButton(button, i);
        layout->addWidget(button);
        connect(button, &QToolButton::clicked, this, [this, i] { emit toolSelected(i); });
    }
    layout->addStretch(1);

    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void ToolboxWidget::setCurrentTool(int tool)
{
    if (auto* b = m_group->button(tool)) {
        b->setChecked(true);  // checking doesn't emit clicked, so no toolSelected loop
    }
}

}  // namespace hopline
