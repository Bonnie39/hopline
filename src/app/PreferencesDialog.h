#pragma once

#include <vector>

#include <QDialog>
#include <QKeySequence>
#include <QString>

class QKeySequenceEdit;

namespace hopline {

class ShortcutManager;

// Edit ▸ Preferences: a keyboard-shortcut customizer. Lists every command the ShortcutManager
// knows (grouped by category) with a QKeySequenceEdit to rebind it; commits on OK.
class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    explicit PreferencesDialog(ShortcutManager* manager, QWidget* parent = nullptr);

private:
    void applyAndAccept();  // validate (no duplicates), apply to the manager, persist
    void restoreDefaults();

    ShortcutManager* m_manager;
    struct Row {
        QString id;
        QKeySequenceEdit* edit;
        QKeySequence defaultSeq;
    };
    std::vector<Row> m_rows;
};

}  // namespace hopline
