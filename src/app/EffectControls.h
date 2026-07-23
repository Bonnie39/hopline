#pragma once

#include <QWidget>

#include "model/Clip.h"

class QComboBox;
class QGroupBox;
class QLabel;

namespace hopline {

class DragValue;

// The Effect Controls panel. Shows the selected clip's default effects — Transform
// for a video clip, Volume Controls for an audio clip, and BOTH when a linked V+A
// pair is selected. A control change emits the whole edited struct; MainWindow issues
// the command against the group's matching (video/audio) clip. Never touches the model.
class EffectControls : public QWidget {
    Q_OBJECT

public:
    explicit EffectControls(QWidget* parent = nullptr);

    // Pass whichever halves exist (nullptr to hide that section). canvas size lets
    // Position read as absolute canvas coordinates of the clip center.
    void showClip(const Clip* video, const Clip* audio, int canvasW, int canvasH);
    void showNone();

signals:
    // `committing` false = live drag preview; true = final value (drag release / typed).
    void transformEdited(const Transform& transform, bool committing);
    void audioEdited(const AudioLevels& audio, bool committing);

private:
    void emitTransform(bool committing);
    void emitAudio(bool committing);

    bool m_populating = false;  // suppress signals while loading values
    int m_canvasW = 0;
    int m_canvasH = 0;

    QLabel* m_placeholder = nullptr;
    QGroupBox* m_transformBox = nullptr;
    QGroupBox* m_audioBox = nullptr;

    DragValue* m_posX = nullptr;
    DragValue* m_posY = nullptr;
    DragValue* m_scale = nullptr;
    DragValue* m_rotation = nullptr;
    DragValue* m_opacity = nullptr;
    QComboBox* m_blend = nullptr;

    DragValue* m_volume = nullptr;
    DragValue* m_pan = nullptr;
};

}  // namespace hopline
