#include "App.h"

void App::WatchBackgroundTasks(Session& CurrentSession)
{
    BackgroundWatch_.NoteBusy(CurrentSession.Rec.Id, CurrentSession.IsRunning() == true && CurrentSession.CurrentStatus == Status::Busy);
    if (CurrentSession.IsRunning() == true && CurrentSession.CurrentStatus == Status::Busy)
    {
        const std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>>::const_iterator Overdue = OverdueShells_.find(CurrentSession.Rec.Id);
        if (Overdue == OverdueShells_.end())
        {
            return;
        }
        const std::string HungPrompt = BackgroundWatch_.CheckHung(CurrentSession.Rec.Id, Overdue->second);
        if (HungPrompt.empty() == true)
        {
            return;
        }
        CurrentSession.PendingPrompt = HungPrompt;
        CurrentSession.SendText("\x1b", Now_, StatusCallback());
        ShowToast("Interrupted " + CurrentSession.Rec.Name + ": its command looked hung.");
        return;
    }
    if (CurrentSession.IsRunning() == false || CurrentSession.CurrentStatus != Status::Idle || CurrentSession.InBackground() == false ||
        CurrentSession.PendingPrompt.empty() == false)
    {
        return;
    }
    double& NextCheck = BackgroundCheckAt_[CurrentSession.Rec.Id];
    if (Now_ < NextCheck)
    {
        return;
    }
    NextCheck = Now_ + BackgroundWatch::CHECK_INTERVAL_SECONDS;
    const std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>>::const_iterator Found = RunningCommands_.find(CurrentSession.Rec.Id);
    if (Found == RunningCommands_.end())
    {
        return;
    }
    const std::string Reminder = BackgroundWatch_.Check(CurrentSession.Rec.Id, Found->second);
    if (Reminder.empty() == true)
    {
        return;
    }
    SendWhenSafe(CurrentSession, Reminder);
    ShowToast("Reminded " + CurrentSession.Rec.Name + " to check its quiet background command.");
}
