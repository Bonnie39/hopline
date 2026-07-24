#include "app/EffectControls.h"

#include <functional>

#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "app/DragValue.h"

namespace hopline {
namespace {

DragValue* makeValue(double lo, double hi, double step, int decimals, const QString& suffix)
{
    auto* v = new DragValue;
    v->setRange(lo, hi);
    v->setStep(step);
    v->setDecimals(decimals);
    v->setSuffix(suffix);
    return v;
}

}  // namespace

EffectControls::EffectControls(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(8);

    m_placeholder = new QLabel("Select a clip to edit its effects.", content);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setStyleSheet("color: #7a7c82;");
    v->addWidget(m_placeholder);

    // Transform (video clips). Position is the clip center in canvas coordinates.
    m_transformBox = new QGroupBox("Transform", content);
    auto* tf = new QFormLayout(m_transformBox);
    m_posX = makeValue(-40000.0, 40000.0, 1.0, 1, " px");
    m_posY = makeValue(-40000.0, 40000.0, 1.0, 1, " px");
    m_scale = makeValue(1.0, 1000.0, 0.5, 1, "%");
    m_rotation = makeValue(-3600.0, 3600.0, 0.5, 1, "°");
    m_opacity = makeValue(0.0, 100.0, 0.5, 1, "%");
    m_blend = new QComboBox;  // order must match the BlendMode enum
    m_blend->addItems({ "Normal", "Add", "Screen", "Multiply", "Overlay", "Darken", "Color Burn",
                        "Lighten", "Color Dodge", "Soft Light", "Hard Light", "Difference",
                        "Exclusion", "Subtract" });

    auto* pos = new QWidget;  // Position is an X + Y pair on one row
    auto* posRow = new QHBoxLayout(pos);
    posRow->setContentsMargins(0, 0, 0, 0);
    posRow->setSpacing(10);
    posRow->addWidget(m_posX);
    posRow->addWidget(m_posY);

    // Each row gets a reset-to-default button on the far right (blend mode excepted).
    auto resettable = [this](QWidget* field, std::function<void()> onReset) -> QWidget* {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        h->addWidget(field);
        h->addStretch();
        auto* reset = new QToolButton;
        reset->setText(QString::fromUtf8("\xE2\x86\xBA"));  // ↺
        reset->setToolTip("Reset to default");
        reset->setAutoRaise(true);
        reset->setFocusPolicy(Qt::NoFocus);
        connect(reset, &QToolButton::clicked, this, onReset);
        h->addWidget(reset);
        return w;
    };

    tf->addRow("Position", resettable(pos, [this] {
        m_posX->setValue(m_canvasW / 2.0);
        m_posY->setValue(m_canvasH / 2.0);
        emitTransform(true);
    }));
    tf->addRow("Scale", resettable(m_scale, [this] { m_scale->setValue(100.0); emitTransform(true); }));
    tf->addRow("Rotation", resettable(m_rotation, [this] { m_rotation->setValue(0.0); emitTransform(true); }));
    tf->addRow("Opacity", resettable(m_opacity, [this] { m_opacity->setValue(100.0); emitTransform(true); }));
    tf->addRow("Blend Mode", m_blend);
    v->addWidget(m_transformBox);

    for (DragValue* box : { m_posX, m_posY, m_scale, m_rotation, m_opacity }) {
        connect(box, &DragValue::valueChanged, this,
                [this](double, bool committing) { emitTransform(committing); });
    }
    connect(m_blend, &QComboBox::currentIndexChanged, this, [this](int) { emitTransform(true); });

    // Volume Controls (audio clips).
    m_audioBox = new QGroupBox("Volume Controls", content);
    auto* av = new QFormLayout(m_audioBox);
    m_volume = makeValue(-60.0, 12.0, 0.1, 1, " dB");
    m_pan = makeValue(-100.0, 100.0, 1.0, 0, "");
    m_pan->setToolTip("-100 = full left, +100 = full right");
    av->addRow("Volume", resettable(m_volume, [this] { m_volume->setValue(0.0); emitAudio(true); }));
    av->addRow("Pan (L/R)", resettable(m_pan, [this] { m_pan->setValue(0.0); emitAudio(true); }));
    v->addWidget(m_audioBox);

    for (DragValue* box : { m_volume, m_pan }) {
        connect(box, &DragValue::valueChanged, this,
                [this](double, bool committing) { emitAudio(committing); });
    }

    v->addStretch();
    scroll->setWidget(content);

    showNone();
}

void EffectControls::showNone()
{
    m_placeholder->show();
    m_transformBox->hide();
    m_audioBox->hide();
}

void EffectControls::showClip(const Clip* video, const Clip* audio, int canvasW, int canvasH)
{
    m_canvasW = canvasW;
    m_canvasH = canvasH;
    m_populating = true;

    if (video) {
        const Transform& t = video->transform;
        m_posX->setValue(canvasW / 2.0 + t.posX);  // absolute center-of-clip on the canvas
        m_posY->setValue(canvasH / 2.0 + t.posY);
        m_scale->setValue(t.scale * 100.0);
        m_rotation->setValue(t.rotation);
        m_opacity->setValue(t.opacity * 100.0);
        m_blend->setCurrentIndex(static_cast<int>(t.blend));
    }
    if (audio) {
        m_volume->setValue(audio->audio.volumeDb);
        m_pan->setValue(audio->audio.pan * 100.0);
    }

    m_populating = false;
    m_transformBox->setVisible(video != nullptr);
    m_audioBox->setVisible(audio != nullptr);
    m_placeholder->setVisible(!video && !audio);
}

void EffectControls::emitTransform(bool committing)
{
    if (m_populating) {
        return;
    }
    Transform t;
    t.posX = m_posX->value() - m_canvasW / 2.0;  // back to offset-from-center for the engine
    t.posY = m_posY->value() - m_canvasH / 2.0;
    t.scale = m_scale->value() / 100.0;
    t.rotation = m_rotation->value();
    t.opacity = m_opacity->value() / 100.0;
    t.blend = static_cast<BlendMode>(m_blend->currentIndex());
    emit transformEdited(t, committing);
}

void EffectControls::emitAudio(bool committing)
{
    if (m_populating) {
        return;
    }
    AudioLevels a;
    a.volumeDb = m_volume->value();
    a.pan = m_pan->value() / 100.0;
    emit audioEdited(a, committing);
}

}  // namespace hopline
