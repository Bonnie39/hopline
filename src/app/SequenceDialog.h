#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QSpinBox;

namespace hopline {

// Settings chosen for a new sequence.
struct SequenceSettings {
    QString name;
    int width = 1920;
    int height = 1080;
    int rateNum = 30;
    int rateDen = 1;
};

// Modal dialog for creating a sequence: name, resolution (presets or custom),
// and frame rate.
class SequenceDialog : public QDialog {
    Q_OBJECT

public:
    explicit SequenceDialog(const QString& defaultName, QWidget* parent = nullptr);

    SequenceSettings settings() const;

private:
    void syncCustomEnabled();

    QLineEdit* m_name = nullptr;
    QComboBox* m_resolution = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QComboBox* m_rate = nullptr;
};

}  // namespace hopline
