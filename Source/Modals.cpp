#include "App.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <thread>

#include "Git.h"
#include "BuildScripts.h"
#include "ImageCache.h"
#include "Platform.h"
#include "Style.h"
#include "UiHelpers.h"
#include "Util.h"

namespace fs = std::filesystem;


// ---------------------------------------------------------------------------------------------
// modals + toasts
// ---------------------------------------------------------------------------------------------

void App::DrawModals()
{
    if (Modal_ == Modal::None)
    {
        return;
    }

    const char* Title = "";
    switch (Modal_)
    {
        case Modal::AddProject: Title = "Add project"; break;
        case Modal::NewSession: Title = "New Claude session"; break;
        case Modal::Rename: Title = "Rename"; break;
        case Modal::Confirm: Title = ConfirmTitle_.c_str(); break;
        case Modal::CloseSession: Title = "Close session"; break;
        case Modal::Settings: Title = "Settings"; break;
        case Modal::Usage: Title = "Usage"; break;
        case Modal::GitHub: Title = "GitHub"; break;
        case Modal::Search: Title = "Search"; break;
        case Modal::Review: Title = "GitHub review"; break;
        default: break;
    }
    std::string const PopupId = std::string(Title) + "###modal";
    if (ModalJustOpened_)
    {
        ImGui::OpenPopup(PopupId.c_str());
        ModalJustOpened_ = false;
        ModalError_.clear();
        if (Modal_ == Modal::NewSession)
        {
            NameBuffer_[0] = 0;
            WantWorktree_ = false;
            WantContinue_ = false;
            ProjectComboIndex_ = 0;
            for (size_t Index = 0; Index < State_.Projects.size(); Index++)
            {
                if (State_.Projects[Index].Id == ModalTarget_)
                {
                    ProjectComboIndex_ = (int)Index;
                }
            }
            GitCache_ = GitCache{};
        }
        if (Modal_ == Modal::Usage)
        {
            StartUsageFetch();
        }
    }

    const float Scale = UiScale_;
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    const bool Resizable = Modal_ == Modal::Settings || Modal_ == Modal::Review;
    if (Resizable == true)
    {
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        const float ModalWidth = Modal_ == Modal::Review ? 1000.0f : 560.0f;
        ImGui::SetNextWindowSize(ImVec2(std::min(ModalWidth * Scale, Viewport->WorkSize.x * 0.95f), std::min(720 * Scale, Viewport->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(460 * Scale, 320 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
    }
    else
    {
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2((Modal_ == Modal::Search ? 620 : 460) * Scale, 0));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * Scale, 14 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * Scale, 9 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
    bool Open = true;
    const ImGuiWindowFlags ModalFlags = ImGuiWindowFlags_NoSavedSettings | (Resizable == true ? 0 : ImGuiWindowFlags_NoResize);
    if (ImGui::BeginPopupModal(PopupId.c_str(), &Open, ModalFlags))
    {
        const bool EscapeBelongsToCapture = KeyCaptureIndex_ >= 0 || ImGui::GetFrameCount() == KeyCaptureEscapeFrame_;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && EscapeBelongsToCapture == false)
        {
            CloseModal();
        }
        const bool Enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

        switch (Modal_)
        {
            case Modal::AddProject: DrawAddProjectModal(Enter); break;
            case Modal::NewSession: DrawNewSessionModal(Enter); break;
            case Modal::Rename: DrawRenameModal(Enter); break;
            case Modal::Confirm: DrawConfirmModal(); break;
            case Modal::CloseSession: DrawCloseSessionModal(); break;
            case Modal::Settings: DrawSettingsModal(); break;
            case Modal::Usage: DrawUsageModal(); break;
            case Modal::GitHub: DrawGitHubModal(); break;
            case Modal::Review: DrawReviewModal(); break;
            case Modal::Search: DrawSearch(Enter); break;
            default: break;
        }
        ImGui::EndPopup();
    }
    else if (Open == false)
    {
        Modal_ = Modal::None;
    }
    ImGui::PopStyleVar(3);
}

void App::DrawAddProjectModal(const bool Enter)
{
    const float Scale = UiScale_;
    const float Full = -1;
    ImGui::TextDisabled("Project folder");
    ImGui::SetNextItemWidth(Platform::CanPickFolder() ? -80 * Scale : Full);
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##path", PathBuffer_, sizeof PathBuffer_);
    if (Platform::CanPickFolder())
    {
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            std::string const Picked = Platform::PickFolder(Window_);
            if (Picked.empty() == false)
            {
                std::snprintf(PathBuffer_, sizeof PathBuffer_, "%s", Picked.c_str());
            }
        }
    }
    ImGui::TextDisabled("You can also drag a folder onto the window.");
    if (ModalError_.empty() == false)
    {
        ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", ModalError_.c_str());
    }
    if (ImGui::Button("Add project") || Enter)
    {
        std::string Id;
        if (AddProjectPath(PathBuffer_, ModalError_, &Id))
        {
            State_.Ui.Collapsed[Id] = false;
            ModalTarget_ = Id;
            OpenModal(Modal::NewSession);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        CloseModal();
    }
}

void App::DrawNewSessionModal(const bool Enter)
{
    const float Full = -1;
    if (State_.Projects.empty())
    {
        CloseModal();
        return;
    }
    ProjectComboIndex_ = std::clamp(ProjectComboIndex_, 0, (int)State_.Projects.size() - 1);
    ImGui::TextDisabled("Project");
    ImGui::SetNextItemWidth(Full);
    if (ImGui::BeginCombo("##proj", State_.Projects[(size_t)ProjectComboIndex_].Name.c_str()))
    {
        for (int Index = 0; Index < (int)State_.Projects.size(); Index++)
        {
            if (ImGui::Selectable(State_.Projects[(size_t)Index].Name.c_str(), Index == ProjectComboIndex_))
            {
                ProjectComboIndex_ = Index;
            }
        }
        ImGui::EndCombo();
    }
    const Project& CurrentProject = State_.Projects[(size_t)ProjectComboIndex_];
    if (GitCache_.Path != CurrentProject.Path)
    {
        Git::Info const CurrentInfo = Git::GetInfo(CurrentProject.Path);
        GitCache_ = {CurrentProject.Path, CurrentInfo.IsGitRepo, CurrentInfo.Branch};
        if (CurrentInfo.IsGitRepo == false)
        {
            WantWorktree_ = false;
        }
    }
    ImGui::TextDisabled("Session name (optional)");
    ImGui::SetNextItemWidth(Full);
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##name", "e.g. Fix login bug", NameBuffer_, sizeof NameBuffer_);
    ImGui::BeginDisabled(GitCache_.IsGitRepo == false);
    ImGui::Checkbox("Isolate in a new git worktree", &WantWorktree_);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", GitCache_.IsGitRepo
                                  ? ("Branches from " + (GitCache_.Branch.empty() ? std::string("HEAD") : GitCache_.Branch) +
                                     " into .claude/worktrees/, so parallel sessions never touch each other's files.")
                                        .c_str()
                                  : "Not a git repository - worktrees unavailable.");
    ImGui::PopTextWrapPos();
    if (WantWorktree_ == true)
    {
        WantContinue_ = false;
    }
    ImGui::BeginDisabled(WantWorktree_ == true);
    ImGui::Checkbox("Continue the last conversation in this folder", &WantContinue_);
    ImGui::EndDisabled();
    if (ModalError_.empty() == false)
    {
        ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", ModalError_.c_str());
    }
    if (ImGui::Button("Start session") || Enter)
    {
        std::string Err;
        Session* const NewSession = CreateSession(CurrentProject.Id, NameBuffer_, WantWorktree_ && GitCache_.IsGitRepo, WantContinue_, Err);
        if (NewSession != nullptr)
        {
            State_.Ui.Collapsed[CurrentProject.Id] = false;
            OpenSession(NewSession->Rec.Id);
            CloseModal();
        }
        else
        {
            ModalError_ = Err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        CloseModal();
    }
}

void App::DrawRenameModal(const bool Enter)
{
    const float Full = -1;
    ImGui::TextDisabled(RenameIsProject_ ? "Project name" : "Session name");
    ImGui::SetNextItemWidth(Full);
    if (ImGui::IsWindowAppearing())
    {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##rename", NameBuffer_, sizeof NameBuffer_);
    if (ImGui::Button("Save") || Enter)
    {
        std::string const Name = Util::Trim(NameBuffer_);
        if (Name.empty() == false)
        {
            if (RenameIsProject_)
            {
                if (Project* const CurrentProject = FindProject(ModalTarget_))
                {
                    CurrentProject->Name = Name;
                }
            }
            else if (Session* const CurrentSession = FindSession(ModalTarget_))
            {
                CurrentSession->Rec.Name = Name;
            }
            MarkDirty();
        }
        CloseModal();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        CloseModal();
    }
}

void App::DrawConfirmModal()
{
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(ConfirmText_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.24f, 1));
    if (ImGui::Button("Confirm"))
    {
        std::function<void()> const YesAction = ConfirmYesAction_;
        CloseModal();
        if (YesAction)
        {
            YesAction();
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        CloseModal();
    }
}

void App::DrawCloseSessionModal()
{
    Session* const CurrentSession = FindSession(ModalTarget_);
    if (CurrentSession == nullptr || CurrentSession->Rec.WorktreeInfo.has_value() == false)
    {
        CloseModal();
        return;
    }
    ImGui::PushTextWrapPos(0);
    ImGui::Text("\"%s\" works in its own git worktree on branch %s.", CurrentSession->Rec.Name.c_str(),
               CurrentSession->Rec.WorktreeInfo->Branch.c_str());
    if (CloseDirty_)
    {
        ImGui::TextColored(ImVec4(0.9f, 0.64f, 0.24f, 1), "The worktree has uncommitted changes.");
    }
    ImGui::PopTextWrapPos();
    ImGui::RadioButton("Keep the worktree and branch", &CloseChoice_, 0);
    ImGui::BeginDisabled(CloseDirty_);
    ImGui::RadioButton("Remove the worktree (branch kept if it has unmerged commits)", &CloseChoice_, 1);
    ImGui::EndDisabled();
    ImGui::RadioButton("Remove and discard all changes", &CloseChoice_, 2);
    if (CloseDirty_ && CloseChoice_ == 1)
    {
        CloseChoice_ = 0;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.24f, 1));
    if (ImGui::Button("Close session"))
    {
        std::string const Id = CurrentSession->Rec.Id;
        int const Choice = CloseChoice_;
        CloseModal();
        CloseSession(Id, Choice);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        CloseModal();
    }
}

void App::DrawSettingsModal()
{
    const float Scale = UiScale_;
    const float Full = -1;
    ImGui::BeginChild("##settingsScroll", ImVec2(0, -(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_None);
    ImGui::TextDisabled("Claude command (add flags, e.g.  claude --model opus)");
    ImGui::SetNextItemWidth(Full);
    ImGui::InputText("##cmd", CommandBuffer_, sizeof CommandBuffer_);
    ImGui::TextDisabled("Terminal font size");
    ImGui::SetNextItemWidth(Full);
    ImGui::SliderInt("##fs", &FontSizeEdit_, 8, 32);
    ImGui::TextDisabled("Session title text size");
    ImGui::SetNextItemWidth(Full);
    ImGui::SliderInt("##pts", &PaneTitleScaleEdit_, 70, 200, "%d%%");
    ImGui::Checkbox("Desktop notifications when a background session needs you", &NotifyEdit_);
    ImGui::Checkbox("Start the 5-hour usage window as soon as it is free (sends a tiny Haiku message)", &AutoStartEdit_);
    ImGui::Checkbox("Use GitHub access (shows login status; needs the gh CLI)", &GitHubEdit_);
    ImGui::TextDisabled("Command changes apply the next time a session starts.");

    ImGui::Separator();
    ImGui::TextUnformatted("Sessions");
    ImGui::BeginChild("##sessionNames", ImVec2(Full, 150 * Scale), ImGuiChildFlags_Borders);
    for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
    {
        Session const& CurrentSession = *SessionPtr;
        ImGui::PushID(CurrentSession.Rec.Id.c_str());
        Project* const CurrentProject = FindProject(CurrentSession.Rec.ProjectId);
        ImGui::TextDisabled("%s", CurrentProject != nullptr ? CurrentProject->Name.c_str() : "?");
        ImGui::SameLine(130 * Scale);
        ImGui::SetNextItemWidth(Full - 130 * Scale);
        std::array<char, 256>& Buffer = SessionNameEdits_[CurrentSession.Rec.Id];
        ImGui::InputText("##name", Buffer.data(), Buffer.size());
        ImGui::PopID();
    }
    if (Sessions_.empty())
    {
        ImGui::TextDisabled("No sessions yet.");
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextUnformatted("Theme");
    ImGui::SameLine();
    if (ImGui::SmallButton("Dark"))
    {
        ThemeEdit_ = ThemeColors::Dark();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Light"))
    {
        ThemeEdit_ = ThemeColors::Light();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Midnight"))
    {
        ThemeEdit_ = ThemeColors::Midnight();
    }
    ImGui::BeginChild("##themeColors", ImVec2(Full, 260 * Scale), ImGuiChildFlags_Borders);
    const std::function<void(const char*, ThemeColor&)> ColorRow = [&](const char* Label, ThemeColor& CurrentColor)
    { ImGui::ColorEdit4(Label, &CurrentColor.Red, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs); };
    ColorRow("Background", ThemeEdit_.Background);
    ColorRow("Surface (sidebar/toolbar)", ThemeEdit_.Surface);
    ColorRow("Pane background", ThemeEdit_.PaneBg);
    ColorRow("Pane border", ThemeEdit_.PaneBorder);
    ColorRow("Accent", ThemeEdit_.Accent);
    ColorRow("Text", ThemeEdit_.Text);
    ColorRow("Text (disabled)", ThemeEdit_.TextDisabled);
    ColorRow("Status: busy", ThemeEdit_.StatusBusy);
    ColorRow("Status: waiting", ThemeEdit_.StatusWaiting);
    ColorRow("Status: done", ThemeEdit_.StatusDone);
    ColorRow("Status: idle", ThemeEdit_.StatusIdle);
    ColorRow("Usage: warning", ThemeEdit_.UsageWarning);
    ColorRow("Usage: full", ThemeEdit_.UsageFull);
    ColorRow("Terminal background", ThemeEdit_.TerminalBg);
    ColorRow("Selection highlight", ThemeEdit_.Selection);
    ColorRow("Cursor", ThemeEdit_.Cursor);
    ColorRow("Title bar background", ThemeEdit_.TitlebarBg);
    ColorRow("Title bar text", ThemeEdit_.TitlebarText);
    ImGui::EndChild();

    DrawKeybindingEditor();
    ImGui::EndChild();

    if (ImGui::Button("Save"))
    {
        std::string const Command = Util::Trim(CommandBuffer_);
        State_.AppSettings.ClaudeCommand = Command.empty() ? "claude" : Command;
        State_.AppSettings.Notifications = NotifyEdit_;
        State_.AppSettings.AutoStartWindow = AutoStartEdit_;
        State_.AppSettings.Keys = KeysEdit_;
        State_.AppSettings.PaneTitleScale = PaneTitleScaleEdit_;
        if (GitHubEdit_ != State_.AppSettings.GitHubEnabled)
        {
            State_.AppSettings.GitHubEnabled = GitHubEdit_;
            GitHubLastCheckAt_ = -1.0e9;
        }
        if (FontSizeEdit_ != State_.AppSettings.FontSize)
        {
            State_.AppSettings.FontSize = FontSizeEdit_;
            FontsDirty_ = true;
        }
        State_.AppSettings.Theme = ThemeEdit_;
        Style::Apply(UiScale_, State_.AppSettings.Theme);
        Platform::ApplyTitlebarColors(Window_, Style::ToRgbColor(State_.AppSettings.Theme.TitlebarBg),
                                      Style::ToRgbColor(State_.AppSettings.Theme.TitlebarText));
        for (const std::pair<const std::string, std::array<char, 256>>& NameEdit : SessionNameEdits_)
        {
            Session* const CurrentSession = FindSession(NameEdit.first);
            if (CurrentSession == nullptr)
            {
                continue;
            }
            std::string const Name = Util::Trim(NameEdit.second.data());
            if (Name.empty() == false)
            {
                CurrentSession->Rec.Name = Name;
            }
        }
        MarkDirty();
        CloseModal();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        CloseModal();
    }
}

void App::DrawUsageModal()
{
    const float Full = -1;
    bool InFlight = false, Done = false;
    UsageInfo CurrentInfo;
    ReadUsageFetch(InFlight, Done, CurrentInfo);
    if (InFlight)
    {
        ImGui::TextDisabled("Fetching usage...");
    }
    else if (Done == false || CurrentInfo.Ok == false)
    {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(WaitingColorV4_, "%s", CurrentInfo.Error.empty() ? "Could not fetch usage." : CurrentInfo.Error.c_str());
        ImGui::PopTextWrapPos();
    }
    else
    {
        if (CurrentInfo.HasSession)
        {
            ImGui::Text("Session usage: %.0f%%", CurrentInfo.SessionPct);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UiHelpers::WithAlpha(UsageFillColor(CurrentInfo.SessionPct), 200));
            ImGui::ProgressBar((float)(CurrentInfo.SessionPct / 100.0), ImVec2(Full, 0));
            ImGui::PopStyleColor();
            DrawResetProgress(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                              UiHelpers::UsageResetProgress(CurrentInfo.SessionPct, UiHelpers::SecondsUntil(CurrentInfo.SessionResetsAt), UiHelpers::SESSION_WINDOW_SECONDS));
            const std::string SessionResetText = UiHelpers::FormatTimeUntil(CurrentInfo.SessionResetsAt);
            if (SessionResetText.empty() == false)
            {
                ImGui::TextDisabled("Resets in %s", SessionResetText.c_str());
            }
            ImGui::Spacing();
        }
        if (CurrentInfo.HasWeekly)
        {
            ImGui::Text("Weekly usage: %.0f%%", CurrentInfo.WeeklyPct);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UiHelpers::WithAlpha(UsageFillColor(CurrentInfo.WeeklyPct), 200));
            ImGui::ProgressBar((float)(CurrentInfo.WeeklyPct / 100.0), ImVec2(Full, 0));
            ImGui::PopStyleColor();
            DrawResetProgress(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                              UiHelpers::UsageResetProgress(CurrentInfo.WeeklyPct, UiHelpers::SecondsUntil(CurrentInfo.WeeklyResetsAt), UiHelpers::WEEKLY_WINDOW_SECONDS));
            const std::string WeeklyResetText = UiHelpers::FormatTimeUntil(CurrentInfo.WeeklyResetsAt);
            if (WeeklyResetText.empty() == false)
            {
                ImGui::TextDisabled("Resets in %s", WeeklyResetText.c_str());
            }
        }
        if (CurrentInfo.HasSession == false && CurrentInfo.HasWeekly == false)
        {
            ImGui::TextDisabled("No usage data returned.");
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("Refresh"))
    {
        StartUsageFetch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
    {
        CloseModal();
    }
}

void App::DrawGitHubModal()
{
    if (DrawGitHubLogin() == true)
    {
        GitHubLogin_.Reset();
        CloseModal();
    }
}

void App::DrawReviewModal()
{
    DrawReview();
    if (ImGui::Button("Close"))
    {
        CloseModal();
    }
}

void App::DrawToasts()
{
    Toasts_.erase(std::remove_if(Toasts_.begin(), Toasts_.end(), [&](const Toast& CurrentToast) { return CurrentToast.Until < Now_; }), Toasts_.end());
    if (Toasts_.empty())
    {
        return;
    }
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    float ToastY = Viewport->WorkPos.y + Viewport->WorkSize.y - 16 * UiScale_;
    int Index = 0;
    for (auto It = Toasts_.rbegin(); It != Toasts_.rend(); ++It, ++Index)
    {
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::SetNextWindowPos(ImVec2(Viewport->WorkPos.x + Viewport->WorkSize.x - 16 * UiScale_, ToastY), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowSize(ImVec2(380 * UiScale_, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, It->IsError ? WaitingColorV4_ : AccentColorV4_);
        ImGui::Begin(("##toast" + std::to_string(Index)).c_str(), nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(It->Text.c_str());
        ImGui::PopTextWrapPos();
        ToastY -= ImGui::GetWindowHeight() + 8 * UiScale_;
        ImGui::End();
        ImGui::PopStyleColor();
    }
}
