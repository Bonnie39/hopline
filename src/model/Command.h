#pragma once

#include <memory>
#include <string>
#include <vector>

namespace hopline {

class Project;

// Every mutation of the project is a command. apply() must be exactly reversible
// by undo(): re-applying after undoing has to reproduce the same state, ids
// included, or redo will diverge.
class Command {
public:
    virtual ~Command() = default;

    virtual bool apply(Project& project) = 0;
    virtual void undo(Project& project) = 0;
    virtual std::string name() const = 0;
};

using CommandPtr = std::unique_ptr<Command>;

class CommandStack {
public:
    // Takes ownership. Returns false and discards the command if apply() failed,
    // so a rejected edit never lands on the undo stack.
    bool execute(Project& project, CommandPtr command);

    bool canUndo() const { return !m_done.empty(); }
    bool canRedo() const { return !m_undone.empty(); }

    void undo(Project& project);
    void redo(Project& project);
    void clear();

    std::string undoName() const;
    std::string redoName() const;

private:
    std::vector<CommandPtr> m_done;
    std::vector<CommandPtr> m_undone;
};

}  // namespace hopline
