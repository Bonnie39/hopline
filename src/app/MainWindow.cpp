#include "app/MainWindow.h"

#include <QDockWidget>
#include <QFileDialog>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QProxyStyle>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "app/PreviewWidget.h"
#include "media/MediaProbe.h"

namespace hopline {
namespace {

// Slider is integer-valued; this is its subdivision of the whole file.
constexpr int kSeekResolution = 10000;

// A click on the groove should jump to that position, not page toward it.
class AbsoluteSliderStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* returnData) const override
    {
        if (hint == SH_Slider_AbsoluteSetButtons) {
            return Qt::LeftButton;
        }
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
};

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

    m_seekBar = new QSlider(Qt::Horizontal, this);
    m_seekBar->setRange(0, kSeekResolution);
    m_seekBar->setEnabled(false);

    auto* sliderStyle = new AbsoluteSliderStyle;
    sliderStyle->setParent(m_seekBar);
    m_seekBar->setStyle(sliderStyle);

    // valueChanged, not sliderMoved: clicks and keyboard also have to seek.
    // tick() blocks signals when it writes the position back, so this only ever
    // fires for user input.
    connect(m_seekBar, &QSlider::valueChanged, this, &MainWindow::seekBarMoved);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_preview, 1);
    layout->addWidget(m_seekBar);
    setCentralWidget(central);

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
    playbackMenu->addSeparator();
    playbackMenu->addAction("Back &5s", Qt::Key_Left, this, [this] { seekRelative(-5.0); });
    playbackMenu->addAction("Forward &5s", Qt::Key_Right, this, [this] { seekRelative(5.0); });
    playbackMenu->addAction("&Restart", Qt::Key_Home, this, [this] { seekRelative(-1e9); });

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

    m_seekBar->setEnabled(true);
    m_seekBar->setValue(0);
    statusBar()->showMessage(QString("Loaded %1  —  space to play").arg(path));
}

void MainWindow::seekRelative(double seconds)
{
    if (m_player) {
        m_player->seek(m_player->position() + seconds);
    }
}

void MainWindow::seekBarMoved(int value)
{
    if (m_player && m_player->duration() > 0.0) {
        m_player->seek(m_player->duration() * value / kSeekResolution);
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

    // Don't fight the user's drag, and don't let our own write look like input.
    if (!m_seekBar->isSliderDown() && m_player->duration() > 0.0) {
        const QSignalBlocker blocker(m_seekBar);
        m_seekBar->setValue(static_cast<int>(m_player->position() / m_player->duration() * kSeekResolution));
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
