#include "app/EffectControls.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <utility>

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPolygonF>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/DragValue.h"

namespace hopline {
namespace {

constexpr double kPi = 3.14159265358979323846;

const QColor kFxIconColor(214, 216, 222);
const QColor kFxIconActive(126, 176, 236);   // stopwatch tint when keyframed

int idx(FxProp p) { return static_cast<int>(p); }

struct PropCfg {
    double lo, hi, step;
    int decimals;
    const char* suffix;
};

PropCfg cfgFor(FxProp p)
{
    switch (p) {
    case FxProp::PosX:
    case FxProp::PosY:     return { -40000.0, 40000.0, 1.0, 1, " px" };
    case FxProp::Scale:    return { 1.0, 1000.0, 0.5, 1, "%" };
    case FxProp::Rotation: return { -3600.0, 3600.0, 0.5, 1, "°" };
    case FxProp::Opacity:  return { 0.0, 100.0, 0.5, 1, "%" };
    case FxProp::VolumeDb: return { -60.0, 12.0, 0.1, 1, " dB" };
    case FxProp::Pan:      return { -100.0, 100.0, 1.0, 0, "" };
    }
    return { 0.0, 0.0, 1.0, 1, "" };
}

enum class Glyph { Stopwatch, Reset, ArrowLeft, ArrowRight, Diamond, DiamondFilled };

// Bold, filled vector icons matching the transport / toolbox glyphs. See CLAUDE.md (Effects).
QIcon makeIcon(Glyph g, const QColor& color)
{
    constexpr qreal kScale = 3.0;
    constexpr int kGrid = 44;
    QPixmap pm(int(kGrid * kScale), int(kGrid * kScale));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(kScale, kScale);  // draw in the 44-unit grid; don't set DPR too or it double-scales

    const double cx = 22.0;
    QPen stroke(color, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

    switch (g) {
    case Glyph::Stopwatch:
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(cx - 7, 3.5, 14, 5), 2.5, 2.5);  // top button
        p.drawRect(QRectF(cx - 2.0, 8, 4, 5));                     // stem
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(cx, 27), 13.5, 13.5);               // body
        p.drawLine(QPointF(cx, 27), QPointF(cx + 7, 20));         // hand
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(cx, 27), 2.6, 2.6);                 // pivot
        break;
    case Glyph::Reset: {
        p.setPen(stroke);
        p.setBrush(Qt::NoBrush);
        const QRectF rr(9.5, 9.5, 25, 25);
        QPainterPath arc;
        arc.arcMoveTo(rr, 68);
        arc.arcTo(rr, 68, 250);
        p.drawPath(arc);
        const double a = 68.0 * kPi / 180.0;  // arrowhead at the arc's open end
        const QPointF tip(cx + 12.5 * std::cos(a), cx - 12.5 * std::sin(a));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF head;
        head << tip << tip + QPointF(-2.5, -8.0) << tip + QPointF(8.0, -3.0);
        p.drawPolygon(head);
        break;
    }
    case Glyph::ArrowLeft: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF t;
        t << QPointF(28, 11) << QPointF(15, 22) << QPointF(28, 33);
        p.drawPolygon(t);
        break;
    }
    case Glyph::ArrowRight: {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPolygonF t;
        t << QPointF(16, 11) << QPointF(29, 22) << QPointF(16, 33);
        p.drawPolygon(t);
        break;
    }
    case Glyph::Diamond:
    case Glyph::DiamondFilled: {
        QPolygonF d;
        d << QPointF(cx, 7) << QPointF(35, 22) << QPointF(cx, 37) << QPointF(9, 22);
        if (g == Glyph::DiamondFilled) {
            p.setPen(Qt::NoPen);
            p.setBrush(color);
        } else {
            p.setPen(QPen(color, 3.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
        }
        p.drawPolygon(d);
        break;
    }
    }
    p.end();
    return QIcon(pm);
}

double niceInterval(double pxPerSec)
{
    const double target = 64.0 / std::max(pxPerSec, 1e-6);
    const double mag = std::pow(10.0, std::floor(std::log10(std::max(target, 1e-6))));
    for (double s : { 1.0, 2.0, 5.0, 10.0 }) {
        if (mag * s >= target) {
            return mag * s;
        }
    }
    return mag * 10.0;
}

QString timeLabel(double seconds)
{
    const int total = static_cast<int>(seconds);
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

}  // namespace

// ── Keyframe pane ────────────────────────────────────────────────────────────
class KeyframePane : public QWidget {
public:
    explicit KeyframePane(DragValue* const* values, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_values(values)
    {
        setMinimumWidth(200);
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);  // for the Delete key
    }

    std::function<void(Tick timelineTime)> onSeek;
    std::function<void()> onScrubBegin;
    std::function<void()> onScrubEnd;
    std::function<void(const std::vector<KeyEdit>&)> onEdits;

    void setData(const FxView& v)
    {
        m_clipStart = v.clipStart;
        m_duration = v.clipDuration;
        m_playheadLocal = v.playheadLocal;
        for (int i = 0; i < kFxPropCount; ++i) {
            m_keys[i] = v.keys[i];
        }
        // Drop any selected keys that no longer exist (e.g. after a move/delete).
        for (auto it = m_selected.begin(); it != m_selected.end();) {
            const std::vector<Tick>& k = m_keys[it->first];
            it = std::find(k.begin(), k.end(), it->second) == k.end() ? m_selected.erase(it) : std::next(it);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    static constexpr int kPad = 10;
    static constexpr int kRulerH = 20;
    static constexpr int kGrab = 5;

    int laneY(FxProp p) const
    {
        DragValue* dv = m_values[idx(p)];
        if (!dv || !dv->isVisible()) {
            return -1;
        }
        return dv->mapTo(parentWidget(), QPoint(0, dv->height() / 2)).y();
    }
    int xForTick(Tick localT) const
    {
        const int area = std::max(1, width() - 2 * kPad);
        const double f = m_duration > 0 ? static_cast<double>(localT) / m_duration : 0.0;
        return kPad + static_cast<int>(std::clamp(f, 0.0, 1.0) * area);
    }
    Tick tickForX(int x) const
    {
        const int area = std::max(1, width() - 2 * kPad);
        return static_cast<Tick>(std::clamp(static_cast<double>(x - kPad) / area, 0.0, 1.0) * m_duration);
    }
    int propAtY(int y) const
    {
        for (int i = 0; i < kFxPropCount; ++i) {
            const int ly = laneY(static_cast<FxProp>(i));
            if (ly >= 0 && std::abs(ly - y) <= 9) {
                return i;
            }
        }
        return -1;
    }
    int hitDiamond(int prop, int x) const
    {
        for (std::size_t k = 0; k < m_keys[prop].size(); ++k) {
            if (std::abs(xForTick(m_keys[prop][k]) - x) <= kGrab) {
                return static_cast<int>(k);
            }
        }
        return -1;
    }

    DragValue* const* m_values;
    std::vector<Tick> m_keys[kFxPropCount];
    Tick m_clipStart = 0, m_duration = 0, m_playheadLocal = 0;

    std::set<std::pair<int, Tick>> m_selected;
    bool m_rulerScrub = false;
    bool m_banding = false;
    bool m_moving = false;
    QPoint m_bandStart, m_bandCur;
    int m_movePressX = 0;
    Tick m_moveDelta = 0;
};

void KeyframePane::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 21, 24));
    const int px = xForTick(m_playheadLocal);

    // Ruler with clip-relative time labels (starts at 0 regardless of timeline position).
    if (m_duration > 0) {
        const double durSec = secondsFromTicks(m_duration);
        const double pxPerSec = static_cast<double>(width() - 2 * kPad) / std::max(durSec, 1e-6);
        const double step = niceInterval(pxPerSec);
        QFont f = p.font();
        f.setPointSizeF(7.0);
        p.setFont(f);
        for (double t = 0.0; t <= durSec + 1e-6; t += step) {
            const int x = xForTick(ticksFromSeconds(t));
            p.setPen(QColor(70, 72, 78));
            p.drawLine(x, kRulerH - 5, x, kRulerH - 1);
            p.setPen(QColor(150, 152, 158));
            p.drawText(x + 2, kRulerH - 6, timeLabel(t));
        }
    }
    p.setPen(QColor(60, 62, 68));
    p.drawLine(kPad, kRulerH - 1, width() - kPad, kRulerH - 1);

    // Lanes + keyframes.
    for (int i = 0; i < kFxPropCount; ++i) {
        const int y = laneY(static_cast<FxProp>(i));
        if (y < 0) {
            continue;
        }
        p.setPen(QColor(40, 42, 47));
        p.drawLine(kPad, y, width() - kPad, y);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        for (Tick t : m_keys[i]) {
            const bool sel = m_selected.count({ i, t }) > 0;
            const int dx = xForTick(m_moving && sel ? std::clamp<Tick>(t + m_moveDelta, 0, m_duration) : t);
            p.setBrush(sel ? QColor(240, 200, 90) : QColor(150, 180, 220));
            QPolygon dia;
            dia << QPoint(dx, y - 4) << QPoint(dx + 4, y) << QPoint(dx, y + 4) << QPoint(dx - 4, y);
            p.drawPolygon(dia);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // Rubber-band — match the media browser's Fusion band (from the palette highlight).
    if (m_banding) {
        const QRect r = QRect(m_bandStart, m_bandCur).normalized();
        const QColor hl = palette().color(QPalette::Highlight);
        QColor fill(std::min(hl.red() / 2 + 110, 255), std::min(hl.green() / 2 + 110, 255),
                    std::min(hl.blue() / 2 + 110, 255));
        fill.setAlpha(80);
        QColor border = hl.darker(120);
        border.setAlpha(180);
        p.setPen(border);
        p.setBrush(fill);
        p.drawRect(r);
    }

    // Playhead: continuous line (through the ruler) + a marker triangle on top.
    p.setPen(QColor(235, 80, 80));
    p.drawLine(px, 0, px, height());
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(235, 80, 80));
    QPolygon tri;
    tri << QPoint(px - 4, 0) << QPoint(px + 4, 0) << QPoint(px, 6);
    p.drawPolygon(tri);
}

void KeyframePane::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        return;
    }
    setFocus();
    const int x = e->position().toPoint().x();
    const int y = e->position().toPoint().y();

    if (y < kRulerH) {  // ruler → continuous scrub
        m_rulerScrub = true;
        if (onScrubBegin) {
            onScrubBegin();
        }
        if (onSeek) {
            onSeek(m_clipStart + tickForX(x));
        }
        return;
    }

    const int prop = propAtY(y);
    const int kidx = prop >= 0 ? hitDiamond(prop, x) : -1;
    if (kidx >= 0) {  // click a diamond → select it (if not already), then start a move
        const std::pair<int, Tick> key{ prop, m_keys[prop][kidx] };
        if (m_selected.count(key) == 0) {
            m_selected.clear();
            m_selected.insert(key);
        }
        m_moving = true;
        m_movePressX = x;
        m_moveDelta = 0;
        update();
        return;
    }

    m_banding = true;  // empty lane area → rubber-band select
    m_bandStart = e->position().toPoint();
    m_bandCur = m_bandStart;
    m_selected.clear();
    update();
}

void KeyframePane::mouseMoveEvent(QMouseEvent* e)
{
    const int x = e->position().toPoint().x();
    if (m_rulerScrub) {
        if (onSeek) {
            onSeek(m_clipStart + tickForX(x));
        }
        return;
    }
    if (m_moving) {
        m_moveDelta = tickForX(x) - tickForX(m_movePressX);
        update();
        return;
    }
    if (m_banding) {
        m_bandCur = e->position().toPoint();
        update();
    }
}

void KeyframePane::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        return;
    }
    if (m_rulerScrub) {
        m_rulerScrub = false;
        if (onScrubEnd) {
            onScrubEnd();
        }
        return;
    }
    if (m_moving) {
        m_moving = false;
        if (m_moveDelta != 0 && onEdits) {
            std::vector<KeyEdit> edits;
            for (const auto& [prop, t] : m_selected) {
                edits.push_back({ static_cast<FxProp>(prop), t, std::clamp<Tick>(t + m_moveDelta, 0, m_duration),
                                  false });
            }
            if (!edits.empty()) {
                onEdits(edits);
            }
        }
        m_moveDelta = 0;
        update();
        return;
    }
    if (m_banding) {
        m_banding = false;
        const QRect r = QRect(m_bandStart, m_bandCur).normalized();
        m_selected.clear();
        for (int i = 0; i < kFxPropCount; ++i) {
            const int ly = laneY(static_cast<FxProp>(i));
            if (ly < 0 || ly < r.top() - 5 || ly > r.bottom() + 5) {
                continue;
            }
            for (Tick t : m_keys[i]) {
                const int dx = xForTick(t);
                if (dx >= r.left() && dx <= r.right()) {
                    m_selected.insert({ i, t });
                }
            }
        }
        update();
    }
}

void KeyframePane::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && !m_selected.empty() && onEdits) {
        std::vector<KeyEdit> edits;
        for (const auto& [prop, t] : m_selected) {
            edits.push_back({ static_cast<FxProp>(prop), t, 0, true });
        }
        onEdits(edits);
        m_selected.clear();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

// ── Effect Controls panel ────────────────────────────────────────────────────
double EffectControls::toDisplay(FxProp p, double model) const
{
    switch (p) {
    case FxProp::PosX:    return m_canvasW / 2.0 + model;  // absolute canvas coord of the clip center
    case FxProp::PosY:    return m_canvasH / 2.0 + model;
    case FxProp::Scale:   return model * 100.0;
    case FxProp::Opacity: return model * 100.0;
    case FxProp::Pan:     return model * 100.0;
    default:             return model;
    }
}

double EffectControls::toModel(FxProp p, double display) const
{
    switch (p) {
    case FxProp::PosX:    return display - m_canvasW / 2.0;
    case FxProp::PosY:    return display - m_canvasH / 2.0;
    case FxProp::Scale:   return display / 100.0;
    case FxProp::Opacity: return display / 100.0;
    case FxProp::Pan:     return display / 100.0;
    default:             return display;
    }
}

double EffectControls::defaultDisplay(FxProp p) const
{
    switch (p) {
    case FxProp::PosX:    return m_canvasW / 2.0;
    case FxProp::PosY:    return m_canvasH / 2.0;
    case FxProp::Scale:   return 100.0;
    case FxProp::Opacity: return 100.0;
    default:             return 0.0;
    }
}

void EffectControls::jumpKeyframe(FxProp prop, bool next)
{
    const std::vector<Tick>& keys = m_view.keys[idx(prop)];
    if (keys.empty()) {
        return;
    }
    const Tick ph = m_view.playheadLocal;
    Tick target = -1;
    if (next) {
        for (Tick t : keys) {
            if (t > ph) {
                target = t;
                break;
            }
        }
    } else {
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
            if (*it < ph) {
                target = *it;
                break;
            }
        }
    }
    if (target >= 0) {
        emit seekRequested(m_view.clipStart + target);
    }
}

void EffectControls::addRow(QFormLayout* form, const QString& label, FxProp prop)
{
    const QColor iconColor = kFxIconColor;
    const PropCfg c = cfgFor(prop);
    auto* value = new DragValue;
    value->setRange(c.lo, c.hi);
    value->setStep(c.step);
    value->setDecimals(c.decimals);
    value->setSuffix(c.suffix);
    m_value[idx(prop)] = value;

    auto smallBtn = [](const QIcon& ic, const QString& tip) {
        auto* b = new QToolButton;
        b->setIcon(ic);
        b->setIconSize(QSize(17, 17));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };

    auto* stopwatch = smallBtn(makeIcon(Glyph::Stopwatch, iconColor), "Toggle keyframing");
    stopwatch->setCheckable(true);
    m_key[idx(prop)] = stopwatch;
    auto* prev = smallBtn(makeIcon(Glyph::ArrowLeft, iconColor), "Previous keyframe");
    auto* addKey = smallBtn(makeIcon(Glyph::Diamond, iconColor), "Add/remove keyframe at playhead");
    auto* next = smallBtn(makeIcon(Glyph::ArrowRight, iconColor), "Next keyframe");
    m_prev[idx(prop)] = prev;
    m_addKey[idx(prop)] = addKey;
    m_next[idx(prop)] = next;
    auto* reset = smallBtn(makeIcon(Glyph::Reset, iconColor), "Reset to default");

    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    h->addWidget(stopwatch);
    h->addWidget(prev);
    h->addWidget(addKey);
    h->addWidget(next);
    h->addWidget(value);
    h->addStretch();
    h->addWidget(reset);
    form->addRow(label, w);

    connect(value, &DragValue::valueChanged, this, [this, prop](double v, bool committing) {
        if (!m_populating) {
            emit propertyEdited(prop, toModel(prop, v), committing);
        }
    });
    connect(stopwatch, &QToolButton::toggled, this, [this, prop](bool on) {
        if (!m_populating) {
            emit keyframeToggled(prop, on);
        }
    });
    connect(reset, &QToolButton::clicked, this, [this, prop] {
        m_value[idx(prop)]->setValue(defaultDisplay(prop));
        emit propertyEdited(prop, toModel(prop, defaultDisplay(prop)), true);
    });
    connect(prev, &QToolButton::clicked, this, [this, prop] { jumpKeyframe(prop, false); });
    connect(next, &QToolButton::clicked, this, [this, prop] { jumpKeyframe(prop, true); });
    connect(addKey, &QToolButton::clicked, this, [this, prop] { toggleKeyAtPlayhead(prop); });
}

double EffectControls::modelValueAt(FxProp prop) const
{
    switch (prop) {
    case FxProp::PosX:     return m_view.posX;
    case FxProp::PosY:     return m_view.posY;
    case FxProp::Scale:    return m_view.scale;
    case FxProp::Rotation: return m_view.rotation;
    case FxProp::Opacity:  return m_view.opacity;
    case FxProp::VolumeDb: return m_view.volumeDb;
    case FxProp::Pan:      return m_view.pan;
    }
    return 0.0;
}

void EffectControls::toggleKeyAtPlayhead(FxProp prop)
{
    const std::vector<Tick>& keys = m_view.keys[idx(prop)];
    if (keys.empty()) {
        emit keyframeToggled(prop, true);  // not animated yet → enable, seeding the first key
        return;
    }
    const Tick ph = m_view.playheadLocal;
    for (Tick t : keys) {
        if (t == ph) {  // sitting on a key → remove it
            emit keyframesEdited(std::vector<KeyEdit>{ { prop, ph, 0, true } });
            return;
        }
    }
    emit propertyEdited(prop, modelValueAt(prop), true);  // add a key holding the current value
}

EffectControls::EffectControls(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    // Draggable split: property controls (left) | keyframe pane (right).
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);

    auto* left = new QWidget;
    auto* v = new QVBoxLayout(left);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);

    m_placeholder = new QLabel("Select a clip to edit its effects.", left);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setStyleSheet("color: #7a7c82;");
    v->addWidget(m_placeholder);

    m_transformBox = new QGroupBox("Transform", left);
    auto* tf = new QFormLayout(m_transformBox);
    addRow(tf, "Position X", FxProp::PosX);
    addRow(tf, "Position Y", FxProp::PosY);
    addRow(tf, "Scale", FxProp::Scale);
    addRow(tf, "Rotation", FxProp::Rotation);
    addRow(tf, "Opacity", FxProp::Opacity);
    m_blend = new QComboBox;  // order must match the BlendMode enum
    m_blend->addItems({ "Normal", "Add", "Screen", "Multiply", "Overlay", "Darken", "Color Burn",
                        "Lighten", "Color Dodge", "Soft Light", "Hard Light", "Difference",
                        "Exclusion", "Subtract" });
    tf->addRow("Blend Mode", m_blend);
    connect(m_blend, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (!m_populating) {
            emit blendEdited(static_cast<BlendMode>(i));
        }
    });
    v->addWidget(m_transformBox);

    m_audioBox = new QGroupBox("Volume Controls", left);
    auto* av = new QFormLayout(m_audioBox);
    addRow(av, "Volume", FxProp::VolumeDb);
    addRow(av, "Pan (L/R)", FxProp::Pan);
    m_value[idx(FxProp::Pan)]->setToolTip("-100 = full left, +100 = full right");
    v->addWidget(m_audioBox);
    v->addStretch();

    m_pane = new KeyframePane(m_value, splitter);
    m_pane->onSeek = [this](Tick t) { emit seekRequested(t); };
    m_pane->onScrubBegin = [this] { emit scrubBegin(); };
    m_pane->onScrubEnd = [this] { emit scrubEnd(); };
    m_pane->onEdits = [this](const std::vector<KeyEdit>& edits) { emit keyframesEdited(edits); };

    splitter->addWidget(left);
    splitter->addWidget(m_pane);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 300, 300 });  // even effects/keyframes split
    scroll->setWidget(splitter);
    showNone();
}

void EffectControls::showNone()
{
    m_placeholder->setText("Select a clip to edit its effects.");
    m_placeholder->show();
    m_transformBox->hide();
    m_audioBox->hide();
    m_pane->hide();
}

void EffectControls::showMultiple()
{
    m_placeholder->setText("Multiple clips selected.\nSelect a single clip to edit its effects.");
    m_placeholder->show();
    m_transformBox->hide();
    m_audioBox->hide();
    m_pane->hide();
}

void EffectControls::showClip(const FxView& view)
{
    m_view = view;
    m_canvasW = view.canvasW;
    m_canvasH = view.canvasH;
    m_populating = true;

    auto setNav = [this, &view](FxProp p, bool animated) {
        m_prev[idx(p)]->setEnabled(animated);
        m_next[idx(p)]->setEnabled(animated);
        m_key[idx(p)]->setIcon(makeIcon(Glyph::Stopwatch, animated ? kFxIconActive : kFxIconColor));
        bool onKey = false;
        for (Tick t : view.keys[idx(p)]) {
            if (t == view.playheadLocal) {
                onKey = true;
                break;
            }
        }
        m_addKey[idx(p)]->setIcon(makeIcon(onKey ? Glyph::DiamondFilled : Glyph::Diamond, kFxIconColor));
    };

    if (view.hasVideo) {
        m_value[idx(FxProp::PosX)]->setValue(toDisplay(FxProp::PosX, view.posX));
        m_value[idx(FxProp::PosY)]->setValue(toDisplay(FxProp::PosY, view.posY));
        m_value[idx(FxProp::Scale)]->setValue(toDisplay(FxProp::Scale, view.scale));
        m_value[idx(FxProp::Rotation)]->setValue(toDisplay(FxProp::Rotation, view.rotation));
        m_value[idx(FxProp::Opacity)]->setValue(toDisplay(FxProp::Opacity, view.opacity));
        m_blend->setCurrentIndex(static_cast<int>(view.blend));
        for (FxProp p : { FxProp::PosX, FxProp::PosY, FxProp::Scale, FxProp::Rotation, FxProp::Opacity }) {
            m_key[idx(p)]->setChecked(view.anim[idx(p)]);
            setNav(p, view.anim[idx(p)]);
        }
    }
    if (view.hasAudio) {
        m_value[idx(FxProp::VolumeDb)]->setValue(toDisplay(FxProp::VolumeDb, view.volumeDb));
        m_value[idx(FxProp::Pan)]->setValue(toDisplay(FxProp::Pan, view.pan));
        for (FxProp p : { FxProp::VolumeDb, FxProp::Pan }) {
            m_key[idx(p)]->setChecked(view.anim[idx(p)]);
            setNav(p, view.anim[idx(p)]);
        }
    }

    m_populating = false;
    m_transformBox->setVisible(view.hasVideo);
    m_audioBox->setVisible(view.hasAudio);
    m_placeholder->setVisible(!view.hasVideo && !view.hasAudio);
    m_pane->setVisible(view.hasVideo || view.hasAudio);
    m_pane->setData(view);
}

}  // namespace hopline
