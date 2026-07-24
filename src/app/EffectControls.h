#pragma once

#include <QWidget>

#include <vector>

#include "model/Clip.h"

class QComboBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QToolButton;

namespace hopline {

class DragValue;
class KeyframePane;

// A keyframable effect property. Order matches the array indices used internally.
enum class FxProp { PosX, PosY, Scale, Rotation, Opacity, VolumeDb, Pan };
inline constexpr int kFxPropCount = 7;

// Resolved-at-playhead values (model units) + per-property "is animated" flags + the
// keyframe times for the right-hand keyframe pane. Position is an offset-from-center;
// the panel shows it as an absolute canvas coordinate.
struct FxView {
    bool hasVideo = false;
    bool hasAudio = false;
    int canvasW = 0;
    int canvasH = 0;
    double posX = 0.0, posY = 0.0, scale = 1.0, rotation = 0.0, opacity = 1.0;
    BlendMode blend = BlendMode::Normal;
    double volumeDb = 0.0, pan = 0.0;
    bool anim[kFxPropCount] = {};

    // Keyframe region: the time axis and per-property key times (clip-local ticks).
    Tick clipStart = 0;       // primary clip's timelineStart (maps lane time → timeline)
    Tick clipDuration = 0;    // time-axis length
    Tick playheadLocal = 0;   // playhead - clipStart
    std::vector<Tick> keys[kFxPropCount];
};

// One keyframe move (remove=false) or delete (remove=true), clip-local ticks.
struct KeyEdit {
    FxProp prop;
    Tick oldTime;
    Tick newTime;
    bool remove;
};

// The Effect Controls panel. Emits per-property edits + keyframe toggles; MainWindow
// turns them into commands (setting a constant, or a keyframe at the playhead). Never
// touches the model. Values displayed follow the playhead (MainWindow re-feeds them).
class EffectControls : public QWidget {
    Q_OBJECT

public:
    explicit EffectControls(QWidget* parent = nullptr);

    void showClip(const FxView& view);
    void showNone();

signals:
    // committing: false = live drag preview, true = final (drag release / typed / reset).
    void propertyEdited(FxProp prop, double modelValue, bool committing);
    void keyframeToggled(FxProp prop, bool enabled);
    void blendEdited(BlendMode blend);
    // From the keyframe pane / nav arrows.
    void seekRequested(Tick timelineTime);   // ruler-scrub move + prev/next-key jump
    void scrubBegin();
    void scrubEnd();
    void keyframesEdited(const std::vector<KeyEdit>& edits);  // move/delete, one undo step

private:
    void addRow(QFormLayout* form, const QString& label, FxProp prop);
    double toDisplay(FxProp prop, double model) const;
    double toModel(FxProp prop, double display) const;
    double defaultDisplay(FxProp prop) const;
    void jumpKeyframe(FxProp prop, bool next);  // move playhead to the prev/next key of `prop`
    void toggleKeyAtPlayhead(FxProp prop);      // add a key at the playhead, or remove the one there
    double modelValueAt(FxProp prop) const;     // the shown clip's model value for `prop` at the playhead

    bool m_populating = false;
    int m_canvasW = 0;
    int m_canvasH = 0;
    FxView m_view;  // last shown, for the nav arrows

    QLabel* m_placeholder = nullptr;
    QGroupBox* m_transformBox = nullptr;
    QGroupBox* m_audioBox = nullptr;
    QComboBox* m_blend = nullptr;
    KeyframePane* m_pane = nullptr;

    DragValue* m_value[kFxPropCount] = {};
    QToolButton* m_key[kFxPropCount] = {};    // stopwatch: keyframing on/off
    QToolButton* m_prev[kFxPropCount] = {};   // jump to previous keyframe
    QToolButton* m_addKey[kFxPropCount] = {}; // add/remove a keyframe at the playhead
    QToolButton* m_next[kFxPropCount] = {};   // jump to next keyframe
};

}  // namespace hopline
