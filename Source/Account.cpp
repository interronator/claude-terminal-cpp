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


void App::StartGitHubCheck()
{
    std::shared_ptr<GitHubFetch> Fetch = GitHubFetch_;
    if (Fetch == nullptr)
    {
        Fetch = std::make_shared<GitHubFetch>();
        GitHubFetch_ = Fetch;
    }
    {
        std::lock_guard<std::mutex> const Guard(Fetch->Mutex);
        if (Fetch->InFlight == true)
        {
            return;
        }
        Fetch->InFlight = true;
    }
    std::thread(
        [Fetch]
        {
            Git::GitHubStatus Status = Git::QueryGitHub();
            std::lock_guard<std::mutex> const Guard(Fetch->Mutex);
            Fetch->Status = std::move(Status);
            Fetch->InFlight = false;
            if (Session::Wake)
            {
                Session::Wake();
            }
        })
        .detach();
}

void App::CenteredText(const std::string& Text, const ImVec4& Color) const
{
    const float TextWidth = ImGui::CalcTextSize(Text.c_str()).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - TextWidth) * 0.5f));
    ImGui::TextColored(Color, "%s", Text.c_str());
}

bool App::DrawGitHubLogin()
{
    const float Scale = UiScale_;
    const ImVec4 DoneColorV4 = ImGui::ColorConvertU32ToFloat4(DoneColor_);
    const Git::GitHubStatus Status = ReadGitHubStatus();
    const GitHubLogin::Snapshot Login = GitHubLogin_.Read();
    if (Login.Value == GitHubLogin::Phase::Succeeded || Login.Value == GitHubLogin::Phase::SignedOut)
    {
        GitHubLastCheckAt_ = -1.0e9;
        GitHubLogin_.Reset();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, AccentColorV4_);
    ImGui::TextUnformatted("*");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Sign in to GitHub");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, PaneBgColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Border, PaneBorderColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * Scale);
    ImGui::BeginChild("##githubCard", ImVec2(-1, 150.0f * Scale), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Dummy(ImVec2(0, 14.0f * Scale));

    const bool LoginRunning = Login.Value == GitHubLogin::Phase::Starting || Login.Value == GitHubLogin::Phase::WaitingForBrowser;
    if (LoginRunning == true && Login.Value == GitHubLogin::Phase::WaitingForBrowser)
    {
        CenteredText("Enter this code on GitHub", TextDisabledColorV4_);
        ImGui::Dummy(ImVec2(0, 6.0f * Scale));
        ImGui::PushFont(MonoFont_);
        ImGui::SetWindowFontScale(2.0f);
        CenteredText(Login.Code, AccentColorV4_);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10.0f * Scale));
        CenteredText("Waiting for you to authorize in the browser...", TextDisabledColorV4_);
    }
    else if (LoginRunning == true)
    {
        ImGui::Dummy(ImVec2(0, 24.0f * Scale));
        CenteredText("Contacting GitHub...", TextDisabledColorV4_);
    }
    else if (Login.Value == GitHubLogin::Phase::Failed)
    {
        CenteredText("Sign-in did not complete", WaitingColorV4_);
        ImGui::Dummy(ImVec2(0, 6.0f * Scale));
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16.0f * Scale);
        ImGui::SetCursorPosX(16.0f * Scale);
        ImGui::TextDisabled("%s", Login.Message.c_str());
        ImGui::PopTextWrapPos();
    }
    else if (Login.Value == GitHubLogin::Phase::SigningOut)
    {
        ImGui::Dummy(ImVec2(0, 24.0f * Scale));
        CenteredText("Signing out...", TextDisabledColorV4_);
    }
    else if (Login.Value == GitHubLogin::Phase::SignOutFailed)
    {
        CenteredText("Could not sign out", WaitingColorV4_);
        ImGui::Dummy(ImVec2(0, 6.0f * Scale));
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16.0f * Scale);
        ImGui::SetCursorPosX(16.0f * Scale);
        ImGui::TextDisabled("%s", Login.Message.c_str());
        ImGui::PopTextWrapPos();
    }
    else if (Status.Value == Git::GitHubStatus::State::LoggedIn)
    {
        ImGui::Dummy(ImVec2(0, 14.0f * Scale));
        CenteredText("Signed in", DoneColorV4);
        if (Status.User.empty() == false)
        {
            CenteredText(Status.User, TextColorV4_);
        }
    }
    else if (Status.Value == Git::GitHubStatus::State::LoggedOut)
    {
        CenteredText("Not signed in", WaitingColorV4_);
        ImGui::Dummy(ImVec2(0, 6.0f * Scale));
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16.0f * Scale);
        ImGui::SetCursorPosX(16.0f * Scale);
        ImGui::TextDisabled("You will get a one-time code to enter on github.com. The GitHub CLI keeps the login; this app never sees your token.");
        ImGui::PopTextWrapPos();
    }
    else if (Status.Value == Git::GitHubStatus::State::NotInstalled)
    {
        CenteredText("GitHub CLI not found", WaitingColorV4_);
        ImGui::Dummy(ImVec2(0, 6.0f * Scale));
        CenteredText("Install gh from cli.github.com, then press Refresh.", TextDisabledColorV4_);
    }
    else
    {
        ImGui::Dummy(ImVec2(0, 24.0f * Scale));
        CenteredText("Checking...", TextDisabledColorV4_);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    bool CloseClicked = false;
    ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHoverColorV4_);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActiveColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.06f, 0.04f, 1));
    bool PrimaryDrawn = true;
    if (Login.Value == GitHubLogin::Phase::WaitingForBrowser)
    {
        if (ImGui::Button("Copy code") == true)
        {
            ImGui::SetClipboardText(Login.Code.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button("Open github.com") == true)
        {
            Platform::Reveal("https://github.com/login/device");
        }
    }
    else if (LoginRunning == true || Login.Value == GitHubLogin::Phase::SigningOut)
    {
        PrimaryDrawn = false;
    }
    else if (Login.Value == GitHubLogin::Phase::SignOutFailed)
    {
        if (ImGui::Button("Try again") == true)
        {
            GitHubLogin_.StartLogout(Status.User);
        }
    }
    else if (Login.Value == GitHubLogin::Phase::Failed || Status.Value == Git::GitHubStatus::State::LoggedOut)
    {
        if (ImGui::Button(Login.Value == GitHubLogin::Phase::Failed ? "Try again" : "Sign in with GitHub") == true)
        {
            GitHubLogin_.Start();
        }
    }
    else if (Status.Value == Git::GitHubStatus::State::NotInstalled)
    {
        if (ImGui::Button("Open download page") == true)
        {
            Platform::Reveal("https://cli.github.com");
        }
    }
    else if (Status.Value == Git::GitHubStatus::State::LoggedIn)
    {
        if (ImGui::Button("Sign out") == true)
        {
            GitHubLogin_.StartLogout(Status.User);
        }
    }
    else
    {
        PrimaryDrawn = false;
    }
    ImGui::PopStyleColor(4);

    if (PrimaryDrawn == true)
    {
        ImGui::SameLine();
    }
    if (LoginRunning == true)
    {
        if (ImGui::Button("Cancel") == true)
        {
            GitHubLogin_.Reset();
        }
    }
    else if (ImGui::Button("Refresh") == true)
    {
        GitHubLastCheckAt_ = -1.0e9;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close") == true)
    {
        CloseClicked = true;
    }
    return CloseClicked;
}

Git::GitHubStatus App::ReadGitHubStatus() const
{
    if (GitHubFetch_ == nullptr)
    {
        return Git::GitHubStatus{};
    }
    std::lock_guard<std::mutex> const Guard(GitHubFetch_->Mutex);
    return GitHubFetch_->Status;
}

std::string App::GitHubLabel() const
{
    const Git::GitHubStatus Status = ReadGitHubStatus();
    switch (Status.Value)
    {
        case Git::GitHubStatus::State::LoggedIn: return Status.User.empty() == true ? "GitHub: logged in" : "GitHub: " + Status.User;
        case Git::GitHubStatus::State::LoggedOut: return "GitHub: logged out";
        case Git::GitHubStatus::State::NotInstalled: return "GitHub: gh missing";
        default: return "GitHub: checking...";
    }
}

void App::StartUsageFetch()
{
    std::shared_ptr<UsageFetch> const Fetch = std::make_shared<UsageFetch>();
    Fetch->InFlight = true;
    if (UsageFetch_ != nullptr)
    {
        std::lock_guard<std::mutex> const PreviousGuard(UsageFetch_->Mutex);
        Fetch->Result = UsageFetch_->Result;
    }
    UsageFetch_ = Fetch;
    std::thread(
        [Fetch]
        {
            UsageInfo Result = UsageClient::Fetch();
            std::lock_guard<std::mutex> const Guard(Fetch->Mutex);
            Fetch->Result = std::move(Result);
            Fetch->InFlight = false;
            Fetch->Done = true;
            if (Session::Wake)
            {
                Session::Wake();
            }
        })
        .detach();
}

void App::StartWindowPing()
{
    std::shared_ptr<std::atomic<bool>> const Busy = WindowPingBusy_;
    Busy->store(true);
    WindowPingLastAt_ = Now_;
    std::vector<std::string> Argv = Util::SplitCommand(State_.AppSettings.ClaudeCommand.empty() ? "claude" : State_.AppSettings.ClaudeCommand);
    if (Argv.empty() == true)
    {
        Argv.push_back("claude");
    }
    Argv.insert(Argv.end(), {"-p", "Reply with just: ok", "--model", "haiku"});
    std::thread(
        [Busy, Argv]
        {
            Git::RunProcess(Argv, "");
            Busy->store(false);
        })
        .detach();
}

void App::ReadUsageFetch(bool& InFlight, bool& Done, UsageInfo& Info) const
{
    InFlight = false;
    Done = false;
    if (UsageFetch_ == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> const Guard(UsageFetch_->Mutex);
    InFlight = UsageFetch_->InFlight;
    Done = UsageFetch_->Done;
    Info = UsageFetch_->Result;
}

ImU32 App::UsageFillColor(const double Pct) const
{
    return UiHelpers::UsageColor(AccentColor_, UsageWarningColor_, UsageFullColor_, Pct);
}

void App::DrawResetProgress(ImDrawList* const DrawList, const ImVec2 Min, const ImVec2 Max, const float Progress) const
{
    if (Progress <= 0.0f)
    {
        return;
    }
    DrawList->AddRectFilled(Min, ImVec2(Min.x + (Max.x - Min.x) * Progress, Max.y), UiHelpers::WithAlpha(AccentColor_, 230), 4.0f);
}

bool App::DrawUsageMeter(ImDrawList* DrawList, float Width, const char* Label, bool HasValue, double Pct, const ImU32 FillColor, const float ResetProgress, const std::string& Suffix,
                         const std::string& ErrorText) const
{
    const float Height = ImGui::GetFrameHeight();
    const std::string LabelText = std::string(Label) + ":";
    const ImVec2 LabelSize = ImGui::CalcTextSize(LabelText.c_str());
    const ImVec2 LabelPos = ImGui::GetCursorScreenPos();
    const ImVec2 Pos(LabelPos.x + LabelSize.x + UiScale_ * UiHelpers::USAGE_LABEL_GAP, LabelPos.y);
    DrawList->AddText(ImVec2(LabelPos.x, LabelPos.y + (Height - LabelSize.y) * 0.5f), TextColorU32_, LabelText.c_str());
    ImGui::SetCursorScreenPos(Pos);
    ImGui::PushID(Label);
    const bool Clicked = ImGui::InvisibleButton("##usageMeter", ImVec2(Width, Height));
    ImGui::PopID();

    DrawList->AddRectFilled(Pos, ImVec2(Pos.x + Width, Pos.y + Height), PaneBgColorU32_, 4.0f);
    if (HasValue == true)
    {
        const float FillWidth = Width * (float)std::clamp(Pct, 0.0, 100.0) / 100.0f;
        DrawList->AddRectFilled(Pos, ImVec2(Pos.x + FillWidth, Pos.y + Height), UiHelpers::WithAlpha(FillColor, 200), 4.0f);
        DrawResetProgress(DrawList, Pos, ImVec2(Pos.x + Width, Pos.y + Height), ResetProgress);
    }
    DrawList->AddRect(Pos, ImVec2(Pos.x + Width, Pos.y + Height), PaneBorderColorU32_, 4.0f);

    const std::string Text =
        HasValue == true ? std::to_string(static_cast<int>(Pct)) + "%" + Suffix : std::string("--");
    const ImVec2 TextSize = ImGui::CalcTextSize(Text.c_str());
    DrawList->AddText(ImVec2(Pos.x + (Width - TextSize.x) * 0.5f, Pos.y + (Height - TextSize.y) * 0.5f), TextColorU32_, Text.c_str());

    if (ImGui::IsItemHovered() == true)
    {
        if (HasValue == false && ErrorText.empty() == false)
        {
            ImGui::SetTooltip("%s usage\n%s", Label, ErrorText.c_str());
        }
        else
        {
            ImGui::SetTooltip("%s usage", Label);
        }
    }
    return Clicked;
}
