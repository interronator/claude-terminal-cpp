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


void App::DrawPane(Session& CurrentSession, ImVec2 Pos, ImVec2 Size, int Index)
{
    const float Scale = UiScale_;
    bool const IsActive = State_.Ui.ActiveId == CurrentSession.Rec.Id;
    ImGui::SetCursorPos(Pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PaneBgColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Border, CurrentSession.CurrentStatus == Status::Waiting ? WaitingColorV4_
                                           : IsActive                              ? AccentColorV4_
                                                                                    : PaneBorderColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6 * Scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 0));
    ImGui::BeginChild(("##pane" + CurrentSession.Rec.Id).c_str(), Size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    PeerVeins_.AddPane(CurrentSession.Rec.Id, ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + Size.x, ImGui::GetWindowPos().y + Size.y));

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2)))
    {
        FocusSession(CurrentSession.Rec.Id);
    }

    // ---- header ----
    const float TitleScale = static_cast<float>(State_.AppSettings.PaneTitleScale) / 100.0f;
    float const HeaderHeight = std::max(ImGui::GetFrameHeight() + 4 * Scale, ImGui::GetTextLineHeight() * TitleScale + 8 * Scale);
    ImVec2 const HeaderPos = ImGui::GetCursorScreenPos();
    ImDrawList* DrawList = ImGui::GetWindowDrawList();

    DrawList->AddRectFilled(HeaderPos, ImVec2(HeaderPos.x + Size.x, HeaderPos.y + HeaderHeight), SurfaceColorU32_);
    DrawList->AddLine(ImVec2(HeaderPos.x, HeaderPos.y + HeaderHeight), ImVec2(HeaderPos.x + Size.x, HeaderPos.y + HeaderHeight),
                      PaneBorderColorU32_);
    DrawList->AddRectFilled(HeaderPos, ImVec2(HeaderPos.x + 4 * Scale, HeaderPos.y + HeaderHeight), UiHelpers::SessionTint(CurrentSession.Rec.Id));
    DrawStatusDot(DrawList, ImVec2(HeaderPos.x + 14 * Scale, HeaderPos.y + HeaderHeight * 0.5f), 4.5f * Scale, CurrentSession);
    Project* const CurrentProject = FindProject(CurrentSession.Rec.ProjectId);
    const std::string ProjectName = CurrentProject != nullptr ? CurrentProject->Name : std::string();
    const std::string BranchText = CurrentSession.Rec.WorktreeInfo ? "[" + CurrentSession.Rec.WorktreeInfo->Branch + "]" : std::string();

    ImGui::SetWindowFontScale(TitleScale);
    ImGui::SetCursorPos(ImVec2(28 * Scale, (HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted(CurrentSession.Rec.Name.c_str());
    ImGui::SameLine(0, 10 * Scale);
    ImGui::SetWindowFontScale(TitleScale);
    ImGui::SetCursorPosY((HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextColored(AccentColorV4_, "%s", ProjectName.c_str());
    ImGui::SetWindowFontScale(1.0f);
    if (BranchText.empty() == false)
    {
        ImGui::SameLine(0, 8 * Scale);
        ImGui::SetCursorPosY((HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::TextDisabled("%s", BranchText.c_str());
    }
    const float TitleEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;

    const std::map<std::string, TranscriptScanner>::const_iterator Scanner = TranscriptScanners_.find(CurrentSession.Rec.Id);
    const int ContextPercent = Scanner != TranscriptScanners_.end() ? Scanner->second.ContextPercent() : -1;

    const char* const MaximizeLabel = MaximizedId_ == CurrentSession.Rec.Id ? "Restore" : "Max";
    const float ButtonPad = 5 * Scale * 2;
    const float ItemGap = 6 * Scale;
    const bool HasMaximize = State_.Ui.Mode == "tile";
    const float ButtonsWidth = ImGui::CalcTextSize("Build & Run").x + ImGui::CalcTextSize("Open").x + ImGui::CalcTextSize("Restart").x + ImGui::CalcTextSize("x").x +
                               ButtonPad * (HasMaximize ? 5 : 4) + ItemGap + (HasMaximize ? ImGui::CalcTextSize(MaximizeLabel).x : 0.0f);
    std::string const Shortcut = "Alt+" + std::to_string(Index + 1);
    const float ShortcutWidth = ImGui::CalcTextSize(Shortcut.c_str()).x;
    const float ShortcutX = Size.x - ButtonsWidth - ShortcutWidth - ItemGap * (HasMaximize ? 4 : 3) - 4 * Scale;
    if (ContextPercent >= 0)
    {
        const std::string ContextText = "ctx " + std::to_string(ContextPercent) + "%";
        const float ContextX = TitleEndX + 24 * Scale;
        if (ContextX + ImGui::CalcTextSize(ContextText.c_str()).x <= ShortcutX - ItemGap)
        {
            ImGui::SetCursorPos(ImVec2(ContextX, (HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f));
            ImGui::TextColored(ContextColor(ContextPercent), "%s", ContextText.c_str());
            if (ImGui::IsItemHovered() == true)
            {
                ImGui::SetTooltip("Share of Claude's context window in use");
            }
        }
    }
    ImGui::SetCursorPos(ImVec2(ShortcutX, (HeaderHeight - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::TextDisabled("%s", Shortcut.c_str());
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5 * Scale, 1 * Scale));
    ImGui::PushID("hdr");
    if (ImGui::SmallButton("Build & Run") == true)
    {
        BuildAndRun(CurrentSession);
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Run build.bat then run.bat (creates them if missing)");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open") == true)
    {
        Platform::Reveal(CurrentProject != nullptr ? CurrentProject->Path : CurrentSession.Rec.Cwd);
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Open project directory");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart"))
    {
        std::string const Id = CurrentSession.Rec.Id;
        ConfirmTitle_ = "Restart session?";
        ConfirmText_ = CurrentSession.CurrentStatus == Status::Busy || CurrentSession.CurrentStatus == Status::Waiting
                           ? "Claude is active right now. Restarting stops it and resumes the conversation from its last saved state."
                           : "Restarting stops Claude and resumes the conversation from its last saved state.";
        ConfirmYesAction_ = [this, Id]
        {
            if (Session* Target = FindSession(Id))
            {
                Target->Kill(Now_, StatusCallback());
                Target->Attempted = false;
            }
        };
        OpenModal(Modal::Confirm);
    }
    ImGui::SameLine();
    if (State_.Ui.Mode == "tile" && ImGui::SmallButton(MaximizeLabel))
    {
        ToggleMaximize(CurrentSession.Rec.Id);
    }
    if (State_.Ui.Mode == "tile")
    {
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("x"))
    {
        HideSession(CurrentSession.Rec.Id);
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Hide pane");
    }
    ImGui::PopID();
    ImGui::PopStyleVar();

    const bool GitHubReady = State_.AppSettings.GitHubEnabled == true && ReadGitHubStatus().Value == Git::GitHubStatus::State::LoggedIn;
    const bool GitActionsEnabled = GitHubReady == true && CurrentSession.IsRunning() == true;
    const float GitPad = 5 * Scale * 2;
    const bool HasLiveButton = IsUnrealSession(CurrentSession);
    const float LiveButtonWidth = HasLiveButton == true ? ImGui::CalcTextSize(LIVE_BUTTON_LABEL).x + GitPad + ItemGap : 0.0f;
    const Handoff* const PendingHandoff = PollHandoff(CurrentSession);
    const char* const HandoffLabel = PendingHandoff == nullptr ? "Handoff###handoff" : PendingHandoff->Ready == true ? "Continue fresh###handoff" : "Handoff...###handoff";
    const float HandoffWidth = ImGui::CalcTextSize(HandoffLabel, nullptr, true).x + GitPad + ItemGap;
    const float GitButtonsWidth = ImGui::CalcTextSize("Log").x + ImGui::CalcTextSize("Review").x + ImGui::CalcTextSize("Commit").x + ImGui::CalcTextSize("Push").x +
                                  GitPad * 4 + ItemGap * 3 + LiveButtonWidth + HandoffWidth;
    const float RightStartX = Size.x - ButtonsWidth - ShortcutWidth - ItemGap * (HasMaximize ? 4 : 3) - 4 * Scale;
    const float GitStartX = std::min((Size.x - GitButtonsWidth) * 0.5f, RightStartX - ItemGap * 2 - GitButtonsWidth);
    if (GitStartX >= TitleEndX + ItemGap * 2)
    {
        const float GitButtonHeight = ImGui::GetTextLineHeight() + 2 * Scale;
        ImGui::SetCursorPos(ImVec2(GitStartX, (HeaderHeight - GitButtonHeight) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5 * Scale, 1 * Scale));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 45)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 95)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 150)));
        ImGui::PushID("gitActions");
        if (ImGui::Button("Log") == true)
        {
            ProjectLogWindow_.Open(CurrentSession.Rec.ProjectId);
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("What has been done in this project, by hour, day and week, plus notes");
        }
        ImGui::SameLine(0, ItemGap);
        const bool HandoffReady = PendingHandoff != nullptr && PendingHandoff->Ready == true;
        if (HandoffReady == true)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
        }
        ImGui::BeginDisabled(PendingHandoff == nullptr && CurrentSession.IsRunning() == false);
        const bool HandoffClicked = ImGui::Button(HandoffLabel);
        ImGui::EndDisabled();
        if (HandoffReady == true)
        {
            ImGui::PopStyleColor();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
        {
            const std::string Tip = PendingHandoff == nullptr ? std::string("Ask this session to write a handoff summary, so the work can continue in a fresh session with a clean context")
                                    : HandoffReady == true    ? "Start a fresh session that reads the summary and carries on:\n" + PendingHandoff->Path + "\nRight-click for more"
                                                              : "Waiting for the session to write the summary:\n" + PendingHandoff->Path + "\nRight-click for more";
            ImGui::SetTooltip("%s", Tip.c_str());
        }
        if (PendingHandoff != nullptr && ImGui::BeginPopupContextItem("##handoffMenu") == true)
        {
            if (ImGui::MenuItem("Open summary file", nullptr, false, std::filesystem::exists(PendingHandoff->Path) == true) == true)
            {
                Platform::Reveal(PendingHandoff->Path);
            }
            if (ImGui::MenuItem("Continue fresh now") == true)
            {
                ContinueFromHandoff(CurrentSession);
            }
            if (ImGui::MenuItem("Ask for a new summary", nullptr, false, CurrentSession.IsRunning() == true) == true)
            {
                RequestHandoff(CurrentSession);
            }
            if (ImGui::MenuItem("Discard") == true)
            {
                Handoffs_.erase(CurrentSession.Rec.Id);
            }
            ImGui::EndPopup();
        }
        if (HandoffClicked == true)
        {
            if (PendingHandoff == nullptr)
            {
                RequestHandoff(CurrentSession);
            }
            else if (HandoffReady == true)
            {
                ContinueFromHandoff(CurrentSession);
            }
        }
        ImGui::SameLine(0, ItemGap);
        if (HasLiveButton == true)
        {
            const bool LiveOn = LiveViewIds_.count(CurrentSession.Rec.Id) > 0;
            if (LiveOn == true)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
            }
            if (ImGui::Button(LIVE_BUTTON_LABEL) == true)
            {
                if (LiveOn == true)
                {
                    LiveViewIds_.erase(CurrentSession.Rec.Id);
                }
                else
                {
                    LiveViewIds_.insert(CurrentSession.Rec.Id);
                }
            }
            if (LiveOn == true)
            {
                ImGui::PopStyleColor();
            }
            const bool HasPreview = HasFreshLiveFrame(PollLiveFrame(CurrentSession));
            const ImVec2 ButtonMin = ImGui::GetItemRectMin();
            const ImVec2 ButtonMax = ImGui::GetItemRectMax();
            const ImVec2 DotCenter(ButtonMin.x + 5 * Scale + 3.5f * Scale, (ButtonMin.y + ButtonMax.y) * 0.5f);
            ImGui::GetWindowDrawList()->AddCircleFilled(DotCenter, 3.5f * Scale, HasPreview == true ? IM_COL32(230, 50, 50, 255) : IM_COL32(130, 134, 145, 255));
            if (ImGui::IsItemHovered() == true)
            {
                ImGui::SetTooltip("%s", HasPreview == true ? "Live picture available: show what this session's Unreal editor or game is drawing"
                                                           : "Nothing to preview right now (no live capture running for this session)");
            }
            ImGui::SameLine(0, ItemGap);
        }
        ImGui::BeginDisabled(GitActionsEnabled == false);
        const bool CommitClicked = ImGui::Button("Commit");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
        {
            ImGui::SetTooltip("%s", GitActionsEnabled == true ? "Ask Claude to commit the current changes"
                                    : GitHubReady == true      ? "Session is not running"
                                                               : "Enable GitHub access and log in to use this");
        }
        ImGui::SameLine(0, ItemGap);
        const bool ReviewClicked = ImGui::Button("Review");
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Review commits before pushing");
        }
        ImGui::SameLine(0, ItemGap);
        ImGui::BeginDisabled(GitActionsEnabled == false);
        const bool PushClicked = ImGui::Button("Push");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
        {
            ImGui::SetTooltip("%s", GitActionsEnabled == true ? "Ask Claude to push the current branch to GitHub"
                                    : GitHubReady == true      ? "Session is not running"
                                                               : "Enable GitHub access and log in to use this");
        }
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (ReviewClicked == true)
        {
            Review_.Load(CurrentSession.Rec.Cwd);
            ModalTarget_ = CurrentSession.Rec.Id;
            OpenModal(Modal::Review);
        }
        if (CommitClicked == true)
        {
            SendPrompt(CurrentSession, COMMIT_PROMPT);
        }
        if (PushClicked == true)
        {
            SendPrompt(CurrentSession, PUSH_PROMPT);
        }
    }

    // Drag handle spans the whole header but is submitted last, after the Restart/Max/Hide buttons
    // above: on a mouse-press frame, whichever widget is submitted first at a given position claims
    // the click, so those buttons (submitted earlier) keep working over the overlap; this invisible
    // button only ever "wins" the press in the header's empty space.
    ImGui::SetCursorScreenPos(HeaderPos);
    ImGui::InvisibleButton("##paneDragHandle", ImVec2(Size.x, HeaderHeight));
    if (ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload("CT_PANE_SESSION_ID", CurrentSession.Rec.Id.c_str(), CurrentSession.Rec.Id.size() + 1);
        const ImVec2 CardSize(150 * Scale, 56 * Scale);
        ImGui::Dummy(CardSize);
        ImVec2 CardMin = ImGui::GetItemRectMin(), CardMax = ImGui::GetItemRectMax();
        ImDrawList* DragDrawList = ImGui::GetWindowDrawList();
        DragDrawList->AddRectFilled(CardMin, CardMax, SurfaceColorU32_, 6 * Scale);
        DragDrawList->AddRect(CardMin, CardMax, AccentColor_, 6 * Scale, 0, 2.0f);
        DrawStatusDot(DragDrawList, ImVec2(CardMin.x + 14 * Scale, CardMin.y + CardSize.y * 0.5f), 4.5f * Scale, CurrentSession);
        DragDrawList->AddText(ImVec2(CardMin.x + 24 * Scale, CardMin.y + (CardSize.y - ImGui::GetFontSize()) * 0.5f), TextColorU32_,
                              CurrentSession.Rec.Name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget())
    {
        const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload("CT_PANE_SESSION_ID", ImGuiDragDropFlags_AcceptBeforeDelivery);
        if (Payload != nullptr)
        {
            DrawList->AddRect(ImVec2(HeaderPos.x + 1, HeaderPos.y + 1), ImVec2(HeaderPos.x + Size.x - 1, HeaderPos.y + Size.y - 1),
                              AccentColor_, 6 * Scale, 0, 3.0f);
            if (Payload->Delivery)
            {
                std::string const DraggedId(static_cast<const char*>(Payload->Data), (size_t)Payload->DataSize - 1);
                SwapSessionOrder(DraggedId, CurrentSession.Rec.Id);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SetCursorPos(ImVec2(0, HeaderHeight + 1));
    DrawTerminal(CurrentSession, ImVec2(Size.x, ImGui::GetContentRegionAvail().y), IsActive);
    if (LiveViewIds_.count(CurrentSession.Rec.Id) > 0)
    {
        DrawLiveView(CurrentSession, HeaderPos, Size, HeaderHeight);
    }
    DrawShellActivity(CurrentSession, HeaderPos, Size, HeaderHeight);

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void App::DrawLiveView(Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight)
{
    constexpr float DEFAULT_ASPECT = 0.5625f;
    const float Scale = UiScale_;
    const LiveView::Frame& Latest = PollLiveFrame(CurrentSession);

    const float Margin = 8 * Scale;
    const float Width = std::min(PaneSize.x * 0.45f, 640 * Scale);
    const ImageCache::Texture* const Image = Latest.Path.empty() == true ? nullptr : Images_->Get(Latest.Path, static_cast<std::time_t>(Latest.Key));
    const bool HasImage = Image != nullptr && Image->Failed == false && Image->Width > 0;
    const float Aspect = HasImage == true ? static_cast<float>(Image->Height) / static_cast<float>(Image->Width) : DEFAULT_ASPECT;
    const float LabelHeight = ImGui::GetTextLineHeight() + 4 * Scale;
    const float Height = std::min(Width * Aspect, (PaneSize.y - HeaderHeight) * 0.6f);
    const ImVec2 Min(PaneTopLeft.x + PaneSize.x - Width - Margin, PaneTopLeft.y + HeaderHeight + Margin);
    const ImVec2 Max(Min.x + Width, Min.y + Height + LabelHeight);

    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    DrawList->AddRectFilled(Min, Max, IM_COL32(0, 0, 0, 235), 6 * Scale);
    const ImVec2 ImageMax(Max.x, Min.y + Height);
    if (HasImage == true)
    {
        const float ImageWidth = std::min(Width, Height / Aspect);
        const float ImageLeft = Min.x + (Width - ImageWidth) * 0.5f;
        DrawList->AddImageRounded(Image->Id, ImVec2(ImageLeft, Min.y), ImVec2(ImageLeft + ImageWidth, ImageMax.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                                  6 * Scale, ImDrawFlags_RoundCornersTop);
    }
    else
    {
        const char* const Waiting = Latest.Path.empty() == true ? "No live frame yet. Ask the session to start the live capture\nin the Unreal editor or game it runs."
                                                                  : "Loading frame...";
        const ImVec2 TextSize = ImGui::CalcTextSize(Waiting);
        DrawList->AddText(ImVec2(Min.x + (Width - TextSize.x) * 0.5f, Min.y + (Height - TextSize.y) * 0.5f), TextDisabledColorU32_, Waiting);
    }

    const bool Stale = Latest.Path.empty() == false && Latest.AgeSeconds > LIVE_STALE_SECONDS;
    std::string Label = "Live";
    if (Latest.Path.empty() == false)
    {
        Label += Stale == true ? "  -  stopped " + std::to_string(static_cast<long long>(Latest.AgeSeconds)) + "s ago"
                               : "  -  " + std::to_string(static_cast<long long>(Latest.AgeSeconds + 0.5)) + "s ago";
    }
    const ImU32 DotColor = Latest.Path.empty() == true ? IdleColor_ : Stale == true ? WaitingColor_ : DoneColor_;
    const float LabelY = ImageMax.y + (LabelHeight - ImGui::GetTextLineHeight()) * 0.5f;
    DrawList->AddCircleFilled(ImVec2(Min.x + 10 * Scale, LabelY + ImGui::GetTextLineHeight() * 0.5f), 3.5f * Scale, DotColor);
    DrawList->AddText(ImVec2(Min.x + 20 * Scale, LabelY), TextColorU32_, Label.c_str());
    DrawList->AddRect(Min, Max, Stale == true ? WaitingColor_ : AccentColor_, 6 * Scale);
}

void App::DrawShellActivity(const Session& CurrentSession, const ImVec2 PaneTopLeft, const ImVec2 PaneSize, const float HeaderHeight)
{
    using ActiveCommand = TranscriptScanner::ActiveCommand;
    const std::map<std::string, std::vector<ActiveCommand>>::const_iterator Found = RunningCommands_.find(CurrentSession.Rec.Id);
    if (Found == RunningCommands_.end() || Found->second.empty() == true || CurrentSession.IsRunning() == false)
    {
        return;
    }
    constexpr size_t MAX_ROWS = 3;
    constexpr double SLOWER_THAN_USUAL = 1.15;
    const float Scale = UiScale_;
    const float Margin = 8 * Scale;
    const float Pad = 7 * Scale;
    const bool LiveShown = LiveViewIds_.count(CurrentSession.Rec.Id) > 0;
    const float Width = std::min(PaneSize.x * (LiveShown == true ? 0.5f : 0.6f) - Margin * 2, 460 * Scale);
    if (Width < 160 * Scale)
    {
        return;
    }

    struct Row
    {
        const ActiveCommand* Command = nullptr;
        std::string Kind;
        std::string Title;
        std::string Elapsed;
        std::string Hint;
        std::string Output;
        float Fraction = 0;
        bool Indeterminate = false;
        ImU32 Fill = 0;
    };
    constexpr std::time_t MIN_SHOW_SECONDS = 3;
    const std::time_t Now = std::time(nullptr);
    std::vector<ActiveCommand> Commands;
    for (const ActiveCommand& Command : Found->second)
    {
        if (Now - Command.StartedAt >= MIN_SHOW_SECONDS)
        {
            Commands.push_back(Command);
        }
    }
    if (Commands.empty() == true)
    {
        return;
    }
    std::vector<Row> Rows;
    for (size_t Index = 0; Index < Commands.size() && Index < MAX_ROWS; Index++)
    {
        const ActiveCommand& Command = Commands[Index];
        Row Current;
        Current.Command = &Command;
        const double Elapsed = static_cast<double>(std::max<std::time_t>(0, Now - Command.StartedAt));
        Current.Kind = Command.Type == ActiveCommand::Kind::Shell ? "shell" : Command.Type == ActiveCommand::Kind::Background ? "background" : "monitor";
        Current.Title = Command.Description.empty() == false ? Command.Description : Command.Command;
        Current.Elapsed = FormatDuration(Elapsed);
        const double Typical = Command.Type == ActiveCommand::Kind::Monitor ? -1 : CommandTiming_.Typical(CommandTiming::KeyFor(Command.Command));
        const double Limit = Command.Deadline != 0 ? static_cast<double>(Command.Deadline - Command.StartedAt) : 0;
        const std::string LimitText = Command.Type == ActiveCommand::Kind::Shell && Limit > 0 ? "  -  stops at the " + FormatRoughDuration(Limit) + " limit" : "";
        if (Command.Type == ActiveCommand::Kind::Monitor && Limit > 0)
        {
            Current.Fraction = static_cast<float>(Elapsed / Limit);
            Current.Fill = AccentColor_;
            Current.Hint = "watching  -  ends on its own in " + FormatRoughDuration(std::max(0.0, Limit - Elapsed));
        }
        else if (Typical > 0 && Elapsed > Typical * SLOWER_THAN_USUAL)
        {
            Current.Fraction = 1.0f;
            Current.Fill = UsageWarningColor_;
            Current.Hint = "taking longer than usual (~" + FormatRoughDuration(Typical) + ")" + LimitText;
        }
        else if (Typical > 0)
        {
            Current.Fraction = static_cast<float>(Elapsed / Typical);
            Current.Fill = AccentColor_;
            Current.Hint = "usually ~" + FormatRoughDuration(Typical) + ", about " + FormatRoughDuration(std::max(0.0, Typical - Elapsed)) + " left" + LimitText;
        }
        else if (Command.Type == ActiveCommand::Kind::Shell && Limit > 0)
        {
            Current.Fraction = static_cast<float>(Elapsed / Limit);
            Current.Fill = UiHelpers::WithAlpha(TextDisabledColorU32_, 170);
            Current.Hint = "no estimate yet" + LimitText;
        }
        else
        {
            Current.Indeterminate = true;
            Current.Fill = UiHelpers::WithAlpha(AccentColor_, 200);
            Current.Hint = Command.Type == ActiveCommand::Kind::Monitor ? "watching" : "no estimate yet  -  runs until it finishes";
        }
        if (Command.OutputFile.empty() == false)
        {
            Current.Output = OutputTail(Command.OutputFile);
        }
        Current.Fraction = std::clamp(Current.Fraction, 0.0f, 1.0f);
        Rows.push_back(Current);
    }

    const float LineHeight = ImGui::GetTextLineHeight();
    const float BarHeight = 6 * Scale;
    const float RowGap = 8 * Scale;
    float Height = Pad * 2;
    for (const Row& Current : Rows)
    {
        Height += LineHeight * (Current.Output.empty() == true ? 2.0f : 3.0f) + BarHeight + 6 * Scale;
    }
    Height += RowGap * static_cast<float>(Rows.size() - 1);
    const size_t Hidden = Commands.size() - Rows.size();
    if (Hidden > 0)
    {
        Height += LineHeight + 4 * Scale;
    }

    const ImVec2 Min(PaneTopLeft.x + Margin, PaneTopLeft.y + HeaderHeight + Margin);
    const ImVec2 Max(Min.x + Width, Min.y + Height);
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    DrawList->AddRectFilled(ImVec2(Min.x + 2 * Scale, Min.y + 3 * Scale), ImVec2(Max.x + 2 * Scale, Max.y + 3 * Scale), IM_COL32(0, 0, 0, 120), 6 * Scale);
    DrawList->AddRectFilled(Min, Max, IM_COL32(12, 14, 18, 255), 6 * Scale);
    DrawList->AddRect(Min, Max, UiHelpers::WithAlpha(AccentColor_, 200), 6 * Scale, 0, 1.5f * Scale);

    const float Inner = Width - Pad * 2;
    const float Left = Min.x + Pad;
    float Y = Min.y + Pad;
    for (const Row& Current : Rows)
    {
        const float KindWidth = ImGui::CalcTextSize(Current.Kind.c_str()).x;
        const float ElapsedWidth = ImGui::CalcTextSize(Current.Elapsed.c_str()).x;
        DrawList->AddRectFilled(ImVec2(Left - 3 * Scale, Y - 1 * Scale), ImVec2(Left + KindWidth + 3 * Scale, Y + LineHeight + 1 * Scale), UiHelpers::WithAlpha(AccentColor_, 70), 3 * Scale);
        DrawList->AddText(ImVec2(Left, Y), TextColorU32_, Current.Kind.c_str());
        const std::string Title = TruncateToWidth(Current.Title, Inner - KindWidth - ElapsedWidth - 20 * Scale);
        DrawList->AddText(ImVec2(Left + KindWidth + 12 * Scale, Y), TextColorU32_, Title.c_str());
        DrawList->AddText(ImVec2(Left + Inner - ElapsedWidth, Y), TextColorU32_, Current.Elapsed.c_str());
        Y += LineHeight + 3 * Scale;

        const ImVec2 TrackMin(Left, Y);
        const ImVec2 TrackMax(Left + Inner, Y + BarHeight);
        DrawList->AddRectFilled(TrackMin, TrackMax, UiHelpers::WithAlpha(TextDisabledColorU32_, 45), BarHeight * 0.5f);
        if (Current.Indeterminate == true)
        {
            // Back-and-forth sweep: there's nothing to measure progress against.
            const float Segment = Inner * 0.25f;
            const float Phase = static_cast<float>(std::fmod(Now_ * 0.6, 2.0));
            const float Travel = Phase < 1.0f ? Phase : 2.0f - Phase;
            const float SegmentLeft = Left + (Inner - Segment) * Travel;
            DrawList->AddRectFilled(ImVec2(SegmentLeft, Y), ImVec2(SegmentLeft + Segment, Y + BarHeight), Current.Fill, BarHeight * 0.5f);
        }
        else if (Current.Fraction > 0)
        {
            DrawList->AddRectFilled(TrackMin, ImVec2(Left + Inner * Current.Fraction, Y + BarHeight), Current.Fill, BarHeight * 0.5f);
        }
        Y += BarHeight + 3 * Scale;

        const std::string Hint = TruncateToWidth(Current.Hint, Inner);
        DrawList->AddText(ImVec2(Left, Y), Current.Fill == UsageWarningColor_ ? UsageWarningColor_ : UiHelpers::WithAlpha(TextColorU32_, 190), Hint.c_str());
        Y += LineHeight;
        if (Current.Output.empty() == false)
        {
            const std::string Output = TruncateToWidth("> " + Current.Output, Inner);
            DrawList->AddText(ImVec2(Left, Y), UiHelpers::WithAlpha(TextColorU32_, 170), Output.c_str());
            Y += LineHeight;
        }
        Y += RowGap;
    }
    if (Hidden > 0)
    {
        const std::string More = "+" + std::to_string(Hidden) + " more running";
        DrawList->AddText(ImVec2(Left, Y - RowGap + 4 * Scale), TextDisabledColorU32_, More.c_str());
    }

    const ImVec2 Mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsWindowHovered() == true && Mouse.x >= Min.x && Mouse.x <= Max.x && Mouse.y >= Min.y && Mouse.y <= Max.y)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        for (size_t Index = 0; Index < Commands.size(); Index++)
        {
            if (Index > 0)
            {
                ImGui::Separator();
            }
            const ActiveCommand& Command = Commands[Index];
            if (Command.Description.empty() == false)
            {
                ImGui::TextUnformatted(Command.Description.c_str());
            }
            ImGui::TextDisabled("%s", Command.Command.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}
