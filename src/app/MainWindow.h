#pragma once

#include <QMainWindow>
#include <memory>
#include <vector>

#include "engine/Player.h"
#include "model/Command.h"
#include "model/Project.h"

class QPlainTextEdit;
class QLabel;
class QTimer;
class QDockWidget;
class QTabBar;

namespace hopline {

class PreviewWidget;
class PreviewCache;
class TimelineWidget;
class MediaBrowser;
class IconButton;
class AudioMeter;
class ToolboxWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void load(const QString& path);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private slots:
    void resetLayout();
    void showMediaInfo(MediaId media);
    void newProject();
    void openProject();
    void saveProject();
    void saveProjectAs();
    void importMediaDialog(FolderId folder);
    void onNewFolder(FolderId parent);
    void onDeleteFolder(FolderId folder);
    void onDeleteMedia(QList<MediaId> media);
    void onNewSequence(FolderId folder);
    void onSequenceActivated(SequenceId sequence);
    void onDeleteSequence(SequenceId sequence);
    void onMediaDropped(MediaId media, Tick start, int level);
    void onFileDropped(const QString& path, Tick start, int level);
    void onAddTrack(bool video);
    void onDeleteTrack(std::size_t trackIndex);
    void togglePlay();
    void tick();
    void seekRelative(double seconds);
    void timelineScrubbed(Tick time);

    void onClipMoved(std::size_t fromTrack, ClipId clip, int levelDelta, Tick newStart);
    void onClipTrimmed(std::size_t trackIndex, ClipId clip, bool trimHead, Tick delta);
    void onUnlink(ClipId clip);
    void onClipLabel(std::size_t trackIndex, ClipId clip, int label);
    void onDeleteClip(std::size_t trackIndex, ClipId clip);
    void undo();
    void redo();
    void deleteSelection();

private:
    QPlainTextEdit* m_log = nullptr;
    PreviewWidget* m_preview = nullptr;
    void commitEdit();
    void applyDefaultLayout();
    void reopenPlayer();
    MediaId importMedia(const QString& path, FolderId folder);
    void placeMedia(MediaId media, Tick start, int level = 0);
    bool mediaInUse(MediaId id) const;
    void activateSequence(SequenceId sequence);
    SequenceId ensureActiveSequence(const MediaSource& source);
    void seedTracks(SequenceId sequence);  // pad a new sequence to 3 video + 3 audio tracks
    int trackIndexForLevel(bool video, int level);
    void ensureTrackLevel(bool video, int level);
    void syncSequenceTabs();
    void closeSequenceTab(int index);
    void refreshPreviewsForProject();

    QTimer* m_timer = nullptr;
    IconButton* m_playButton = nullptr;
    QLabel* m_timeLabel = nullptr;
    TimelineWidget* m_timeline = nullptr;
    QTabBar* m_seqTabs = nullptr;
    std::vector<SequenceId> m_openSequences;
    bool m_syncingTabs = false;
    MediaBrowser* m_browser = nullptr;
    AudioMeter* m_meter = nullptr;
    ToolboxWidget* m_toolbox = nullptr;
    QDockWidget* m_browserDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_logDock = nullptr;
    QDockWidget* m_meterDock = nullptr;
    QDockWidget* m_toolsDock = nullptr;
    std::unique_ptr<Player> m_player;
    std::unique_ptr<PreviewCache> m_previews;
    Project m_project;
    CommandStack m_commands;
    QString m_projectPath;
    bool m_layoutRestored = false;
    bool m_wasPlaying = false;
};

}  // namespace hopline
