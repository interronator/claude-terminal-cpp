#include "App.h"

#include "Util.h"

void App::BeginNewSession(const std::string& ProjectId)
{
    if (State_.AppSettings.AskSessionName == true)
    {
        ModalTarget_ = ProjectId;
        OpenModal(Modal::NewSession);
        return;
    }
    std::string Err;
    Session* const NewSession = CreateSession(ProjectId, "", false, false, Err);
    if (NewSession == nullptr)
    {
        ShowToast("Could not start a session: " + Err, true);
        return;
    }
    NewSession->Rec.AutoTitle = true;
    State_.Ui.Collapsed[ProjectId] = false;
    OpenSession(NewSession->Rec.Id);
}

void App::StartSessionWithOptions(const std::string& ProjectId, const bool Worktree, const bool ContinueLast)
{
    std::string Err;
    Session* const NewSession = CreateSession(ProjectId, "", Worktree, ContinueLast, Err);
    if (NewSession == nullptr)
    {
        ShowToast("Could not start a session: " + Err, true);
        return;
    }
    State_.Ui.Collapsed[ProjectId] = false;
    OpenSession(NewSession->Rec.Id);
}

void App::DrawNewSessionChoices(const Project& CurrentProject)
{
    if (ImGui::MenuItem("New session") == true)
    {
        BeginNewSession(CurrentProject.Id);
    }
    if (ImGui::MenuItem("New session (choose name/options...)") == true)
    {
        ModalTarget_ = CurrentProject.Id;
        OpenModal(Modal::NewSession);
    }
    if (ImGui::MenuItem("New worktree session") == true)
    {
        StartSessionWithOptions(CurrentProject.Id, true, false);
    }
    if (ImGui::MenuItem("Continue last conversation") == true)
    {
        StartSessionWithOptions(CurrentProject.Id, false, true);
    }
}

void App::AutoTitleTick(Session& CurrentSession)
{
    if (CurrentSession.Rec.AutoTitle == false || Titler_.IsPending(CurrentSession.Rec.Id) == true)
    {
        return;
    }
    const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator Scanner = TranscriptScanners_.find(CurrentSession.Rec.Id);
    if (Scanner == TranscriptScanners_.end() || SessionTitler::IsUsablePrompt(Scanner->second->FirstUserPrompt()) == false)
    {
        return;
    }
    std::vector<std::string> Argv = Util::SplitCommand(State_.AppSettings.ClaudeCommand.empty() == true ? "claude" : State_.AppSettings.ClaudeCommand);
    if (Argv.empty() == true)
    {
        Argv.push_back("claude");
    }
    Titler_.Request(Argv, CurrentSession.Rec.Id, CurrentSession.Rec.Name, Scanner->second->FirstUserPrompt());
}

void App::ApplyAutoTitles()
{
    for (const SessionTitler::Finished& Done : Titler_.TakeFinished())
    {
        Session* const Target = FindSession(Done.SessionId);
        if (Target == nullptr)
        {
            continue;
        }
        Target->Rec.AutoTitle = false;
        if (Done.Title.empty() == false && Target->Rec.Name == Done.OriginalName)
        {
            Target->Rec.Name = Done.Title;
        }
        MarkDirty();
    }
}
