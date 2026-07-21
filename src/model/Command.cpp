#include "model/Command.h"

#include "model/Project.h"

namespace hopline {

bool CommandStack::execute(Project& project, CommandPtr command)
{
    if (!command || !command->apply(project)) {
        return false;
    }
    m_done.push_back(std::move(command));
    m_undone.clear();  // a new edit invalidates the redo branch
    return true;
}

void CommandStack::undo(Project& project)
{
    if (m_done.empty()) {
        return;
    }
    CommandPtr command = std::move(m_done.back());
    m_done.pop_back();
    command->undo(project);
    m_undone.push_back(std::move(command));
}

void CommandStack::redo(Project& project)
{
    if (m_undone.empty()) {
        return;
    }
    CommandPtr command = std::move(m_undone.back());
    m_undone.pop_back();
    if (!command->apply(project)) {
        return;  // dropping it beats leaving the stacks out of step with the project
    }
    m_done.push_back(std::move(command));
}

void CommandStack::clear()
{
    m_done.clear();
    m_undone.clear();
}

std::string CommandStack::undoName() const
{
    return m_done.empty() ? std::string() : m_done.back()->name();
}

std::string CommandStack::redoName() const
{
    return m_undone.empty() ? std::string() : m_undone.back()->name();
}

}  // namespace hopline
