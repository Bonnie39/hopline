#pragma once

#include <functional>
#include <vector>

#include <QKeySequence>
#include <QString>

class QAction;
class QWidget;

namespace hopline {

// Central registry of user-customizable keyboard shortcuts. Every command is backed by a QAction
// so menu items and bare shortcuts are handled uniformly; overrides persist to QSettings (group
// "shortcuts") and are applied on creation. The Preferences dialog edits bindings through this.
class ShortcutManager {
public:
    struct Command {
        QString id;        // stable key for QSettings
        QString category;  // for grouping in the dialog
        QString label;     // menu text (may contain '&' mnemonics)
        QKeySequence defaultSeq;
        QAction* action = nullptr;
    };

    explicit ShortcutManager(QWidget* window)
        : m_window(window)
    {
    }

    // Creates the QAction for `handler`, applies any saved override, and returns it. Pass inMenu
    // true when the caller will add it to a QMenu (the menu keeps the shortcut live); false for a
    // bare shortcut (the action is added to the window so the key fires window-wide).
    QAction* add(const QString& id, const QString& category, const QString& label,
                 const QKeySequence& defaultSeq, std::function<void()> handler, bool inMenu);

    const std::vector<Command>& commands() const { return m_commands; }
    void setSequence(const QString& id, const QKeySequence& seq);  // apply live (not persisted)
    void save() const;  // write current bindings (only those differing from default) to QSettings

private:
    QWidget* m_window;
    std::vector<Command> m_commands;
};

}  // namespace hopline
