#pragma once

#include <QWidget>

namespace hopline {

// Vertical stereo peak meter. Fed linear peak levels each UI tick; holds and
// decays them internally for meter-style ballistics.
class AudioMeter : public QWidget {
    Q_OBJECT

public:
    explicit AudioMeter(QWidget* parent = nullptr);

    void setLevels(float left, float right);  // linear peaks, 0..1

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float m_left = 0.0f;
    float m_right = 0.0f;
};

}  // namespace hopline
