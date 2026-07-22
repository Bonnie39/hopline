#include "app/AudioMeter.h"

#include <algorithm>
#include <cmath>

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace hopline {
namespace {

constexpr float kDecay = 0.90f;  // per UI tick (~4ms), snappy but visible falloff
constexpr int kMaxBarsWidth = 54;

// Linear peak -> 0..1 bar fraction on a -60..0 dB scale.
float toFraction(float lin)
{
    if (lin <= 1e-4f) return 0.0f;
    const float db = 20.0f * std::log10(lin);
    return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}

}  // namespace

AudioMeter::AudioMeter(QWidget* parent)
    : QWidget(parent)
{
    // The bars cap at kMaxBarsWidth, so the meter never needs to be wide. Capping the
    // width also stops it absorbing horizontal slack as the corner dock on each
    // restore — which was growing it a little on every relaunch — and clamps any
    // already-bloated saved layout back down.
    setMinimumWidth(44);
    setMaximumWidth(72);
}

QSize AudioMeter::sizeHint() const { return { 60, 220 }; }

void AudioMeter::setLevels(float left, float right)
{
    m_left = std::max(left, m_left * kDecay);
    m_right = std::max(right, m_right * kDecay);
    update();
}

void AudioMeter::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(24, 25, 27));

    const int pad = 8;
    const int labelH = 12;
    const int top = pad;
    const int bottom = height() - pad - labelH;
    if (bottom <= top) return;

    const int gap = 6;
    const int areaW = std::min(width() - 2 * pad, kMaxBarsWidth);
    const int barW = (areaW - gap) / 2;
    if (barW < 2) return;
    const int left = (width() - (2 * barW + gap)) / 2;

    const auto drawBar = [&](int x, float level, const QString& tag) {
        const QRect track(x, top, barW, bottom - top);
        QPainterPath clip;
        clip.addRoundedRect(track, 3, 3);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(40, 42, 47));
        painter.drawPath(clip);

        const float frac = toFraction(level);
        const int h = static_cast<int>(frac * track.height());
        if (h > 0) {
            QLinearGradient g(0, track.bottom(), 0, track.top());
            g.setColorAt(0.0, QColor(80, 200, 120));
            g.setColorAt(0.65, QColor(120, 205, 100));
            g.setColorAt(0.85, QColor(230, 200, 80));
            g.setColorAt(1.0, QColor(230, 90, 80));
            painter.save();
            painter.setClipPath(clip);
            painter.fillRect(QRect(track.left(), track.bottom() - h, track.width(), h), g);
            painter.restore();
        }

        painter.setPen(QColor(150, 150, 150));
        QFont f = painter.font();
        f.setPixelSize(10);
        painter.setFont(f);
        painter.drawText(QRect(x, bottom + 2, barW, labelH), Qt::AlignCenter, tag);
    };

    drawBar(left, m_left, "L");
    drawBar(left + barW + gap, m_right, "R");
}

}  // namespace hopline
