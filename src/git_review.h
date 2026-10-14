#pragma once
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "git.h"

class GitReview
{
public:
    enum class Action { None, Revert, UndoCommit, DiscardCommit, DiscardChanges };

    struct Entry
    {
        std::string Hash;
        std::string ShortHash;
        std::string Subject;
        std::string Author;
        std::string When;
        bool IsWorkingTree = false;
        bool Unpushed = false;
    };

    std::string Cwd;
    std::string Error;
    std::string Upstream;
    std::vector<Entry> Entries;
    std::vector<std::string> DiffLines;
    std::vector<std::string> ActivityLog;
    int Selected = -1;
    int HeadIndex = -1;
    Action Pending = Action::None;

    void Load(const std::string& NewCwd)
    {
        Cwd = NewCwd;
        Error.clear();
        Upstream.clear();
        Entries.clear();
        DiffLines.clear();
        Selected = -1;
        HeadIndex = -1;
        Pending = Action::None;

        const ProcResult UpstreamResult = Git::RunProcess({"git", "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"}, Cwd);
        if (UpstreamResult.Code == 0)
        {
            Upstream = Trim(UpstreamResult.Output);
        }

        const ProcResult StatusResult = Git::RunProcess({"git", "status", "--porcelain"}, Cwd);
        if (StatusResult.Code != 0)
        {
            Error = "This session's folder is not a git repository.";
            return;
        }
        if (Trim(StatusResult.Output).empty() == false)
        {
            Entry WorkingTree;
            WorkingTree.IsWorkingTree = true;
            WorkingTree.Subject = "Uncommitted changes";
            Entries.push_back(WorkingTree);
        }

        std::set<std::string> UnpushedHashes;
        if (Upstream.empty() == false)
        {
            for (const std::string& Line : Split(Git::RunProcess({"git", "rev-list", Upstream + "..HEAD"}, Cwd).Output, '\n'))
            {
                UnpushedHashes.insert(Line);
            }
        }

        const ProcResult LogResult = Git::RunProcess({"git", "log", "-100", "--format=%H%x1f%h%x1f%s%x1f%an%x1f%ar", "HEAD"}, Cwd);
        if (LogResult.Code == 0)
        {
            for (const std::string& Line : Split(LogResult.Output, '\n'))
            {
                const std::vector<std::string> Fields = Split(Line, '\x1f');
                if (Fields.size() < 5)
                {
                    continue;
                }
                Entry Commit;
                Commit.Hash = Fields[0];
                Commit.ShortHash = Fields[1];
                Commit.Subject = Fields[2];
                Commit.Author = Fields[3];
                Commit.When = Fields[4];
                Commit.Unpushed = UnpushedHashes.count(Commit.Hash) > 0;
                if (HeadIndex < 0)
                {
                    HeadIndex = static_cast<int>(Entries.size());
                }
                Entries.push_back(Commit);
            }
        }

        if (Entries.empty() == false)
        {
            Select(0);
        }
    }

    void Select(const int Index)
    {
        if (Index < 0 || Index >= (int)Entries.size())
        {
            return;
        }
        Selected = Index;
        const Entry& Chosen = Entries[(size_t)Index];
        std::string Text;
        if (Chosen.IsWorkingTree == true)
        {
            Text = Git::RunProcess({"git", "status", "--short"}, Cwd).Output + "\n" + Git::RunProcess({"git", "diff", "HEAD"}, Cwd).Output;
        }
        else
        {
            Text = Git::RunProcess({"git", "show", "--stat", "--patch", "--format=fuller", Chosen.Hash}, Cwd).Output;
        }
        static constexpr size_t MAX_DIFF_CHARS = 400000;
        const bool Truncated = Text.size() > MAX_DIFF_CHARS;
        if (Truncated == true)
        {
            Text.resize(MAX_DIFF_CHARS);
        }
        DiffLines = Split(Text, '\n');
        if (Truncated == true)
        {
            DiffLines.push_back("... diff truncated ...");
        }
    }

    bool SelectedIsHead() const
    {
        return Selected >= 0 && Selected == HeadIndex;
    }

    const Entry* SelectedEntry() const
    {
        return Selected >= 0 && Selected < static_cast<int>(Entries.size()) ? &Entries[(size_t)Selected] : nullptr;
    }

    bool CanRevert() const
    {
        const Entry* const Chosen = SelectedEntry();
        return Chosen != nullptr && Chosen->IsWorkingTree == false;
    }

    // Only the newest commit, and only while it has not been pushed, can be rewritten safely.
    bool CanRewriteHead() const
    {
        const Entry* const Chosen = SelectedEntry();
        return Chosen != nullptr && Chosen->IsWorkingTree == false && SelectedIsHead() == true && Chosen->Unpushed == true;
    }

    bool CanDiscardChanges() const
    {
        const Entry* const Chosen = SelectedEntry();
        return Chosen != nullptr && Chosen->IsWorkingTree == true;
    }

    // Runs the pending action, records it in the activity log, reloads the history and returns a one-line outcome.
    std::string RunPending()
    {
        const Action ToRun = Pending;
        Pending = Action::None;
        const Entry* const Chosen = SelectedEntry();
        if (ToRun == Action::None || Chosen == nullptr)
        {
            return {};
        }
        const std::string Label = Chosen->IsWorkingTree == true ? std::string("uncommitted changes") : Chosen->ShortHash + " " + Chosen->Subject;
        ProcResult Result;
        std::string What;
        switch (ToRun)
        {
            case Action::Revert:
                What = "Reverted";
                Result = Git::RunProcess({"git", "revert", "--no-edit", Chosen->Hash}, Cwd);
                if (Result.Code != 0)
                {
                    Git::RunProcess({"git", "revert", "--abort"}, Cwd);
                }
                break;
            case Action::UndoCommit:
                What = "Undid commit (changes kept)";
                Result = Git::RunProcess({"git", "reset", "--soft", "HEAD~1"}, Cwd);
                break;
            case Action::DiscardCommit:
                What = "Discarded commit";
                Result = Git::RunProcess({"git", "reset", "--hard", "HEAD~1"}, Cwd);
                break;
            case Action::DiscardChanges:
                What = "Discarded";
                Result = Git::RunProcess({"git", "reset", "--hard", "HEAD"}, Cwd);
                break;
            case Action::None: break;
        }
        std::string Outcome;
        if (Result.Code == 0)
        {
            Outcome = What + ": " + Label;
        }
        else
        {
            Outcome = "Failed - " + What + " " + Label + " (" + FirstLine(Result.Output) + ")";
        }
        char Stamp[16] = {};
        const std::time_t Now = std::time(nullptr);
        std::strftime(Stamp, sizeof Stamp, "%H:%M", std::localtime(&Now));
        ActivityLog.insert(ActivityLog.begin(), std::string(Stamp) + "  " + Outcome);
        const std::string KeepCwd = Cwd;
        Load(KeepCwd);
        return Outcome;
    }

    static const char* ActionTitle(const Action Value)
    {
        switch (Value)
        {
            case Action::Revert: return "Revert this commit?";
            case Action::UndoCommit: return "Undo this commit?";
            case Action::DiscardCommit: return "Discard this commit?";
            case Action::DiscardChanges: return "Discard all uncommitted changes?";
            case Action::None: break;
        }
        return "";
    }

    static const char* ActionDetail(const Action Value)
    {
        switch (Value)
        {
            case Action::Revert: return "Adds a new commit that reverses it. History is kept, so this is safe even after pushing.";
            case Action::UndoCommit: return "Removes the commit but keeps its changes in your working folder.";
            case Action::DiscardCommit: return "Removes the commit AND throws away its changes, plus any uncommitted changes. Cannot be undone.";
            case Action::DiscardChanges: return "Throws away every uncommitted change to tracked files. Cannot be undone.";
            case Action::None: break;
        }
        return "";
    }


private:
    static std::string FirstLine(const std::string& Text)
    {
        const std::string Trimmed = Trim(Text);
        return Trimmed.substr(0, Trimmed.find('\n'));
    }

    static std::string Trim(const std::string& Text)
    {
        const size_t First = Text.find_first_not_of(" \t\r\n");
        if (First == std::string::npos)
        {
            return {};
        }
        return Text.substr(First, Text.find_last_not_of(" \t\r\n") - First + 1);
    }

    static std::vector<std::string> Split(const std::string& Text, const char Separator)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        while (Start <= Text.size())
        {
            size_t End = Text.find(Separator, Start);
            if (End == std::string::npos)
            {
                End = Text.size();
            }
            std::string Part = Text.substr(Start, End - Start);
            if (Part.empty() == false && Part.back() == '\r')
            {
                Part.pop_back();
            }
            Parts.push_back(std::move(Part));
            Start = End + 1;
        }
        return Parts;
    }
};
