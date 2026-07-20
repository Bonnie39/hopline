#pragma once

#include <QMainWindow>
#include <memory>

#include "engine/Player.h"

class QPlainTextEdit;
class QSlider;
class QTimer;

namespace hopline {

class PreviewWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void load(const QString& path);

private slots:
    void openFile();
    void togglePlay();
    void tick();
    void seekRelative(double seconds);
    void seekBarMoved(int value);

private:
    QPlainTextEdit* m_log = nullptr;
    PreviewWidget* m_preview = nullptr;
    QTimer* m_timer = nullptr;
    QSlider* m_seekBar = nullptr;
    std::unique_ptr<Player> m_player;
    bool m_wasPlaying = false;
};

}  // namespace hopline
