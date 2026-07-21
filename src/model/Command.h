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

// Applies several commands as one undoable unit — a linked move is one move per
// group member. apply() is atomic: if any child fails, the ones that already
// applied are rolled back and the whole thing reports failure.
class CompoundCommand : public Command {
public:
    explicit CompoundCommand(std::string name)
        : m_name(std::move(name))
    {
    }

    void add(CommandPtr command) { m_commands.push_back(std::move(command)); }
    bool empty() const { return m_commands.empty(); }
    size_t size() const { return m_commands.size(); }

    bool apply(Project& project) override;
    void undo(Project& project) override;
    std::string name() const override { return m_name; }

private:
    std::vector<CommandPtr> m_commands;
    std::string m_name;
};

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
