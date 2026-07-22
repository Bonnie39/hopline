#include "app/SequenceDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QSize>
#include <QSpinBox>
#include <QVBoxLayout>

namespace hopline {

SequenceDialog::SequenceDialog(const QString& defaultName, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("New Sequence");
    setModal(true);

    m_name = new QLineEdit(defaultName, this);
    m_name->selectAll();

    m_resolution = new QComboBox(this);
    struct Res {
        const char* label;
        int w, h;
    };
    const Res presets[] = {
        { "1920 × 1080 — HD", 1920, 1080 }, { "1280 × 720 — HD", 1280, 720 },
        { "3840 × 2160 — UHD", 3840, 2160 }, { "2560 × 1440 — QHD", 2560, 1440 },
        { "1080 × 1920 — Vertical", 1080, 1920 }, { "Custom…", 0, 0 },
    };
    for (const Res& r : presets) {
        m_resolution->addItem(r.label, QSize(r.w, r.h));
    }

    m_width = new QSpinBox(this);
    m_width->setRange(16, 16384);
    m_width->setValue(1920);
    m_height = new QSpinBox(this);
    m_height->setRange(16, 16384);
    m_height->setValue(1080);

    m_rate = new QComboBox(this);
    struct Rate {
        const char* label;
        int num, den;
    };
    const Rate rates[] = {
        { "23.976", 24000, 1001 }, { "24", 24, 1 }, { "25", 25, 1 }, { "29.97", 30000, 1001 },
        { "30", 30, 1 }, { "50", 50, 1 }, { "59.94", 60000, 1001 }, { "60", 60, 1 },
    };
    for (const Rate& r : rates) {
        m_rate->addItem(r.label, QPoint(r.num, r.den));
    }
    m_rate->setCurrentIndex(4);  // 30 fps

    auto* customRow = new QWidget(this);
    auto* customLayout = new QHBoxLayout(customRow);
    customLayout->setContentsMargins(0, 0, 0, 0);
    customLayout->addWidget(m_width);
    customLayout->addWidget(new QLabel("×", this));
    customLayout->addWidget(m_height);
    customLayout->addStretch(1);

    auto* form = new QFormLayout;
    form->addRow("Name", m_name);
    form->addRow("Resolution", m_resolution);
    form->addRow("Custom size", customRow);
    form->addRow("Frame rate", m_rate);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_resolution, &QComboBox::currentIndexChanged, this, &SequenceDialog::syncCustomEnabled);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    syncCustomEnabled();
}

void SequenceDialog::syncCustomEnabled()
{
    const QSize preset = m_resolution->currentData().toSize();
    const bool custom = preset.width() == 0;
    m_width->setEnabled(custom);
    m_height->setEnabled(custom);
    if (!custom) {
        m_width->setValue(preset.width());
        m_height->setValue(preset.height());
    }
}

SequenceSettings SequenceDialog::settings() const
{
    SequenceSettings s;
    s.name = m_name->text().trimmed().isEmpty() ? QStringLiteral("Sequence") : m_name->text().trimmed();
    s.width = m_width->value();
    s.height = m_height->value();
    const QPoint r = m_rate->currentData().toPoint();
    s.rateNum = r.x();
    s.rateDen = r.y();
    return s;
}

}  // namespace hopline
