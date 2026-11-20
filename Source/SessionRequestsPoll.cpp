#include "App.h"

#include <filesystem>
#include <string>
#include <vector>

#include "PeerNames.h"
#include "SessionRequests.h"

std::string App::RequestProjectId(const Session* const Sender, const std::string& Cwd, std::string& Err)
{
    if (Sender != nullptr)
    {
        return Sender->Rec.ProjectId;
    }
    if (Cwd.empty() == true)
    {
        Err = "no sending session or folder was given";
        return std::string();
    }
    std::error_code ErrorCode;
    for (const Project& Candidate : State_.Projects)
    {
        if (std::filesystem::equivalent(Candidate.Path, Cwd, ErrorCode) == true)
        {
            return Candidate.Id;
        }
    }
    std::string ProjectId;
    if (AddProjectPath(Cwd, Err, &ProjectId) == false)
    {
        return std::string();
    }
    return ProjectId;
}

void App::WatchRequestedStart(Session& Target)
{
    if (Target.StartedFile.empty() == true)
    {
        return;
    }
    if (Target.ReadyAt == 0)
    {
        if (Target.CurrentStatus != Status::Idle || Target.PendingPrompt.empty() == false)
        {
            return;
        }
        SessionRequests::WriteStarted(Target.StartedFile, Target.Rec.Name, ScreenshotFinder::SessionTag(Target.Rec.Id));
        Target.ReadyAt = Now_;
        return;
    }
    if (Target.CurrentStatus != Status::Idle)
    {
        Target.StartedFile.clear();
        return;
    }
    if (Target.NoTaskWarned == false && Now_ - Target.ReadyAt > NO_TASK_WARN_SECONDS)
    {
        Target.NoTaskWarned = true;
        ShowToast("'" + Target.Rec.Name + "' is ready but has not received a task yet. Its parent may need to resend it.", true);
    }
}

void App::PollSessionRequests()
{
    for (const std::string& DoneTag : SessionRequests_.CollectDone())
    {
        Session* const Finished = FindSessionByTag(DoneTag);
        if (Finished != nullptr && Finished->Rec.ParentId.empty() == false)
        {
            Finished->ClosePending = true;
        }
    }
    const std::vector<SessionRequests::Request> Requests = SessionRequests_.Collect();
    if (Requests.empty() == true)
    {
        return;
    }
    std::map<std::string, std::vector<std::string>> StartedBySender;
    for (const SessionRequests::Request& Current : Requests)
    {
        if (Current.Error.empty() == false)
        {
            ShowToast("Ignored a new-session request (" + Current.File + "): " + Current.Error, true);
            continue;
        }
        const Session* const Sender = FindSessionByTag(Current.From);
        std::string Err;
        const std::string ProjectId = RequestProjectId(Sender, Current.Cwd, Err);
        if (ProjectId.empty() == true)
        {
            ShowToast("Could not start requested session '" + Current.Name + "': " + Err, true);
            continue;
        }
        Session* const Fresh = CreateSession(ProjectId, Current.Name, false, false, Err);
        if (Fresh == nullptr)
        {
            ShowToast("Could not start requested session '" + Current.Name + "': " + Err, true);
            continue;
        }
        std::error_code ErrorCode;
        if (Current.Cwd.empty() == false && std::filesystem::is_directory(Current.Cwd, ErrorCode) == true)
        {
            Fresh->Rec.Cwd = Current.Cwd;
        }
        if (Sender != nullptr)
        {
            Fresh->Rec.ParentId = Sender->Rec.Id;
            Fresh->Rec.Model = Sender->Rec.Model;
            const std::map<std::string, std::shared_ptr<TranscriptScanner>>::const_iterator ParentScanner = TranscriptScanners_.find(Sender->Rec.Id);
            if (Fresh->Rec.Model.empty() == true && ParentScanner != TranscriptScanners_.end())
            {
                Fresh->Rec.Model = ParentScanner->second->CurrentModel();
            }
        }
        Fresh->StartedFile = Current.File;
        if (Current.Prompt.empty() == false)
        {
            Fresh->PendingPrompt = Current.Prompt;
        }
        Fresh->Rec.WasRunning = true;  // TickSessions launches it in the background without taking focus
        State_.Ui.Collapsed[ProjectId] = false;
        StartedBySender[Sender != nullptr ? Sender->Rec.Name : std::string("A session")].push_back(Fresh->Rec.Name);
    }
    MarkDirty();
    for (const std::pair<const std::string, std::vector<std::string>>& Entry : StartedBySender)
    {
        std::string Names;
        for (const std::string& Name : Entry.second)
        {
            Names += (Names.empty() == true ? "" : ", ") + Name;
        }
        const size_t Count = Entry.second.size();
        ShowToast(Entry.first + " started " + std::to_string(Count) + " new session" + (Count == 1 ? "" : "s") + ": " + Names);
    }
}
