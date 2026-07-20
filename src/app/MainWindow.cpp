#include "app/MainWindow.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTimer>

#include "app/PreviewWidget.h"
#include "media/MediaProbe.h"

namespace hopline {
namespace {

QString formatDuration(double seconds)
{
    const int total = static_cast<int>(seconds);
    return QString("%1:%2:%3")
        .arg(total / 3600, 2, 10, QChar('0'))
        .arg((total / 60) % 60, 2, 10, QChar('0'))
        .arg(total % 60, 2, 10, QChar('0'));
}

QString describe(const MediaInfo& info)
{
    QString text;
    text += QString("%1\n").arg(QString::fromStdString(info.path));
    text += QString("  format   %1\n").arg(QString::fromStdString(info.formatName));
    text += QString("  duration %1 (%2s)\n").arg(formatDuration(info.duration)).arg(info.duration, 0, 'f', 3);
    text += QString("  bitrate  %1 kb/s\n").arg(info.bitRate / 1000);

    for (const StreamInfo& s : info.streams) {
        if (s.type == "video") {
            text += QString("  [%1] video  %2  %3x%4  %5 fps\n")
                        .arg(s.index)
                        .arg(QString::fromStdString(s.codec))
                        .arg(s.width)
                        .arg(s.height)
                        .arg(s.frameRate, 0, 'f', 3);
        } else if (s.type == "audio") {
            text += QString("  [%1] audio  %2  %3 Hz  %4 ch\n")
                        .arg(s.index)
                        .arg(QString::fromStdString(s.codec))
                        .arg(s.sampleRate)
                        .arg(s.channels);
        } else {
            text += QString("  [%1] %2  %3\n")
                        .arg(s.index)
                        .arg(QString::fromStdString(s.type))
                        .arg(QString::fromStdString(s.codec));
        }
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
    setCentralWidget(m_preview);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    auto* logDock = new QDockWidget("Media Info", this);
    logDock->setWidget(m_log);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);

    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&Open...", QKeySequence::Open, this, &MainWindow::openFile);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", QKeySequence::Quit, this, &QWidget::close);

    auto* playbackMenu = menuBar()->addMenu("&Playback");
    playbackMenu->addAction("&Play/Pause", Qt::Key_Space, this, &MainWindow::togglePlay);

    // Poll well above frame rate; the clock decides what's actually due.
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(4);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::tick);
    m_timer->start();

    statusBar()->showMessage("Ready");
}

MainWindow::~MainWindow() = default;

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, "Open Media", QString(),
        "Media Files (*.mp4 *.mov *.mkv *.avi *.webm *.wav *.mp3 *.flac);;All Files (*)");

    if (!path.isEmpty()) {
        load(path);
    }
}

void MainWindow::load(const QString& path)
{
    std::string error;
    const auto info = probeMedia(path.toStdString(), error);
    if (!info) {
        m_log->appendPlainText(QString("failed: %1\n  %2").arg(path, QString::fromStdString(error)));
        statusBar()->showMessage("Failed to open");
        return;
    }

    m_log->appendPlainText(describe(*info));

    m_player = std::make_unique<Player>();
    if (!m_player->open(path.toStdString(), error)) {
        m_log->appendPlainText(QString("decoder: %1").arg(QString::fromStdString(error)));
        m_player.reset();
        m_preview->clear();
        statusBar()->showMessage("No video stream");
        return;
    }

    statusBar()->showMessage(QString("Loaded %1  —  space to play").arg(path));
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

    // One extra refresh after stopping, so the final position actually gets painted.
    const bool playing = m_player->isPlaying();
    if (playing || m_wasPlaying) {
        statusBar()->showMessage(QString("%1 / %2 s   dropped %3   underruns %4   clock: %5")
                                     .arg(m_player->position(), 0, 'f', 2)
                                     .arg(m_player->duration(), 0, 'f', 2)
                                     .arg(m_player->droppedFrames())
                                     .arg(m_player->underruns())
                                     .arg(m_player->hasAudio() ? "audio" : "wall"));
    }
    m_wasPlaying = playing;
}

}  // namespace hopline
