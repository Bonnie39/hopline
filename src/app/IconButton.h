#pragma once

#include <QAbstractButton>

namespace hopline {

// Flat transport button that paints a crisp vector glyph and a full rounded
// hover fill itself — no raster icons (which scale blurry) and none of the
// QToolButton/QPushButton icon-alignment quirks.
class IconButton : public QAbstractButton {
public:
    enum class Glyph { SkipBack, Rewind, Play, Pause, Forward };

    explicit IconButton(Glyph glyph, QWidget* parent = nullptr);

    void setGlyph(Glyph glyph);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    Glyph m_glyph;
};

}  // namespace hopline
