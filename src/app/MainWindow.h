#pragma once

#include <QMainWindow>
#include <memory>

#include "engine/Player.h"
#include "model/Command.h"
#include "model/Project.h"

class QPlainTextEdit;
class QLabel;
class QTimer;
class QDockWidget;

namespace hopline {

class PreviewWidget;
class PreviewCache;
class TimelineWidget;
class MediaBrowser;
class IconButton;

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
    void onMediaDropped(MediaId media, Tick start);
    void onFileDropped(const QString& path, Tick start);
    void togglePlay();
    void tick();
    void seekRelative(double seconds);
    void timelineScrubbed(Tick time);

    void onClipMoved(std::size_t trackIndex, ClipId clip, Tick newStart);
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
    void placeMedia(MediaId media, Tick start);
    bool mediaInUse(MediaId id) const;
    void refreshPreviewsForProject();

    QTimer* m_timer = nullptr;
    IconButton* m_playButton = nullptr;
    QLabel* m_timeLabel = nullptr;
    TimelineWidget* m_timeline = nullptr;
    MediaBrowser* m_browser = nullptr;
    QDockWidget* m_browserDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_logDock = nullptr;
    std::unique_ptr<Player> m_player;
    std::unique_ptr<PreviewCache> m_previews;
    Project m_project;
    CommandStack m_commands;
    QString m_projectPath;
    bool m_layoutRestored = false;
    bool m_wasPlaying = false;
};

}  // namespace hopline
