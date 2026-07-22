#include "app/MainWindow.h"

#include <algorithm>

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
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "app/AudioMeter.h"
#include "app/IconButton.h"
#include "app/MediaBrowser.h"
#include "app/PreviewCache.h"
#include "app/PreviewWidget.h"
#include "app/SequenceDialog.h"
#include "app/TimelineWidget.h"
#include "app/ToolboxWidget.h"
#include "media/MediaProbe.h"
#include "model/Commands.h"
#include "model/ProjectIO.h"

namespace hopline {
namespace {

// Bump when the dock set or default arrangement changes, so a stale saved layout
// is discarded.
constexpr int kLayoutVersion = 6;

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
    connect(m_timeline, &TimelineWidget::addTrackRequested, this, &MainWindow::onAddTrack);
    connect(m_timeline, &TimelineWidget::deleteTrackRequested, this, &MainWindow::onDeleteTrack);
    connect(m_timeline, &TimelineWidget::selectionChanged, this, [this](ClipId clip) {
        statusBar()->showMessage(clip ? QString("Selected clip %1").arg(clip) : QString("Ready"));
    });
    connect(m_timeline, &TimelineWidget::mediaDropped, this, &MainWindow::onMediaDropped);
    connect(m_timeline, &TimelineWidget::fileDropped, this, &MainWindow::onFileDropped);

    // Sequence tab strip above the timeline — one tab per open sequence.
    m_seqTabs = new QTabBar(this);
    m_seqTabs->setExpanding(false);
    m_seqTabs->setTabsClosable(true);
    m_seqTabs->setDrawBase(false);
    m_seqTabs->setElideMode(Qt::ElideRight);
    m_seqTabs->setUsesScrollButtons(true);
    m_seqTabs->setVisible(false);  // shown once a sequence is open
    connect(m_seqTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_syncingTabs || index < 0) return;
        const auto id = static_cast<SequenceId>(m_seqTabs->tabData(index).toULongLong());
        if (id != m_project.activeSequenceId()) onSequenceActivated(id);
    });
    connect(m_seqTabs, &QTabBar::tabCloseRequested, this, &MainWindow::closeSequenceTab);

    auto* timelinePanel = new QWidget(this);
    auto* timelineLayout = new QVBoxLayout(timelinePanel);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(0);
    timelineLayout->addWidget(m_seqTabs);
    timelineLayout->addWidget(m_timeline, 1);

    m_timelineDock = new QDockWidget("Timeline", this);
    m_timelineDock->setObjectName("timelineDock");
    m_timelineDock->setWidget(timelinePanel);

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
    connect(m_browser, &MediaBrowser::newSequenceRequested, this, &MainWindow::onNewSequence);
    connect(m_browser, &MediaBrowser::sequenceActivated, this, &MainWindow::onSequenceActivated);
    connect(m_browser, &MediaBrowser::sequenceRenamed, this, [this](SequenceId id, const QString& name) {
        m_project.renameSequence(id, name.toStdString());
        m_browser->refresh();
        syncSequenceTabs();
    });
    connect(m_browser, &MediaBrowser::deleteSequenceRequested, this, &MainWindow::onDeleteSequence);
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
    fileMenu->addAction("New &Sequence…", QKeySequence("Ctrl+Shift+N"), this,
                        [this] { onNewSequence(m_browser->currentFolder()); });
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
    resizeDocks({ m_browserDock, m_timelineDock }, { 440, 440 }, Qt::Vertical);  // taller timeline (fits 3+3 tracks)
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
    if (!m_project.hasActiveSequence()) {
        m_player.reset();  // nothing to play until a sequence is active
        return;
    }
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

void MainWindow::activateSequence(SequenceId sequence)
{
    m_project.setActiveSequence(sequence);
    // Opening a sequence adds it to the timeline's tab strip.
    if (sequence != kInvalidSequence
        && std::find(m_openSequences.begin(), m_openSequences.end(), sequence) == m_openSequences.end()) {
        m_openSequences.push_back(sequence);
    }
    reopenPlayer();
    if (const Sequence* s = m_project.activeSequence()) {
        m_preview->setCanvas(s->width(), s->height());
    } else {
        m_preview->setCanvas(0, 0);
    }
    m_preview->clear();  // show the black canvas until the player supplies a frame
    m_timeline->clearSelection();
    m_timeline->setPlayhead(0);
    m_timeline->update();
    m_browser->refresh();
    syncSequenceTabs();
}

SequenceId MainWindow::ensureActiveSequence(const MediaSource& source)
{
    if (m_project.hasActiveSequence()) {
        return m_project.activeSequenceId();
    }
    // Auto-create a sequence matching the dropped media (Premiere-style).
    const int rn = source.rateNum > 0 ? source.rateNum : 30;
    const int rd = source.rateDen > 0 ? source.rateDen : 1;
    const int w = source.width > 0 ? source.width : 1920;
    const int h = source.height > 0 ? source.height : 1080;
    const SequenceId id = m_project.addSequence("Sequence 1", rn, rd, w, h, kRootFolder);
    seedTracks(id);
    activateSequence(id);
    return id;
}

void MainWindow::seedTracks(SequenceId sequence)
{
    Sequence* seq = m_project.sequenceById(sequence);
    if (!seq) {
        return;
    }
    int nv = 0, na = 0;
    for (std::size_t i = 0; i < seq->trackCount(); ++i) {
        (seq->track(i).kind() == Track::Kind::Video ? nv : na)++;
    }
    for (; nv < 3; ++nv) {
        seq->addTrack(Track::Kind::Video, QString("V%1").arg(nv + 1).toStdString());
    }
    for (; na < 3; ++na) {
        seq->addTrack(Track::Kind::Audio, QString("A%1").arg(na + 1).toStdString());
    }
}

int MainWindow::trackIndexForLevel(bool video, int level)
{
    const Sequence& seq = m_project.sequence();
    int l = 0;
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        if ((seq.track(i).kind() == Track::Kind::Video) == video) {
            if (l == level) {
                return static_cast<int>(i);
            }
            ++l;
        }
    }
    return -1;
}

void MainWindow::ensureTrackLevel(bool video, int level)
{
    Sequence& seq = m_project.sequence();
    int count = 0;
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        if ((seq.track(i).kind() == Track::Kind::Video) == video) {
            ++count;
        }
    }
    for (; count <= level; ++count) {
        const QString name = QString("%1%2").arg(video ? "V" : "A").arg(count + 1);
        seq.addTrack(video ? Track::Kind::Video : Track::Kind::Audio, name.toStdString());
    }
}

void MainWindow::onNewSequence(FolderId folder)
{
    SequenceDialog dialog(QString("Sequence %1").arg(m_project.sequences().size() + 1), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const SequenceSettings s = dialog.settings();
    const SequenceId id = m_project.addSequence(s.name.toStdString(), s.rateNum, s.rateDen, s.width, s.height, folder);
    seedTracks(id);
    activateSequence(id);
    statusBar()->showMessage(QString("Created sequence \"%1\"").arg(s.name));
}

void MainWindow::onSequenceActivated(SequenceId sequence) { activateSequence(sequence); }

void MainWindow::onDeleteSequence(SequenceId sequence)
{
    m_openSequences.erase(std::remove(m_openSequences.begin(), m_openSequences.end(), sequence),
                          m_openSequences.end());
    const bool wasActive = m_project.activeSequenceId() == sequence;
    m_project.removeSequence(sequence);
    if (wasActive) {
        // Fall back to another open sequence, if any.
        activateSequence(m_openSequences.empty() ? m_project.activeSequenceId() : m_openSequences.back());
    } else {
        m_browser->refresh();
        syncSequenceTabs();
    }
}

void MainWindow::syncSequenceTabs()
{
    m_syncingTabs = true;
    while (m_seqTabs->count() > 0) {
        m_seqTabs->removeTab(0);
    }
    int activeIndex = -1;
    for (SequenceId id : m_openSequences) {
        const Sequence* seq = m_project.sequenceById(id);
        if (!seq) continue;
        const int idx = m_seqTabs->addTab(QString::fromStdString(seq->name()));
        m_seqTabs->setTabData(idx, static_cast<qulonglong>(id));
        if (id == m_project.activeSequenceId()) activeIndex = idx;
    }
    if (activeIndex >= 0) m_seqTabs->setCurrentIndex(activeIndex);
    m_seqTabs->setVisible(m_seqTabs->count() > 0);
    m_syncingTabs = false;
}

void MainWindow::closeSequenceTab(int index)
{
    if (index < 0 || index >= m_seqTabs->count()) return;
    const auto id = static_cast<SequenceId>(m_seqTabs->tabData(index).toULongLong());
    m_openSequences.erase(std::remove(m_openSequences.begin(), m_openSequences.end(), id),
                          m_openSequences.end());
    // Closing the active sequence's tab activates another open one (or clears).
    if (id == m_project.activeSequenceId()) {
        activateSequence(m_openSequences.empty() ? kInvalidSequence : m_openSequences.back());
    } else {
        syncSequenceTabs();
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
    const MediaId id = m_project.addMedia(source, folder);
    m_previews->request(id, path, source.hasVideo, source.hasAudio, source.width, source.height);
    m_browser->refresh();
    return id;
}

void MainWindow::placeMedia(MediaId media, Tick start, int level)
{
    const MediaSource* source = m_project.media(media);
    if (!source) {
        return;
    }
    const bool hadSequence = m_project.hasActiveSequence();
    ensureActiveSequence(*source);  // make (and open) a sequence if the project has none
    if (!hadSequence) {
        start = 0;  // a clip that creates a new sequence starts at the very beginning
    }

    // Both halves land on the target level (mirrored), creating tracks if the drop
    // was above/below the outermost track.
    const int lvl = std::max(0, level);
    int videoTrack = -1, audioTrack = -1;
    if (source->hasVideo) {
        ensureTrackLevel(true, lvl);
        videoTrack = trackIndexForLevel(true, lvl);
    }
    if (source->hasAudio) {
        ensureTrackLevel(false, lvl);
        audioTrack = trackIndexForLevel(false, lvl);
    }

    Clip clip;
    clip.source = media;
    clip.timelineStart = start;
    clip.sourceIn = 0;
    clip.duration = source->duration;
    clip.label = source->label;  // inherit the bin color; editable on the timeline after
    if (source->hasVideo && source->hasAudio && videoTrack >= 0 && audioTrack >= 0) {
        clip.linkGroup = m_project.nextLinkGroup();
    }

    auto compound = std::make_unique<CompoundCommand>("Add Clip");
    if (source->hasVideo && videoTrack >= 0) {
        compound->add(std::make_unique<AddClipCommand>(static_cast<std::size_t>(videoTrack), clip));
    }
    if (source->hasAudio && audioTrack >= 0) {
        compound->add(std::make_unique<AddClipCommand>(static_cast<std::size_t>(audioTrack), clip));
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
    m_openSequences.clear();
    m_preview->setCanvas(0, 0);
    m_preview->clear();
    m_browser->setProject(&m_project);  // resets navigation to the root folder
    m_timeline->clearSelection();
    m_timeline->setPlayhead(0);
    m_timeline->update();
    syncSequenceTabs();
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
    m_browser->setProject(&m_project);  // resets navigation to the root folder
    m_openSequences.clear();
    if (m_project.hasActiveSequence()) {
        activateSequence(m_project.activeSequenceId());  // opens its tab, canvas, player, timeline
    } else {
        m_preview->setCanvas(0, 0);
        m_preview->clear();
        reopenPlayer();
        m_timeline->clearSelection();
        m_timeline->setPlayhead(0);
        m_timeline->update();
        syncSequenceTabs();
    }
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

void MainWindow::onMediaDropped(MediaId media, Tick start, int level)
{
    placeMedia(media, start, level);
}

void MainWindow::onFileDropped(const QString& path, Tick start, int level)
{
    const MediaId id = importMedia(path, kRootFolder);
    if (id != kInvalidMedia) {
        placeMedia(id, start, level);
    }
}

void MainWindow::onAddTrack(bool video)
{
    if (!m_project.hasActiveSequence()) {
        return;
    }
    Sequence& seq = m_project.sequence();
    int count = 0;
    for (std::size_t i = 0; i < seq.trackCount(); ++i) {
        if ((seq.track(i).kind() == Track::Kind::Video) == video) {
            ++count;
        }
    }
    const QString name = QString("%1%2").arg(video ? "V" : "A").arg(count + 1);
    // Append (indices of existing tracks are unchanged, so the undo stack stays valid).
    seq.addTrack(video ? Track::Kind::Video : Track::Kind::Audio, name.toStdString());
    commitEdit();
}

void MainWindow::onDeleteTrack(std::size_t trackIndex)
{
    if (!m_project.hasActiveSequence() || trackIndex >= m_project.sequence().trackCount()) {
        return;
    }
    // Removing a track shifts higher track indices, which would invalidate the
    // index-based undo commands — so this clears the undo history.
    m_project.sequence().removeTrackAt(trackIndex);
    m_commands.clear();
    m_timeline->clearSelection();
    commitEdit();
}

void MainWindow::timelineScrubbed(Tick time)
{
    if (m_player) {
        m_player->seek(secondsFromTicks(time));
    }
    // Scrubbing into empty space (no video at `time`) shows black, not a stale frame.
    const Sequence* s = m_project.activeSequence();
    if (!s || !s->topVideoClipAt(time)) {
        m_preview->clear();
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
    m_browser->refresh();  // the active sequence's duration in the bin may have changed
    // If no video covers the playhead now (e.g. the last clip was deleted), the
    // player emits no frame — clear the stale one so the preview shows black.
    const Sequence* s = m_project.activeSequence();
    if (!s || !s->topVideoClipAt(m_timeline->playhead())) {
        m_preview->clear();
    }
}

void MainWindow::onClipMoved(std::size_t fromTrack, ClipId clip, int levelDelta, Tick newStart)
{
    const Clip* dragged = m_project.sequence().findClip(clip);
    if (!dragged) {
        return;
    }
    const Tick timeDelta = newStart - dragged->timelineStart;
    const auto targets = editTargets(m_project, fromTrack, clip);  // the whole link group

    auto levelOf = [this](std::size_t track, bool video) {
        int level = 0;
        for (std::size_t i = 0; i < track; ++i) {
            if ((m_project.sequence().track(i).kind() == Track::Kind::Video) == video) {
                ++level;
            }
        }
        return level;
    };

    // Only the dragged clip changes track (in the timeline, video/audio move between
    // tracks independently); linked partners keep their track and shift in time.
    // Dragging past the outermost track creates a new one (appends don't shift
    // existing indices, so the undo stack stays consistent).
    if (levelDelta != 0) {
        const bool video = m_project.sequence().track(fromTrack).kind() == Track::Kind::Video;
        ensureTrackLevel(video, std::max(0, levelOf(fromTrack, video) + levelDelta));
    }

    auto compound = std::make_unique<CompoundCommand>("Move Clip");
    for (const auto& [track, id] : targets) {
        const Clip* member = m_project.sequence().findClip(id);
        if (!member) {
            continue;
        }
        std::size_t toTrack = track;
        if (id == clip && levelDelta != 0) {  // the dragged clip only
            const bool video = m_project.sequence().track(track).kind() == Track::Kind::Video;
            const int idx = trackIndexForLevel(video, std::max(0, levelOf(track, video) + levelDelta));
            if (idx >= 0) {
                toTrack = static_cast<std::size_t>(idx);
            }
        }
        compound->add(std::make_unique<MoveClipCommand>(track, id, toTrack, member->timelineStart + timeDelta));
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
