#include "app/DragValue.h"

#include <algorithm>
#include <cmath>

#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>

namespace hopline {

namespace {
constexpr int kDragThreshold = 3;
const QColor kValueColor(126, 178, 230);  // highlighted, clickable-looking number
const QColor kSuffixColor(120, 122, 128);
}  // namespace

DragValue::DragValue(QWidget* parent)
    : QWidget(parent)
{
    setCursor(Qt::SizeHorCursor);  // signals "drag me"
    setFocusPolicy(Qt::ClickFocus);
}

QSize DragValue::sizeHint() const
{
    return QSize(72, 22);
}

void DragValue::setRange(double lo, double hi)
{
    m_min = lo;
    m_max = hi;
    setValue(m_value);
}

void DragValue::setDecimals(int decimals)
{
    m_decimals = decimals;
    update();
}

void DragValue::setSuffix(const QString& suffix)
{
    m_suffix = suffix;
    update();
}

void DragValue::setStep(double perPixel)
{
    m_step = perPixel;
}

double DragValue::clamp(double v) const
{
    return std::clamp(v, m_min, m_max);
}

void DragValue::setValue(double value)
{
    m_value = clamp(value);
    update();
}

QString DragValue::displayText() const
{
    return QString::number(m_value, 'f', m_decimals) + m_suffix;
}

void DragValue::paintEvent(QPaintEvent*)
{
    if (m_editor && m_editor->isVisible()) {
        return;  // the text editor covers us
    }
    QPainter p(this);
    p.setPen(kValueColor);
    const QString num = QString::number(m_value, 'f', m_decimals);
    const QRect r = rect().adjusted(1, 0, -1, 0);
    p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, num);
    if (!m_suffix.isEmpty()) {
        const int numW = fontMetrics().horizontalAdvance(num);
        p.setPen(kSuffixColor);
        p.drawText(r.adjusted(numW + 1, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_suffix);
    }
}

void DragValue::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_pressed = true;
    m_dragged = false;
    m_pressPos = event->position().toPoint();
    m_pressValue = m_value;
}

void DragValue::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_pressed) {
        return;
    }
    const int dx = event->position().toPoint().x() - m_pressPos.x();
    if (!m_dragged && std::abs(dx) > kDragThreshold) {
        m_dragged = true;
    }
    if (m_dragged) {
        const double nv = clamp(m_pressValue + dx * m_step);
        if (nv != m_value) {
            m_value = nv;
            update();
            emit valueChanged(m_value, false);  // live preview
        }
    }
}

void DragValue::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !m_pressed) {
        return;
    }
    m_pressed = false;
    if (m_dragged) {
        emit valueChanged(m_value, true);  // commit the drag as one undo step
    } else {
        beginEdit();  // a click (no drag) opens the text editor
    }
}

void DragValue::beginEdit()
{
    if (!m_editor) {
        m_editor = new QLineEdit(this);
        m_editor->setFrame(false);
        connect(m_editor, &QLineEdit::editingFinished, this, &DragValue::commitText);
    }
    m_editor->setGeometry(rect());
    m_editor->setText(QString::number(m_value, 'f', m_decimals));
    m_editor->selectAll();
    m_editor->show();
    m_editor->setFocus();
}

void DragValue::commitText()
{
    if (!m_editor || !m_editor->isVisible()) {
        return;
    }
    bool ok = false;
    const double parsed = m_editor->text().toDouble(&ok);
    m_editor->hide();
    setFocus();
    if (ok) {
        const double v = clamp(parsed);
        if (v != m_value) {
            m_value = v;
            update();
            emit valueChanged(m_value, true);
            return;
        }
    }
    update();
}

}  // namespace hopline
