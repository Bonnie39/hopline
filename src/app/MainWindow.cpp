#include "app/MainWindow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <tuple>
#include <utility>

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
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
#include <QProgressDialog>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>

#include "app/AudioMeter.h"
#include "app/IconButton.h"
#include "app/MediaBrowser.h"
#include "app/PreferencesDialog.h"
#include "app/PreviewCache.h"
#include "app/PreviewWidget.h"
#include "app/SequenceDialog.h"
#include "app/ShortcutManager.h"
#include "app/EffectControls.h"
#include "app/TimelineScrollBar.h"
#include "app/TimelineWidget.h"
#include "app/ToolboxWidget.h"
#include "engine/Exporter.h"
#include "media/MediaProbe.h"
#include "model/Commands.h"
#include "model/ProjectIO.h"

namespace hopline {
namespace {

// Bump when the dock set or default arrangement changes, so a stale saved layout
// is discarded.
constexpr int kLayoutVersion = 9;  // wider effects + media docks

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
    connect(m_timeline, &TimelineWidget::scrubStarted, this, [this] {
        m_scrubbing = true;
        if (m_player) {
            m_player->beginScrub();  // idle decode threads for on-demand scrub
        }
    });
    connect(m_timeline, &TimelineWidget::scrubEnded, this, [this] {
        m_scrubbing = false;
        if (m_player) {
            m_player->endScrub();  // resume normal playback at the scrub position
        }
    });
    connect(m_timeline, &TimelineWidget::clipMoved, this, &MainWindow::onClipMoved);
    connect(m_timeline, &TimelineWidget::clipsMoved, this, &MainWindow::onClipsMoved);
    connect(m_timeline, &TimelineWidget::clipTrimmed, this, &MainWindow::onClipTrimmed);
    connect(m_timeline, &TimelineWidget::splitRequested, this, &MainWindow::onClipSplit);
    connect(m_timeline, &TimelineWidget::clipRolled, this, &MainWindow::onClipRoll);
    connect(m_timeline, &TimelineWidget::unlinkRequested, this, &MainWindow::onUnlink);
    connect(m_timeline, &TimelineWidget::linkRequested, this, &MainWindow::onLink);
    connect(m_timeline, &TimelineWidget::clipLabelRequested, this, &MainWindow::onClipLabel);
    connect(m_timeline, &TimelineWidget::deleteRequested, this, &MainWindow::onDeleteClip);
    connect(m_timeline, &TimelineWidget::deleteSelectionRequested, this, &MainWindow::deleteSelection);
    connect(m_timeline, &TimelineWidget::addTrackRequested, this, &MainWindow::onAddTrack);
    connect(m_timeline, &TimelineWidget::deleteTrackRequested, this, &MainWindow::onDeleteTrack);
    connect(m_timeline, &TimelineWidget::trackVisibilityToggled, this, &MainWindow::onTrackVisibility);
    connect(m_timeline, &TimelineWidget::trackMuteToggled, this, &MainWindow::onTrackMute);
    connect(m_timeline, &TimelineWidget::trackSoloToggled, this, &MainWindow::onTrackSolo);
    connect(m_timeline, &TimelineWidget::selectionChanged, this, [this](ClipId clip) {
        statusBar()->showMessage(clip ? QString("Selected clip %1").arg(clip) : QString("Ready"));
        updateEffectPanel(clip);
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

    m_timelineScroll = new TimelineScrollBar(this);
    connect(m_timeline, &TimelineWidget::viewChanged, this, [this] {
        m_timelineScroll->setRange(m_timeline->viewTotal(), m_timeline->viewStart(), m_timeline->viewSpan());
    });
    connect(m_timelineScroll, &TimelineScrollBar::rangeChanged, this,
            [this](double start, double span) { m_timeline->setView(start, span); });

    auto* timelinePanel = new QWidget(this);
    auto* timelineLayout = new QVBoxLayout(timelinePanel);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    timelineLayout->setSpacing(0);
    timelineLayout->addWidget(m_seqTabs);
    timelineLayout->addWidget(m_timeline, 1);
    timelineLayout->addWidget(m_timelineScroll);

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

    m_effects = new EffectControls(this);
    m_effectsDock = new QDockWidget("Effect Controls", this);
    m_effectsDock->setObjectName("effectsDock");
    m_effectsDock->setWidget(m_effects);
    connect(m_effects, &EffectControls::propertyEdited, this, &MainWindow::onPropertyEdited);
    connect(m_effects, &EffectControls::keyframeToggled, this, &MainWindow::onKeyframeToggled);
    connect(m_effects, &EffectControls::blendEdited, this, [this](BlendMode blend) {
        if (m_fxVideoClip == kInvalidClip) {
            return;
        }
        const Clip* c = m_project.sequence().findClip(m_fxVideoClip);
        if (!c) {
            return;
        }
        Transform t = c->transform;
        t.blend = blend;
        if (m_commands.execute(m_project,
                               std::make_unique<SetClipTransformCommand>(m_fxVideoTrack, m_fxVideoClip, t))) {
            commitEdit();
        }
    });
    connect(m_effects, &EffectControls::scrubBegin, this, [this] {
        m_scrubbing = true;
        if (m_player) {
            m_player->beginScrub();
        }
    });
    connect(m_effects, &EffectControls::scrubEnd, this, [this] {
        m_scrubbing = false;
        if (m_player) {
            m_player->endScrub();
        }
    });
    connect(m_effects, &EffectControls::seekRequested, this, [this](Tick t) {
        m_timeline->setPlayhead(t);
        timelineScrubbed(t);  // scrubComposite while ruler-scrubbing, else a plain seek (arrow jump)
        updateEffectPanel(m_timeline->selected());
    });
    connect(m_effects, &EffectControls::keyframesEdited, this, &MainWindow::applyKeyEdits);

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

    connect(m_toolbox, &ToolboxWidget::toolSelected, this,
            [this](int tool) { m_timeline->setTool(static_cast<TimelineWidget::Tool>(tool)); });
    // Customizable shortcuts run through the ShortcutManager (rebindable via Edit ▸ Preferences).
    // WindowShortcut context, so a focused text field's ShortcutOverride blocks them mid-typing.
    m_shortcuts = std::make_unique<ShortcutManager>(this);
    m_shortcuts->add("tool.select", "Tools", "Select Tool", QKeySequence(Qt::Key_V),
                     [this] { activateTool(int(ToolboxWidget::Select)); }, false);
    m_shortcuts->add("tool.blade", "Tools", "Blade Tool", QKeySequence(Qt::Key_C),
                     [this] { activateTool(int(ToolboxWidget::Blade)); }, false);
    m_shortcuts->add("tool.text", "Tools", "Text Tool", QKeySequence(Qt::Key_T),
                     [this] { activateTool(int(ToolboxWidget::Text)); }, false);
    m_shortcuts->add("edit.rippleStart", "Editing", "Ripple Trim Start", QKeySequence(Qt::Key_Q),
                     [this] { rippleTrim(true); }, false);
    m_shortcuts->add("edit.rippleEnd", "Editing", "Ripple Trim End", QKeySequence(Qt::Key_E),
                     [this] { rippleTrim(false); }, false);

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
    fileMenu->addAction(m_shortcuts->add("file.export", "File", "&Export Media…",
                                         QKeySequence("Ctrl+E"), [this] { exportMedia(); }, true));
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", QKeySequence::Quit, this, &QWidget::close);

    auto* editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction(m_shortcuts->add("edit.undo", "Editing", "&Undo",
                                         QKeySequence(QKeySequence::Undo), [this] { undo(); }, true));
    editMenu->addAction(m_shortcuts->add("edit.redo", "Editing", "&Redo",
                                         QKeySequence(QKeySequence::Redo), [this] { redo(); }, true));
    editMenu->addSeparator();
    editMenu->addAction(m_shortcuts->add("edit.delete", "Editing", "&Delete Clip",
                                         QKeySequence(QKeySequence::Delete), [this] { deleteSelection(); }, true));
    editMenu->addSeparator();
    editMenu->addAction("&Preferences…", this, &MainWindow::showPreferences);

    auto* playbackMenu = menuBar()->addMenu("&Playback");
    playbackMenu->addAction(m_shortcuts->add("playback.playPause", "Playback", "&Play/Pause",
                                             QKeySequence(Qt::Key_Space), [this] { togglePlay(); }, true));
    playbackMenu->addSeparator();
    playbackMenu->addAction(m_shortcuts->add("playback.back5", "Playback", "Back &5s",
                                             QKeySequence(Qt::Key_Left), [this] { seekRelative(-5.0); }, true));
    playbackMenu->addAction(m_shortcuts->add("playback.forward5", "Playback", "Forward &5s",
                                             QKeySequence(Qt::Key_Right), [this] { seekRelative(5.0); }, true));
    playbackMenu->addAction(m_shortcuts->add("playback.restart", "Playback", "&Restart",
                                             QKeySequence(Qt::Key_Home), [this] { seekRelative(-1e9); }, true));

    auto* windowMenu = menuBar()->addMenu("&Window");
    windowMenu->addAction("&Reset Layout", this, &MainWindow::resetLayout);
    windowMenu->addSeparator();
    // Checkable toggles for each panel.
    windowMenu->addAction(m_effectsDock->toggleViewAction());
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
    // The bottom row owns both bottom corners, so it spans the full width while the
    // Effect Controls dock stays in the upper-left (left of the preview) and Media
    // Info stays top-right.
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);

    addDockWidget(Qt::LeftDockWidgetArea, m_effectsDock);  // upper-left, next to the preview
    addDockWidget(Qt::BottomDockWidgetArea, m_browserDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_toolsDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_meterDock);
    addDockWidget(Qt::RightDockWidgetArea, m_logDock);
    // Bottom area, left to right: media bin | tools | timeline | Levels meter.
    splitDockWidget(m_browserDock, m_toolsDock, Qt::Horizontal);
    splitDockWidget(m_toolsDock, m_timelineDock, Qt::Horizontal);
    splitDockWidget(m_timelineDock, m_meterDock, Qt::Horizontal);

    for (QDockWidget* dock :
         { m_effectsDock, m_browserDock, m_toolsDock, m_timelineDock, m_logDock, m_meterDock }) {
        dock->setFloating(false);
        dock->show();
    }

    const int w = width() > 100 ? width() : 1600;
    resizeDocks({ m_browserDock, m_toolsDock, m_timelineDock, m_meterDock }, { 380, 42, w - 492, 70 },
                Qt::Horizontal);
    resizeDocks({ m_effectsDock }, { 600 }, Qt::Horizontal);  // room for an even effects/keyframes split
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
    m_timelineScroll->setRange(m_timeline->viewTotal(), m_timeline->viewStart(), m_timeline->viewSpan());
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
        start = 0;   // a clip that creates a new sequence starts at the very beginning...
        level = 0;   // ...on V1/A1
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

    // Overwrite any existing clips in the drop region on each target track (auto-trim).
    const TimeRange region{ clip.timelineStart, clip.duration };
    auto compound = std::make_unique<CompoundCommand>("Add Clip");
    if (source->hasVideo && videoTrack >= 0) {
        compound->add(std::make_unique<ClearRegionCommand>(static_cast<std::size_t>(videoTrack), region));
        compound->add(std::make_unique<AddClipCommand>(static_cast<std::size_t>(videoTrack), clip));
    }
    if (source->hasAudio && audioTrack >= 0) {
        compound->add(std::make_unique<ClearRegionCommand>(static_cast<std::size_t>(audioTrack), region));
        compound->add(std::make_unique<AddClipCommand>(static_cast<std::size_t>(audioTrack), clip));
    }
    if (compound->empty()) {
        return;
    }

    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        statusBar()->showMessage("Couldn't place clip there");
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

void MainWindow::exportMedia()
{
    if (!m_project.hasActiveSequence() || m_project.sequence().duration() <= 0) {
        statusBar()->showMessage("Nothing to export — the active sequence is empty.");
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Export Media", QString(), "MP4 Video (*.mp4)");
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(".mp4", Qt::CaseInsensitive)) {
        path += ".mp4";
    }

    if (m_player) {
        m_player->pause();  // free the CPU for the export; playback threads idle on a full queue
    }

    // The exporter runs headless on a worker thread; the UI polls progress and can cancel.
    const Project snapshot = m_project;  // decouple from any later edits
    const std::string outPath = path.toStdString();
    std::atomic<double> progress{ 0.0 };
    std::atomic<bool> cancel{ false };
    std::atomic<bool> done{ false };
    bool ok = false;
    std::string error;

    std::thread worker([&] {
        Exporter exporter;
        ok = exporter.run(snapshot, outPath, [&](double p) { progress.store(p, std::memory_order_relaxed); },
                          cancel, error);
        done.store(true, std::memory_order_release);
    });

    QProgressDialog dlg("Exporting…", "Cancel", 0, 100, this);
    dlg.setWindowTitle("Export");
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setValue(0);
    while (!done.load(std::memory_order_acquire)) {
        dlg.setValue(static_cast<int>(progress.load(std::memory_order_relaxed) * 100.0));
        if (dlg.wasCanceled()) {
            cancel.store(true, std::memory_order_relaxed);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    worker.join();
    dlg.reset();

    if (cancel.load(std::memory_order_relaxed)) {
        QFile::remove(path);  // partial file has no trailer; drop it
        statusBar()->showMessage("Export canceled.");
    } else if (ok) {
        statusBar()->showMessage("Exported to " + path);
    } else {
        QFile::remove(path);
        statusBar()->showMessage("Export failed: " + QString::fromStdString(error));
    }
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

void MainWindow::onTrackVisibility(std::size_t trackIndex)
{
    if (!m_project.hasActiveSequence() || trackIndex >= m_project.sequence().trackCount()) {
        return;
    }
    Track& t = m_project.sequence().track(trackIndex);
    t.setVisible(!t.visible());
    commitEdit();
}

void MainWindow::onTrackMute(std::size_t trackIndex)
{
    if (!m_project.hasActiveSequence() || trackIndex >= m_project.sequence().trackCount()) {
        return;
    }
    Track& t = m_project.sequence().track(trackIndex);
    t.setMuted(!t.muted());
    commitEdit();
}

void MainWindow::onTrackSolo(std::size_t trackIndex)
{
    if (!m_project.hasActiveSequence() || trackIndex >= m_project.sequence().trackCount()) {
        return;
    }
    Track& t = m_project.sequence().track(trackIndex);
    t.setSoloed(!t.soloed());
    commitEdit();
}

void MainWindow::timelineScrubbed(Tick time)
{
    if (!m_player) {
        return;
    }
    if (m_scrubbing) {
        // On-demand: decode + composite the frame at `time` without restarting threads.
        m_preview->setFrame(m_player->scrubComposite(time));
        return;
    }
    // Programmatic playhead move (not a ruler drag): a normal seek.
    m_player->seek(secondsFromTicks(time));
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

bool isVideoProp(FxProp p)
{
    return p == FxProp::PosX || p == FxProp::PosY || p == FxProp::Scale || p == FxProp::Rotation
           || p == FxProp::Opacity;
}

AnimatedValue& videoAV(Transform& t, FxProp p)
{
    switch (p) {
    case FxProp::PosX:     return t.posX;
    case FxProp::PosY:     return t.posY;
    case FxProp::Scale:    return t.scale;
    case FxProp::Rotation: return t.rotation;
    default:              return t.opacity;
    }
}

AnimatedValue& audioAV(AudioLevels& a, FxProp p) { return p == FxProp::VolumeDb ? a.volumeDb : a.pan; }

const AnimatedValue& videoAVc(const Transform& t, FxProp p)
{
    switch (p) {
    case FxProp::PosX:     return t.posX;
    case FxProp::PosY:     return t.posY;
    case FxProp::Scale:    return t.scale;
    case FxProp::Rotation: return t.rotation;
    default:              return t.opacity;
    }
}

const AnimatedValue& audioAVc(const AudioLevels& a, FxProp p) { return p == FxProp::VolumeDb ? a.volumeDb : a.pan; }

// Set the property to `value` at the playhead: a keyframe when animated, else the constant.
void applyProp(AnimatedValue& av, double value, Tick localT)
{
    if (av.animated()) {
        av.setKeyframe(localT, value);
    } else {
        av.constant = value;
    }
}

std::vector<Tick> keyTimes(const AnimatedValue& av)
{
    std::vector<Tick> t;
    t.reserve(av.keys.size());
    for (const Keyframe& k : av.keys) {
        t.push_back(k.time);
    }
    return t;
}

}  // namespace

void MainWindow::updateEffectPanel(ClipId clip)
{
    m_fxVideoClip = kInvalidClip;
    m_fxAudioClip = kInvalidClip;

    // 2+ clips selected that aren't a single linked clip: can't edit effects for a set.
    const std::vector<ClipId>& selection = m_timeline->selection();
    if (selection.size() >= 2) {
        LinkGroup g = kNoLink;
        bool oneLinkedClip = true;
        for (ClipId id : selection) {
            const Clip* c = m_project.sequence().findClip(id);
            if (!c || !c->linked() || (g != kNoLink && c->linkGroup != g)) {
                oneLinkedClip = false;
                break;
            }
            g = c->linkGroup;
        }
        if (!oneLinkedClip) {
            m_fxHasAnim = false;
            m_effects->showMultiple();
            return;
        }
    }

    std::size_t selTrack = 0;
    const Clip* sel = (clip != kInvalidClip && m_project.hasActiveSequence())
                          ? m_project.sequence().findClip(clip, &selTrack)
                          : nullptr;
    if (!sel) {
        m_effects->showNone();
        return;
    }

    // A linked V+A pair shows both sections; each edit targets its own half.
    const Sequence& seq = m_project.sequence();
    std::vector<std::pair<std::size_t, ClipId>> members;
    if (sel->linked()) {
        members = seq.clipsInGroup(sel->linkGroup);
    } else {
        members = { { selTrack, clip } };
    }

    const Clip* videoClip = nullptr;
    const Clip* audioClip = nullptr;
    for (const auto& [track, id] : members) {
        const Clip* c = seq.track(track).find(id);
        if (!c) {
            continue;
        }
        if (seq.track(track).kind() == Track::Kind::Video) {
            videoClip = c;
            m_fxVideoTrack = track;
            m_fxVideoClip = id;
        } else {
            audioClip = c;
            m_fxAudioTrack = track;
            m_fxAudioClip = id;
        }
    }

    if (!videoClip && !audioClip) {
        m_fxHasAnim = false;
        m_effects->showNone();
        return;
    }

    // Build the resolved-at-playhead view for the panel.
    const Tick playhead = m_timeline->playhead();
    FxView view;
    view.canvasW = seq.width();
    view.canvasH = seq.height();
    bool anyAnim = false;
    if (videoClip) {
        view.hasVideo = true;
        const Transform& tf = videoClip->transform;
        const Tick lt = playhead - videoClip->timelineStart;
        view.posX = tf.posX.at(lt);
        view.posY = tf.posY.at(lt);
        view.scale = tf.scale.at(lt);
        view.rotation = tf.rotation.at(lt);
        view.opacity = tf.opacity.at(lt);
        view.blend = tf.blend;
        view.anim[static_cast<int>(FxProp::PosX)] = tf.posX.animated();
        view.anim[static_cast<int>(FxProp::PosY)] = tf.posY.animated();
        view.anim[static_cast<int>(FxProp::Scale)] = tf.scale.animated();
        view.anim[static_cast<int>(FxProp::Rotation)] = tf.rotation.animated();
        view.anim[static_cast<int>(FxProp::Opacity)] = tf.opacity.animated();
        view.keys[static_cast<int>(FxProp::PosX)] = keyTimes(tf.posX);
        view.keys[static_cast<int>(FxProp::PosY)] = keyTimes(tf.posY);
        view.keys[static_cast<int>(FxProp::Scale)] = keyTimes(tf.scale);
        view.keys[static_cast<int>(FxProp::Rotation)] = keyTimes(tf.rotation);
        view.keys[static_cast<int>(FxProp::Opacity)] = keyTimes(tf.opacity);
        anyAnim = anyAnim || tf.posX.animated() || tf.posY.animated() || tf.scale.animated()
                  || tf.rotation.animated() || tf.opacity.animated();
    }
    if (audioClip) {
        view.hasAudio = true;
        const AudioLevels& lv = audioClip->audio;
        const Tick lt = playhead - audioClip->timelineStart;
        view.volumeDb = lv.volumeDb.at(lt);
        view.pan = lv.pan.at(lt);
        view.anim[static_cast<int>(FxProp::VolumeDb)] = lv.volumeDb.animated();
        view.anim[static_cast<int>(FxProp::Pan)] = lv.pan.animated();
        view.keys[static_cast<int>(FxProp::VolumeDb)] = keyTimes(lv.volumeDb);
        view.keys[static_cast<int>(FxProp::Pan)] = keyTimes(lv.pan);
        anyAnim = anyAnim || lv.volumeDb.animated() || lv.pan.animated();
    }
    // Keyframe pane time axis: the primary (video, else audio) clip.
    const Clip* primary = videoClip ? videoClip : audioClip;
    view.clipStart = primary->timelineStart;
    view.clipDuration = primary->duration;
    view.playheadLocal = playhead - primary->timelineStart;
    m_fxHasAnim = anyAnim;  // drives the playhead-follow value refresh
    m_effects->showClip(view);
}

void MainWindow::onPropertyEdited(FxProp prop, double modelValue, bool committing)
{
    const bool video = isVideoProp(prop);
    const ClipId clipId = video ? m_fxVideoClip : m_fxAudioClip;
    const std::size_t track = video ? m_fxVideoTrack : m_fxAudioTrack;
    if (clipId == kInvalidClip) {
        return;
    }
    const Clip* c = m_project.sequence().findClip(clipId);
    if (!c) {
        return;
    }
    const Tick localT = m_timeline->playhead() - c->timelineStart;

    if (video) {
        if (!m_fxEditing) {  // interaction start: capture baseline for a single undo step
            m_fxEditing = true;
            m_fxVideoBaseline = c->transform;
            if (!committing && m_player) {
                m_player->beginPreview();  // idle threads + capture playhead frames (a drag)
            }
        }
        Transform nt = m_fxVideoBaseline;  // recompute from baseline so a drag doesn't accumulate
        applyProp(videoAV(nt, prop), modelValue, localT);
        if (!committing) {
            m_project.sequence().track(track).setTransform(clipId, nt);
            if (m_player) {
                m_preview->setFrame(m_player->previewComposite(m_project));
            }
            return;
        }
        m_fxEditing = false;
        m_project.sequence().track(track).setTransform(clipId, m_fxVideoBaseline);  // restore for undo
        if (nt != m_fxVideoBaseline
            && m_commands.execute(m_project, std::make_unique<SetClipTransformCommand>(track, clipId, nt))) {
            commitEdit();
        } else if (m_player) {
            m_player->reload(m_project);  // no net change: restart threads to end the preview
        }
    } else {
        if (!m_fxEditing) {
            m_fxEditing = true;
            m_fxAudioBaseline = c->audio;
        }
        AudioLevels na = m_fxAudioBaseline;
        applyProp(audioAV(na, prop), modelValue, localT);
        if (!committing) {
            if (m_player) {  // live via the audio-thread override (resolved at the playhead)
                m_player->setAudioPreview(clipId, na.volumeDb.at(localT), na.pan.at(localT));
            }
            return;
        }
        m_fxEditing = false;
        if (m_player) {
            m_player->clearAudioPreview();
        }
        if (na != m_fxAudioBaseline
            && m_commands.execute(m_project, std::make_unique<SetClipAudioCommand>(track, clipId, na))) {
            commitEdit();
        }
    }
    updateEffectPanel(m_timeline->selected());  // show a just-created keyframe's diamond now (drags returned above)
}

void MainWindow::onKeyframeToggled(FxProp prop, bool enabled)
{
    const bool video = isVideoProp(prop);
    const ClipId clipId = video ? m_fxVideoClip : m_fxAudioClip;
    const std::size_t track = video ? m_fxVideoTrack : m_fxAudioTrack;
    if (clipId == kInvalidClip) {
        return;
    }
    const Clip* c = m_project.sequence().findClip(clipId);
    if (!c) {
        return;
    }
    const Tick localT = m_timeline->playhead() - c->timelineStart;

    auto toggle = [&](AnimatedValue& av) {
        const double cur = av.at(localT);
        av.clearKeys();
        if (enabled) {
            av.setKeyframe(localT, cur);  // first keyframe at the playhead
        } else {
            av.constant = cur;            // collapse to a constant (value at the playhead)
        }
    };

    if (video) {
        Transform t = c->transform;
        toggle(videoAV(t, prop));
        if (m_commands.execute(m_project, std::make_unique<SetClipTransformCommand>(track, clipId, t))) {
            commitEdit();
        }
    } else {
        AudioLevels a = c->audio;
        toggle(audioAV(a, prop));
        if (m_commands.execute(m_project, std::make_unique<SetClipAudioCommand>(track, clipId, a))) {
            commitEdit();
        }
    }
    updateEffectPanel(m_timeline->selected());  // reflect the new animated state
}

void MainWindow::applyKeyEdits(const std::vector<KeyEdit>& edits)
{
    if (edits.empty()) {
        return;
    }
    const Clip* vc = m_fxVideoClip != kInvalidClip ? m_project.sequence().findClip(m_fxVideoClip) : nullptr;
    const Clip* ac = m_fxAudioClip != kInvalidClip ? m_project.sequence().findClip(m_fxAudioClip) : nullptr;

    Transform t = vc ? vc->transform : Transform{};
    AudioLevels a = ac ? ac->audio : AudioLevels{};
    bool videoChanged = false, audioChanged = false;

    // Capture each key's value from the *original* first, then remove all, then add all —
    // so overlapping moves within a property don't read a half-modified curve.
    struct Cap {
        const KeyEdit* e;
        double val;
        bool video;
    };
    std::vector<Cap> caps;
    for (const KeyEdit& e : edits) {
        const bool video = isVideoProp(e.prop);
        if ((video && !vc) || (!video && !ac)) {
            continue;
        }
        const AnimatedValue& av0 = video ? videoAVc(vc->transform, e.prop) : audioAVc(ac->audio, e.prop);
        caps.push_back({ &e, av0.at(e.oldTime), video });
    }
    for (const Cap& c : caps) {
        (c.video ? videoAV(t, c.e->prop) : audioAV(a, c.e->prop)).removeKeyframe(c.e->oldTime);
        (c.video ? videoChanged : audioChanged) = true;
    }
    for (const Cap& c : caps) {
        if (!c.e->remove) {
            (c.video ? videoAV(t, c.e->prop) : audioAV(a, c.e->prop)).setKeyframe(c.e->newTime, c.val);
        }
    }

    auto compound = std::make_unique<CompoundCommand>("Keyframes");
    if (videoChanged) {
        compound->add(std::make_unique<SetClipTransformCommand>(m_fxVideoTrack, m_fxVideoClip, t));
    }
    if (audioChanged) {
        compound->add(std::make_unique<SetClipAudioCommand>(m_fxAudioTrack, m_fxAudioClip, a));
    }
    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    }
    updateEffectPanel(m_timeline->selected());
}

void MainWindow::commitEdit()
{
    if (m_player) {
        m_player->reload(m_project);  // decode threads run on a snapshot; refresh it
    }
    m_timeline->update();
    m_timelineScroll->setRange(m_timeline->viewTotal(), m_timeline->viewStart(), m_timeline->viewSpan());
    m_browser->refresh();  // the active sequence's duration in the bin may have changed
    // If no video covers the playhead now (e.g. the last clip was deleted), the
    // player emits no frame — clear the stale one so the preview shows black.
    const Sequence* s = m_project.activeSequence();
    if (!s || !s->topVideoClipAt(m_timeline->playhead())) {
        m_preview->clear();
    }
}

void MainWindow::onClipMoved(std::size_t fromTrack, ClipId clip, int levelDelta, Tick newStart, bool duplicate)
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

    // Duplicate: copies form one new link group (linked to each other, not the originals).
    const LinkGroup newGroup = (duplicate && dragged->linked()) ? m_project.nextLinkGroup() : kNoLink;

    auto compound = std::make_unique<CompoundCommand>(duplicate ? "Duplicate Clip" : "Move Clip");
    for (const auto& [track, id] : targets) {
        const Clip* member = m_project.sequence().findClip(id);
        if (!member) {
            continue;
        }
        // Move: only the dragged clip changes track (V/A move independently). Duplicate: every
        // copy shifts by the level so a linked pair lands on a fresh V/A layer together.
        // Dragging past the outermost track creates one (appends don't shift existing indices).
        std::size_t toTrack = track;
        if (levelDelta != 0 && (duplicate || id == clip)) {
            const bool video = m_project.sequence().track(track).kind() == Track::Kind::Video;
            const int lvl = std::max(0, levelOf(track, video) + levelDelta);
            ensureTrackLevel(video, lvl);
            const int idx = trackIndexForLevel(video, lvl);
            if (idx >= 0) {
                toTrack = static_cast<std::size_t>(idx);
            }
        }

        const Tick memberStart = member->timelineStart + timeDelta;
        if (duplicate) {
            Clip copy = *member;
            copy.id = kInvalidClip;  // AddClipCommand assigns a fresh id
            copy.timelineStart = memberStart;
            if (copy.linked()) {
                copy.linkGroup = newGroup;
            }
            // Copies overwrite whatever they land on, including the originals.
            compound->add(std::make_unique<ClearRegionCommand>(
                toTrack, TimeRange{ memberStart, member->duration }));
            compound->add(std::make_unique<AddClipCommand>(toTrack, copy));
        } else {
            compound->add(std::make_unique<ClearRegionCommand>(
                toTrack, TimeRange{ memberStart, member->duration }, std::vector<ClipId>{ id }));
            compound->add(std::make_unique<MoveClipCommand>(track, id, toTrack, memberStart));
        }
    }

    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        m_timeline->update();  // rejected (overlap): snap back to the model
    }
}

void MainWindow::onClipsMoved(const std::vector<ClipId>& clips, Tick delta, bool duplicate)
{
    if (delta == 0 || clips.empty()) {
        return;
    }
    // Expand the selection to full link groups, de-duplicated.
    std::vector<std::pair<std::size_t, ClipId>> members;
    auto known = [&](ClipId id) {
        for (const auto& [t, i] : members) {
            if (i == id) {
                return true;
            }
        }
        return false;
    };
    for (ClipId id : clips) {
        std::size_t track = 0;
        if (!m_project.sequence().findClip(id, &track)) {
            continue;
        }
        for (const auto& [t, mid] : editTargets(m_project, track, id)) {
            if (!known(mid)) {
                members.push_back({ t, mid });
            }
        }
    }
    if (members.size() < 1) {
        return;
    }
    std::vector<ClipId> memberIds;
    for (const auto& [t, id] : members) {
        memberIds.push_back(id);
    }

    if (duplicate) {
        // Copy each member at +delta with fresh ids; each original link group maps to one new
        // group so the copies are linked to each other, not to the originals.
        std::vector<std::pair<LinkGroup, LinkGroup>> groupMap;
        auto mapGroup = [&](LinkGroup g) {
            for (const auto& [oldg, newg] : groupMap) {
                if (oldg == g) {
                    return newg;
                }
            }
            const LinkGroup ng = m_project.nextLinkGroup();
            groupMap.push_back({ g, ng });
            return ng;
        };
        auto compound = std::make_unique<CompoundCommand>("Duplicate Clips");
        for (const auto& [track, id] : members) {
            const Clip* c = m_project.sequence().findClip(id);
            if (!c) {
                continue;
            }
            Clip copy = *c;
            copy.id = kInvalidClip;  // AddClipCommand assigns a fresh id
            copy.timelineStart += delta;
            if (copy.linked()) {
                copy.linkGroup = mapGroup(copy.linkGroup);
            }
            compound->add(std::make_unique<ClearRegionCommand>(
                track, TimeRange{ copy.timelineStart, copy.duration }));  // copies overwrite, incl. originals
            compound->add(std::make_unique<AddClipCommand>(track, copy));
        }
        if (m_commands.execute(m_project, std::move(compound))) {
            commitEdit();
        }
        return;
    }

    // Multi-move: overwrite non-members at each destination, then move all atomically.
    auto compound = std::make_unique<CompoundCommand>("Move Clips");
    for (const auto& [track, id] : members) {
        const Clip* c = m_project.sequence().findClip(id);
        if (!c) {
            continue;
        }
        compound->add(std::make_unique<ClearRegionCommand>(
            track, TimeRange{ c->timelineStart + delta, c->duration }, memberIds));
    }
    compound->add(std::make_unique<MoveClipsCommand>(members, delta));
    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        m_timeline->update();
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

void MainWindow::onClipRoll(std::size_t trackIndex, ClipId left, ClipId right, Tick delta)
{
    if (delta == 0) {
        return;
    }
    const Sequence& seq = m_project.sequence();
    const Clip* L = seq.findClip(left);
    const Clip* R = seq.findClip(right);
    if (!L || !R) {
        return;
    }
    const Tick boundary = L->range().end();

    // Roll the base pair plus any linked-partner pair butt-joined at the same boundary, so a
    // linked V+A edit point rolls together.
    std::vector<std::tuple<std::size_t, ClipId, ClipId>> pairs;
    pairs.push_back({ trackIndex, left, right });
    if (L->linked() && R->linked()) {
        for (const auto& [lt, lid] : seq.clipsInGroup(L->linkGroup)) {
            if (lid == left) {
                continue;
            }
            for (const auto& [rt, rid] : seq.clipsInGroup(R->linkGroup)) {
                const Clip* lp = seq.findClip(lid);
                const Clip* rp = seq.findClip(rid);
                if (rt == lt && lp && rp && lp->range().end() == boundary && rp->timelineStart == boundary) {
                    pairs.push_back({ lt, lid, rid });
                }
            }
        }
    }

    auto compound = std::make_unique<CompoundCommand>("Roll Edit");
    for (const auto& [t, l, r] : pairs) {
        compound->add(std::make_unique<RollEditCommand>(t, l, r, delta));
    }
    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    } else {
        m_timeline->update();
    }
}

void MainWindow::onClipSplit(std::size_t trackIndex, ClipId clip, Tick at)
{
    const auto targets = editTargets(m_project, trackIndex, clip);
    // A linked pair splits into a left pair (the original group) and a new right pair.
    const LinkGroup rightGroup = targets.size() > 1 ? m_project.nextLinkGroup() : kNoLink;

    auto compound = std::make_unique<CompoundCommand>("Split Clip");
    for (const auto& [track, id] : targets) {
        compound->add(std::make_unique<SplitClipCommand>(track, id, at, rightGroup));
    }
    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
    }
}

void MainWindow::activateTool(int tool)
{
    m_toolbox->setCurrentTool(tool);  // no-op re-check is fine; doesn't re-emit
    m_timeline->setTool(static_cast<TimelineWidget::Tool>(tool));
}

void MainWindow::showPreferences()
{
    PreferencesDialog dlg(m_shortcuts.get(), this);
    dlg.exec();  // applies + persists on OK, live on the QActions
}

void MainWindow::rippleTrim(bool trimStart)
{
    if (!m_project.hasActiveSequence()) {
        return;
    }
    const ClipId id = m_timeline->selected();
    if (id == kInvalidClip) {
        statusBar()->showMessage("Select a clip first to ripple-trim.");
        return;
    }
    std::size_t track = 0;
    const Clip* clip = m_project.sequence().findClip(id, &track);
    if (!clip) {
        return;
    }
    const Tick playhead = m_timeline->playhead();
    if (playhead <= clip->timelineStart || playhead >= clip->range().end()) {
        statusBar()->showMessage("Move the playhead inside the clip to ripple-trim.");
        return;
    }
    const Tick clipStart = clip->timelineStart;  // a head trim keeps this — the clip's new start
    const Tick ripplePoint = clip->range().end();  // original end: everything at/after here slides left
    const Tick delta = trimStart ? (playhead - clip->timelineStart) : (clip->range().end() - playhead);
    const auto edge = trimStart ? RippleTrimCommand::Edge::Head : RippleTrimCommand::Edge::Tail;

    // Global ripple: the link group's tracks get the trim (+ their own downstream shift); every
    // other track slides its clips at/after the ripple point, so all tracks stay in sync.
    const auto targets = editTargets(m_project, track, id);
    auto isLinkTrack = [&](std::size_t t) {
        for (const auto& [lt, lid] : targets) {
            if (lt == t) {
                return true;
            }
        }
        return false;
    };

    auto compound = std::make_unique<CompoundCommand>(trimStart ? "Ripple Trim Start" : "Ripple Trim End");
    for (const auto& [t, cid] : targets) {
        compound->add(std::make_unique<RippleTrimCommand>(t, cid, edge, delta));
    }
    for (std::size_t t = 0; t < m_project.sequence().trackCount(); ++t) {
        if (!isLinkTrack(t)) {
            compound->add(std::make_unique<RippleShiftCommand>(t, ripplePoint, delta));
        }
    }
    if (m_commands.execute(m_project, std::move(compound))) {
        commitEdit();
        if (trimStart) {  // front ripple: park the playhead at the trimmed clip's new head
            m_timeline->setPlayhead(clipStart);
            timelineScrubbed(clipStart);
        }
    } else {
        statusBar()->showMessage("Ripple trim rejected (would overlap or exceed the source).");
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

void MainWindow::onLink(const std::vector<ClipId>& clips)
{
    std::vector<std::pair<std::size_t, ClipId>> members;
    for (ClipId id : clips) {
        std::size_t track = 0;
        if (m_project.sequence().findClip(id, &track)) {
            members.push_back({ track, id });
        }
    }
    if (members.size() >= 2
        && m_commands.execute(m_project, std::make_unique<LinkClipsCommand>(std::move(members)))) {
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
    const std::vector<ClipId> sel = m_timeline->selection();
    if (sel.empty()) {
        return;
    }
    // Every selected clip plus each one's link group, de-duplicated, in one undo step.
    auto compound = std::make_unique<CompoundCommand>("Delete Clips");
    std::vector<ClipId> done;
    for (ClipId clip : sel) {
        std::size_t track = 0;
        if (!m_project.sequence().findClip(clip, &track)) {
            continue;
        }
        for (const auto& [t, id] : editTargets(m_project, track, clip)) {
            if (std::find(done.begin(), done.end(), id) != done.end()) {
                continue;
            }
            done.push_back(id);
            compound->add(std::make_unique<RemoveClipCommand>(t, id));
        }
    }
    if (m_commands.execute(m_project, std::move(compound))) {
        m_timeline->clearSelection();
        commitEdit();
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

    // Keyframed values in the panel follow the playhead (only when animated + idle).
    if (m_fxHasAnim && !m_fxEditing) {
        const Tick ph = m_timeline->playhead();
        if (ph != m_lastFxPlayhead) {
            m_lastFxPlayhead = ph;
            updateEffectPanel(m_timeline->selected());
        }
    }

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
