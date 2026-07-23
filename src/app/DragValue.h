#pragma once

#include <QString>
#include <QWidget>

class QLineEdit;

namespace hopline {

// A Premiere-style "scrubby" number field: the value reads as a highlighted number
// you drag left/right to change, or click to type into (like the inline folder
// rename). Emits valueChanged once per interaction (drag release or text commit),
// never per drag pixel, so each edit is a single command/undo step.
class DragValue : public QWidget {
    Q_OBJECT

public:
    explicit DragValue(QWidget* parent = nullptr);

    void setRange(double lo, double hi);
    void setDecimals(int decimals);
    void setSuffix(const QString& suffix);
    void setStep(double perPixel);       // value change per pixel dragged
    void setValue(double value);         // programmatic; no signal
    double value() const { return m_value; }

signals:
    // `committing` is false for the live values emitted while dragging, true for the
    // final value on drag release or text commit — so the model can preview live but
    // record a single undo step.
    void valueChanged(double value, bool committing);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    void beginEdit();
    void commitText();
    QString displayText() const;
    double clamp(double v) const;

    double m_value = 0.0;
    double m_min = -1e9;
    double m_max = 1e9;
    double m_step = 1.0;
    int m_decimals = 1;
    QString m_suffix;

    QLineEdit* m_editor = nullptr;
    bool m_pressed = false;
    bool m_dragged = false;
    QPoint m_pressPos;
    double m_pressValue = 0.0;
};

}  // namespace hopline
