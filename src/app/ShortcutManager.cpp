#include "app/ShortcutManager.h"

#include <QAction>
#include <QSettings>
#include <QWidget>

namespace hopline {

QAction* ShortcutManager::add(const QString& id, const QString& category, const QString& label,
                              const QKeySequence& defaultSeq, std::function<void()> handler, bool inMenu)
{
    auto* action = new QAction(label, m_window);
    action->setShortcut(defaultSeq);
    action->setShortcutContext(Qt::WindowShortcut);  // fires when the window/child has focus
    QObject::connect(action, &QAction::triggered, m_window, [h = std::move(handler)] { h(); });
    if (!inMenu) {
        m_window->addAction(action);  // a menu item is already live; a bare shortcut needs this
    }

    QSettings settings;
    settings.beginGroup("shortcuts");
    if (settings.contains(id)) {
        action->setShortcut(QKeySequence(settings.value(id).toString()));
    }
    settings.endGroup();

    m_commands.push_back({ id, category, label, defaultSeq, action });
    return action;
}

void ShortcutManager::setSequence(const QString& id, const QKeySequence& seq)
{
    for (const Command& c : m_commands) {
        if (c.id == id) {
            c.action->setShortcut(seq);
            return;
        }
    }
}

void ShortcutManager::save() const
{
    QSettings settings;
    settings.beginGroup("shortcuts");
    settings.remove("");  // clear stale keys so a reset-to-default doesn't linger
    for (const Command& c : m_commands) {
        if (c.action->shortcut() != c.defaultSeq) {
            settings.setValue(c.id, c.action->shortcut().toString());
        }
    }
    settings.endGroup();
}

}  // namespace hopline
