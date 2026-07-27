#include "app/PreferencesDialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "app/ShortcutManager.h"

namespace hopline {

PreferencesDialog::PreferencesDialog(ShortcutManager* manager, QWidget* parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle("Preferences");

    auto* root = new QVBoxLayout(this);
    auto* heading = new QLabel("Keyboard Shortcuts", this);
    QFont hf = heading->font();
    hf.setBold(true);
    heading->setFont(hf);
    root->addWidget(heading);
    root->addWidget(new QLabel("Click a field and press a key combination to rebind it.", this));

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);

    QString currentCategory;
    QFormLayout* form = nullptr;
    for (const auto& cmd : m_manager->commands()) {
        if (cmd.category != currentCategory) {
            currentCategory = cmd.category;
            auto* group = new QGroupBox(currentCategory, content);
            form = new QFormLayout(group);
            contentLayout->addWidget(group);
        }
        auto* edit = new QKeySequenceEdit(cmd.action->shortcut(), content);
        edit->setMaximumSequenceLength(1);  // one combo per command
        QString label = cmd.label;
        label.remove('&');  // drop menu mnemonics for display
        form->addRow(label, edit);
        m_rows.push_back({ cmd.id, edit, cmd.defaultSeq });
    }
    contentLayout->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* reset = buttons->addButton("Restore Defaults", QDialogButtonBox::ResetRole);
    connect(reset, &QPushButton::clicked, this, &PreferencesDialog::restoreDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::applyAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    resize(440, 500);
}

void PreferencesDialog::restoreDefaults()
{
    for (const Row& r : m_rows) {
        r.edit->setKeySequence(r.defaultSeq);
    }
}

void PreferencesDialog::applyAndAccept()
{
    // Reject duplicate non-empty bindings, or Qt would make both commands ambiguous (neither fires).
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const QKeySequence a = m_rows[i].edit->keySequence();
        if (a.isEmpty()) {
            continue;
        }
        for (std::size_t j = i + 1; j < m_rows.size(); ++j) {
            if (m_rows[j].edit->keySequence() == a) {
                QMessageBox::warning(
                    this, "Shortcut Conflict",
                    QString("\"%1\" is assigned to more than one command. Give each a unique shortcut.")
                        .arg(a.toString(QKeySequence::NativeText)));
                return;
            }
        }
    }
    for (const Row& r : m_rows) {
        m_manager->setSequence(r.id, r.edit->keySequence());
    }
    m_manager->save();
    accept();
}

}  // namespace hopline
