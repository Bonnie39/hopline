#include "app/MainWindow.h"

#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QSettings>
#include <QShowEvent>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "app/AudioMeter.h"
#include "app/IconButton.h"
#include "app/MediaBrowser.h"
#include "app/PreviewCache.h"
#include "app/PreviewWidget.h"
#include "app/TimelineWidget.h"
#include "app/ToolboxWidget.h"
#include "media/MediaProbe.h"
#include "model/Commands.h"
#include "model/ProjectIO.h"

namespace hopline {
namespace {

// Bump when the dock set or default arrangement changes, so a stale saved layout
// is discarded.
constexpr int kLayoutVersion = 5;

// Transport time readout: M:SS.CC.
QString formatClock(double seconds)
{
    const int total = static_cast<int>(seconds);
    const int cs = static_cast<int>((seconds - total) * 100);
    return QString("%1:%2.%3").arg(total / 60).arg(total % 60, 2, 10, QChar('0')).arg(cs, 2, 10, QChar('0'));
}

QString formatDuration(double seconds)
{
    const int total = static_cast<int>(seconds);
    return QString("%1:%2:%3")
        .arg(total / 3600, 2, 10, QChar('0'))
        .arg((total / 60) % 60, 2, 10, QChar('0'))
        .arg(total % 60, 2, 10, QChar('0'));
}

MediaSource toMediaSource(const MediaInfo& info)
{
    MediaSource source;
    source.path = info.path;
    source.duration = ticksFromSeconds(info.duration);

    for (const StreamInfo& stream : info.streams) {
        if (stream.type == "video" && !source.hasVideo) {
            source.hasVideo = true;
            source.width = stream.width;
            source.height = stream.height;
            source.rateNum = stream.rateNum;
            source.rateDen = stream.rateDen;
        } else if (stream.type == "audio" && !source.hasAudio) {
            source.hasAudio = true;
            source.sampleRate = stream.sampleRate;
            source.channels = stream.channels;
        }
    }
    return source;
}

// Media Info pane content for the clip selected in the bin.
QString describeMedia(const MediaSource& media)
{
    const double seconds = secondsFromTicks(media.duration);
    QString text;
    text += QString("%1\n\n").arg(QFileInfo(QString::fromStdString(media.path)).fileName());
    text += QString("path      %1\n").arg(QString::fromStdString(media.path));
    text += QString("duration  %1 (%2s)\n").arg(formatDuration(seconds)).arg(seconds, 0, 'f', 3);
    if (media.hasVideo) {
        const double fps = media.rateDen > 0 ? static_cast<double>(media.rateNum) / media.rateDen : 0.0;
        text += QString("\nvideo     %1x%2  %3 fps\n").arg(media.width).arg(media.height).arg(fps, 0, 'f', 3);
    }
    if (media.hasAudio) {
        text += QString("\naudio     %1 Hz  %2 ch\n").arg(media.sampleRate).arg(media.channels);
    }
    if (media.label > 0) {
        text += QString("\nlabel     %1\n").arg(labelName(media.label));
    }
    return text;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("hopline");
    resize(1600, 900);

    m_preview = new PreviewWidget(this);

    // Transport controls under the preview (scrubbing lives on the timeline).
    auto* controls = new QWidget(this);
    controls->setStyleSheet("QLabel { color: #c8c8c8; }");
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(8, 6, 8, 6);
    controlsLayout->setSpacing(4);
    controlsLayout->addStretch();

    auto addButton = [&](IconButton::Glyph glyph, const QString& tip, auto handler) {
        auto* button = new IconButton(glyph, controls);
        button->setToolTip(tip);
        connect(button, &QAbstractButton::clicked, this, handler);
        controlsLayout->addWidget(button);
        return button;
    };

    addButton(IconButton::Glyph::SkipBack, "Restart (Home)", [this] { seekRelative(-1e9); });
    addButton(IconButton::Glyph::Rewind, "Back 5s (Left)", [this] { seekRelative(-5.0); });
    m_playButton = addButton(IconButton::Glyph::Play, "Play / Pause (Space)", [this] { togglePlay(); });
    addButton(IconButton::Glyph::Forward, "Forward 5s (Right)", [this] { seekRelative(5.0); });

    controlsLayout->addSpacing(12);
    m_timeLabel = new QLabel("0:00.00 / 0:00.00", controls);
    controlsLayout->addWidget(m_timeLabel);
    controlsLayout->addStretch();

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_preview, 1);
    layout->addWidget(controls);
    setCentralWidget(central);

    m_previews = std::make_unique<PreviewCache>();
    connect(m_previews.get(), &PreviewCache::ready, this, [this](MediaId) {
        m_timeline->update();
        if (m_browser) {
            m_browser->refresh();  // thumbnails become bin icons once ready
        }
    });

    m_timeline = new TimelineWidget(this);
    m_timeline->setProject(&m_project);
    m_timeline->setPreviewCache(m_previews.get());
    connect(m_timeline, &TimelineWidget::playheadDragged, this, &MainWindow::timelineScrubbed);
    connect(m_timeline, &TimelineWidget::clipMoved, this, &MainWindow::onClipMoved);
    connect(m_timeline, &TimelineWidget::clipTrimmed, this, &MainWindow::onClipTrimmed);
    connect(m_timeline, &TimelineWidget::unlinkRequested, this, &MainWindow::onUnlink);
    connect(m_timeline, &TimelineWidget::clipLabelRequested, this, &MainWindow::onClipLabel);
    connect(m_timeline, &TimelineWidget::deleteRequested, this, &MainWindow::onDeleteClip);
    connect(m_timeline, &TimelineWidget::selectionChanged, this, [this](ClipId clip) {
        statusBar()->showMessage(clip ? QString("Selected clip %1").arg(clip) : QString("Ready"));
    });
    connect(m_timeline, &TimelineWidget::mediaDropped, this, &MainWindow::onMediaDropped);
    connect(m_timeline, &TimelineWidget::fileDropped, this, &MainWindow::onFileDropped);
    m_timelineDock = new QDockWidget("Timeline", this);
    m_timelineDock->setObjectName("timelineDock");
    m_timelineDock->setWidget(m_timeline);

    m_browser = new MediaBrowser(this);
    m_browser->setPreviewCache(m_previews.get());
    m_browser->setProject(&m_project);
    connect(m_browser, &MediaBrowser::newFolderRequested, this, &MainWindow::onNewFolder);
    connect(m_browser, &MediaBrowser::importRequested, this, &MainWindow::importMediaDialog);
    connect(m_browser, &MediaBrowser::deleteFolderRequested, this, &MainWindow::onDeleteFolder);
    connect(m_browser, &MediaBrowser::mediaMovedToFolder, this, [this](QList<MediaId> media, FolderId folder) {
        for (MediaId id : media) {
            m_project.setMediaFolder(id, folder);
        }
        m_browser->refresh();
    });
    connect(m_browser, &MediaBrowser::filesImported, this, [this](const QStringList& paths, FolderId folder) {
        for (const QString& path : paths) {
            importMedia(path, folder);
        }
    });
    connect(m_browser, &MediaBrowser::deleteMediaRequested, this, &MainWindow::onDeleteMedia);
    connect(m_browser, &MediaBrowser::mediaLabelChanged, this, [this](QList<MediaId> media, int label) {
        for (MediaId id : media) {
            m_project.setMediaLabel(id, label);
        }
        m_browser->refresh();
    });
    connect(m_browser, &MediaBrowser::folderLabelChanged, this, [this](FolderId folder, int label) {
        m_project.setFolderLabel(folder, label);
        m_browser->refresh();
    });
    connect(m_browser, &MediaBrowser::folderRenamed, this, [this](FolderId folder, const QString& name) {
        m_project.renameFolder(folder, name.toStdString());
        m_browser->refresh();
    });
    connect(m_browser, &MediaBrowser::mediaSelected, this, &MainWindow::showMediaInfo);
    m_browserDock = new QDockWidget("Media", this);
    m_browserDock->setObjectName("mediaDock");
    m_browserDock->setWidget(m_browser);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_logDock = new QDockWidget("Media Info", this);
    m_logDock->setObjectName("mediaInfoDock");
    m_logDock->setWidget(m_log);

    m_meter = new AudioMeter(this);
    m_meterDock = new QDockWidget("Levels", this);
    m_meterDock->setObjectName("levelsDock");
    m_meterDock->setWidget(m_meter);

    m_toolbox = new ToolboxWidget(this);
    m_toolsDock = new QDockWidget("Tools", this);
    m_toolsDock->setObjectName("toolsDock");
    m_toolsDock->setWidget(m_toolbox);
    // The tool strip is narrow; blank the title text and drop the title buttons so
    // the title bar doesn't force the dock wider than the icons. It keeps a standard
    // (draggable, non-clipping) title bar — just with nothing that reserves width.
    m_toolsDock->setWindowTitle(QString());
    m_toolsDock->toggleViewAction()->setText("Tools");  // keep the Window-menu label
    m_toolsDock->setFeatures(QDockWidget::DockWidgetMovable);

    applyDefaultLayout();

    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&New Project", QKeySequence::New, this, &MainWindow::newProject);
    fileMenu->addAction("&Open Project…", QKeySequence::Open, this, &MainWindow::openProject);
    fileMenu->addAction("&Save Project", QKeySequence::Save, this, &MainWindow::saveProject);
    fileMenu->addAction("Save Project &As…", QKeySequence::SaveAs, this, &MainWindow::saveProjectAs);
    fileMenu->addSeparator();
    fileMenu->addAction("&Import Media…", this, [this] { importMediaDialog(m_browser->currentFolder()); });
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", QKeySequence::Quit, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction("&Undo", QKeySequence::Undo, this, &MainWindow::undo);
    editMenu->addAction("&Redo", QKeySequence::Redo, this, &MainWindow::redo);
    editMenu->addSeparator();
    editMenu->addAction("&Delete Clip", QKeySequence::Delete, this, &MainWindow::deleteSelection);

    auto* playbackMenu = menuBar()->addMenu("&Playback");
    playbackMenu->addAction("&Play/Pause", Qt::Key_Space, this, &MainWindow::togglePlay);
    playbackMenu->addSeparator();
    playbackMenu->addAction("Back &5s", Qt::Key_Left, this, [this] { seekRelative(-5.0); });
    playbackMenu->addAction("Forward &5s", Qt::Key_Right, this, [this] { seekRelative(5.0); });
    playbackMenu->addAction("&Restart", Qt::Key_Home, this, [this] { seekRelative(-1e9); });

    auto* windowMenu = menuBar()->addMenu("&Window");
    windowMenu->addAction("&Reset Layout", this, &MainWindow::resetLayout);
    windowMenu->addSeparator();
    // Checkable toggles for each panel.
    windowMenu->addAction(m_browserDock->toggleViewAction());
    windowMenu->addAction(m_toolsDock->toggleViewAction());
    windowMenu->addAction(m_timelineDock->toggleViewAction());
    windowMenu->addAction(m_logDock->toggleViewAction());
    windowMenu->addAction(m_meterDock->toggleViewAction());

    // Poll well above frame rate; the clock decides what's actually due.
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(4);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);
    m_timer->start();

    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow() = default;

void MainWindow::applyDefaultLayout()
{
    // The bottom row owns the bottom-right corner, so the Levels meter can sit
    // right of the timeline while Media Info stays in the top-right only.
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    addDockWidget(Qt::BottomDockWidgetArea, m_browserDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_toolsDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_meterDock);
    addDockWidget(Qt::RightDockWidgetArea, m_logDock);
    // Bottom area, left to right: media bin | tools | timeline | Levels meter.
    splitDockWidget(m_browserDock, m_toolsDock, Qt::Horizontal);
    splitDockWidget(m_toolsDock, m_timelineDock, Qt::Horizontal);
    splitDockWidget(m_timelineDock, m_meterDock, Qt::Horizontal);

    for (QDockWidget* dock : { m_browserDock, m_toolsDock, m_timelineDock, m_logDock, m_meterDock }) {
        dock->setFloating(false);
        dock->show();
    }

    const int w = width() > 100 ? width() : 1600;
    resizeDocks({ m_browserDock, m_toolsDock, m_timelineDock, m_meterDock }, { 300, 42, w - 412, 70 },
                Qt::Horizontal);
    resizeDocks({ m_browserDock, m_timelineDock }, { 360, 360 }, Qt::Vertical);  // taller timeline
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    // Apply the user's saved layout once, over the programmatic default. Only when
    // the saved layout matches the current dock set (kLayoutVersion) — otherwise a
    // stale state wouldn't include a newly added dock and would hide it.
    if (!m_layoutRestored) {
        m_layoutRestored = true;
        QSettings settings;
        if (settings.value("layoutVersion").toInt() == kLayoutVersion && settings.contains("windowState")) {
            restoreGeometry(settings.value("geometry").toByteArray());
            restoreState(settings.value("windowState").toByteArray());
        }
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings settings;
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
    settings.setValue("layoutVersion", kLayoutVersion);
    QMainWindow::closeEvent(event);
}

void MainWindow::resetLayout()
{
    // Re-apply the layout programmatically (never touches window geometry, so a
    // maximized window stays maximized).
    applyDefaultLayout();
}

void MainWindow::showMediaInfo(MediaId media)
{
    if (const MediaSource* source = m_project.media(media)) {
        m_log->setPlainText(describeMedia(*source));
    } else {
        m_log->clear();
    }
}

void MainWindow::reopenPlayer()
{
    m_player = std::make_unique<Player>();
    std::string error;
    m_player->open(m_project, error);
}

void MainWindow::refreshPreviewsForProject()
{
    for (const MediaSource& media : m_project.mediaPool()) {
        m_previews->request(media.id, QString::fromStdString(media.path), media.hasVideo, media.hasAudio,
                            media.width, media.height);
    }
}

MediaId MainWindow::importMedia(const QString& path, FolderId folder)
{
    std::string error;
    const auto info = probeMedia(path.toStdString(), error);
    if (!info) {
        statusBar()->showMessage(QString("Failed to import %1: %2").arg(path, QString::fromStdString(error)));
        return kInvalidMedia;
    }

    const MediaSource source = toMediaSource(*info);
    const bool firstMedia = m_project.mediaPool().empty();
    const MediaId id = m_project.addMedia(source, folder);

    // The first import sets the sequence format.
    if (firstMedia) {
        if (source.rateNum > 0) {
            m_project.sequence().setFrameRate(source.rateNum, source.rateDen);
        }
        if (source.width > 0) {
            m_project.sequence().setResolution(source.width, source.height);
        }
    }

    if (!m_player) {
        reopenPlayer();
    }
    m_previews->request(id, path, source.hasVideo, source.hasAudio, source.width, source.height);
    m_browser->refresh();
    return id;
}

void MainWindow::placeMedia(MediaId media, Tick start)
{
    const MediaSource* source = m_project.media(media);
    if (!source || m_project.sequence().trackCount() < 2) {
        return;
    }

    Clip clip;
    clip.source = media;
    clip.timelineStart = start;
    clip.sourceIn = 0;
    clip.duration = source->duration;
    clip.label = source->label;  // inherit the bin color; editable on the timeline after
    if (source->hasVideo && source->hasAudio) {
        clip.linkGroup = m_project.nextLinkGroup();
    }

    auto compound = std::make_unique<CompoundCommand>("Add Clip");
    if (source->hasVideo) {
        compound->add(std::make_unique<AddClipCommand>(0, clip));
    }
    if (source->hasAudio) {
        compound->add(std::make_unique<AddClipCommand>(1, clip));
    }
    if (compound->empty()) {
        return;
    }

    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        statusBar()->showMessage("Couldn't place clip there (overlaps an existing clip)");
    }
}

void MainWindow::load(const QString& path)
{
    const MediaId id = importMedia(path, kRootFolder);
    if (id != kInvalidMedia) {
        placeMedia(id, 0);
        m_timeline->zoomToFit();
    }
}

void MainWindow::newProject()
{
    m_project.reset();
    m_commands.clear();
    m_previews->clear();
    m_player.reset();
    m_projectPath.clear();
    m_browser->setProject(&m_project);  // resets navigation to the root folder
    m_timeline->clearSelection();
    m_timeline->setPlayhead(0);
    m_timeline->update();
    statusBar()->showMessage("New project");
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(this, "Open Project", QString(),
                                                      "hopline Project (*.hop *.json);;All Files (*)");
    if (path.isEmpty()) {
        return;
    }

    Project loaded;
    std::string error;
    if (!loadProject(path.toStdString(), loaded, error)) {
        QMessageBox::warning(this, "Open Project", QString::fromStdString(error));
        return;
    }

    m_project = std::move(loaded);
    m_commands.clear();
    m_projectPath = path;
    m_previews->clear();
    refreshPreviewsForProject();
    reopenPlayer();
    m_browser->setProject(&m_project);  // resets navigation to the root folder
    m_timeline->clearSelection();
    m_timeline->setPlayhead(0);
    m_timeline->zoomToFit();
    statusBar()->showMessage(QString("Opened %1").arg(path));
}

void MainWindow::saveProject()
{
    if (m_projectPath.isEmpty()) {
        saveProjectAs();
        return;
    }
    std::string error;
    if (hopline::saveProject(m_project, m_projectPath.toStdString(), error)) {
        statusBar()->showMessage(QString("Saved %1").arg(m_projectPath));
    } else {
        QMessageBox::warning(this, "Save Project", QString::fromStdString(error));
    }
}

void MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(this, "Save Project As", QString(), "hopline Project (*.hop)");
    if (path.isEmpty()) {
        return;
    }
    if (!path.contains('.')) {
        path += ".hop";
    }
    m_projectPath = path;
    saveProject();
}

void MainWindow::importMediaDialog(FolderId folder)
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this, "Import Media", QString(),
        "Media Files (*.mp4 *.mov *.mkv *.avi *.webm *.wav *.mp3 *.flac);;All Files (*)");
    for (const QString& path : paths) {
        importMedia(path, folder);
    }
}

void MainWindow::onNewFolder(FolderId parent)
{
    // Create with a placeholder name, then drop straight into inline rename
    // (Finder/Premiere style) instead of prompting up front.
    const FolderId id = m_project.addFolder(parent, "New Folder");
    m_browser->refresh();
    m_browser->beginRenameFolder(id);
}

void MainWindow::onDeleteFolder(FolderId folder)
{
    m_project.removeFolder(folder);
    m_browser->refresh();
}

bool MainWindow::mediaInUse(MediaId id) const
{
    const Sequence& seq = m_project.sequence();
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        for (const Clip& clip : seq.track(i).clips()) {
            if (clip.source == id) {
                return true;
            }
        }
    }
    return false;
}

void MainWindow::onDeleteMedia(QList<MediaId> media)
{
    int inUse = 0;
    for (MediaId id : media) {
        if (mediaInUse(id)) {
            ++inUse;
        } else {
            m_project.removeMedia(id);
        }
    }
    m_browser->refresh();
    if (inUse > 0) {
        statusBar()->showMessage(
            QString("%1 item(s) left in the bin — still used on the timeline.").arg(inUse));
    }
}

void MainWindow::onMediaDropped(MediaId media, Tick start)
{
    placeMedia(media, start);
}

void MainWindow::onFileDropped(const QString& path, Tick start)
{
    const MediaId id = importMedia(path, kRootFolder);
    if (id != kInvalidMedia) {
        placeMedia(id, start);
    }
}

void MainWindow::timelineScrubbed(Tick time)
{
    if (m_player) {
        m_player->seek(secondsFromTicks(time));
    }
}

namespace {

// The clips an edit touches: the whole link group, or just the one clip.
std::vector<std::pair<std::size_t, ClipId>> editTargets(const Project& project, std::size_t track, ClipId clip)
{
    const Clip* c = project.sequence().findClip(clip);
    if (c && c->linked()) {
        return project.sequence().clipsInGroup(c->linkGroup);
    }
    return { { track, clip } };
}

}  // namespace

void MainWindow::commitEdit()
{
    if (m_player) {
        m_player->reload(m_project);  // decode threads run on a snapshot; refresh it
    }
    m_timeline->update();
}

void MainWindow::onClipMoved(std::size_t trackIndex, ClipId clip, Tick newStart)
{
    const Clip* c = m_project.sequence().findClip(clip);
    if (!c) {
        return;
    }
    const Tick delta = newStart - c->timelineStart;

    auto compound = std::make_unique<CompoundCommand>("Move Clip");
    for (const auto& [track, id] : editTargets(m_project, trackIndex, clip)) {
        const Clip* member = m_project.sequence().findClip(id);
        compound->add(std::make_unique<MoveClipCommand>(track, id, track, member->timelineStart + delta));
    }

    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        m_timeline->update();  // rejected (overlap): snap back to the model
    }
}

void MainWindow::onClipTrimmed(std::size_t trackIndex, ClipId clip, bool trimHead, Tick delta)
{
    const auto edge = trimHead ? TrimClipCommand::Edge::Head : TrimClipCommand::Edge::Tail;

    auto compound = std::make_unique<CompoundCommand>("Trim Clip");
    for (const auto& [track, id] : editTargets(m_project, trackIndex, clip)) {
        compound->add(std::make_unique<TrimClipCommand>(track, id, edge, delta));
    }

    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        m_timeline->update();
    }
}

void MainWindow::onUnlink(ClipId clip)
{
    const Clip* c = m_project.sequence().findClip(clip);
    if (c && c->linked()
        && m_commands.execute(m_project, std::make_unique<UnlinkGroupCommand>(c->linkGroup))) {
        commitEdit();
    }
}

void MainWindow::onClipLabel(std::size_t trackIndex, ClipId clip, int label)
{
    auto compound = std::make_unique<CompoundCommand>("Label Clip");
    for (const auto& [track, id] : editTargets(m_project, trackIndex, clip)) {
        compound->add(std::make_unique<SetClipLabelCommand>(track, id, label));
    }
    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    }
}

void MainWindow::onDeleteClip(std::size_t trackIndex, ClipId clip)
{
    auto compound = std::make_unique<CompoundCommand>("Delete Clip");
    for (const auto& [track, id] : editTargets(m_project, trackIndex, clip)) {
        compound->add(std::make_unique<RemoveClipCommand>(track, id));
    }

    if (m_commands.execute(m_project, std::move(compound))) {
        m_timeline->clearSelection();
        commitEdit();
    }
}

void MainWindow::deleteSelection()
{
    const ClipId clip = m_timeline->selected();
    std::size_t track = 0;
    if (clip != kInvalidClip && m_project.sequence().findClip(clip, &track)) {
        onDeleteClip(track, clip);
    }
}

void MainWindow::undo()
{
    if (m_commands.canUndo()) {
        m_commands.undo(m_project);
        m_timeline->clearSelection();
        commitEdit();
    }
}

void MainWindow::redo()
{
    if (m_commands.canRedo()) {
        m_commands.redo(m_project);
        m_timeline->clearSelection();
        commitEdit();
    }
}

void MainWindow::seekRelative(double seconds)
{
    if (m_player) {
        m_player->seek(m_player->position() + seconds);
    }
}

void MainWindow::togglePlay()
{
    if (m_player) {
        m_player->togglePlay();
    }
}

void MainWindow::tick()
{
    if (!m_player) {
        return;
    }

    VideoFrame frame;
    if (m_player->update(frame)) {
        m_preview->setFrame(frame);
    }

    m_timeline->setPlayhead(ticksFromSeconds(m_player->position()));

    const bool playing = m_player->isPlaying();
    m_playButton->setGlyph(playing ? IconButton::Glyph::Pause : IconButton::Glyph::Play);
    m_timeLabel->setText(QString("%1 / %2").arg(formatClock(m_player->position()), formatClock(m_player->duration())));
    m_wasPlaying = playing;

    if (m_player->hasAudio()) {
        m_meter->setLevels(m_player->audioPeak(0), m_player->audioPeak(1));
    } else {
        m_meter->setLevels(0.0f, 0.0f);
    }
}

}  // namespace hopline
