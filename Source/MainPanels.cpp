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


void App::DrawMain(const float Width, const float Height)
{
    const float Scale = UiScale_;
    ImGui::BeginChild("##main", ImVec2(Width, Height), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    PeerVeins_.BeginFrame();
    LayoutDebug_.Sample("main.width", ImGui::GetWindowWidth());
    LayoutDebug_.Sample("main.x", ImGui::GetWindowPos().x);
    DrawToolbar();

    std::vector<std::string> Ids = VisibleIds();
    ImVec2 const Avail = ImGui::GetContentRegionAvail();
    if (Ids.empty())
    {
        const char* const Message = State_.Projects.empty() ? "Add a project folder, then start a Claude session in it."
                                                       : "Pick a session in the sidebar, or press + next to a project to start one.";
        ImVec2 const TextSize = ImGui::CalcTextSize(Message);
        ImGui::SetCursorPos(ImVec2((Avail.x - TextSize.x) * 0.5f, ImGui::GetCursorPosY() + Avail.y * 0.4f));
        ImGui::TextDisabled("%s", Message);
        ImGui::EndChild();
        return;
    }

    const float Gap = 6 * Scale;
    int const Count = (int)Ids.size();
    int const Columns = Count <= 2 ? Count : (int)std::ceil(std::sqrt((double)Count));
    int const Rows = (Count + Columns - 1) / Columns;
    const ImVec2 PanesMin = ImGui::GetCursorScreenPos();
    ImVec2 Origin = ImGui::GetCursorPos();
    Origin.x += Gap;
    Origin.y += Gap;
    float const CellWidth = (Avail.x - Gap * (Columns + 1)) / Columns;
    float const CellHeight = (Avail.y - Gap * (Rows + 1)) / Rows;

    for (int Index = 0; Index < Count; Index++)
    {
        Session* const CurrentSession = FindSession(Ids[(size_t)Index]);
        if (CurrentSession == nullptr)
        {
            continue;
        }
        int Row = Index / Columns, Col = Index % Columns;
        float Width = CellWidth;
        if (Index == Count - 1 && Count % Columns != 0)
        {
            Width = CellWidth * (Columns - (Count % Columns) + 1) + Gap * (Columns - (Count % Columns));
        }
        ImVec2 const Pos(Origin.x + Col * (CellWidth + Gap), Origin.y + Row * (CellHeight + Gap));
        DrawPane(*CurrentSession, Pos, ImVec2(Width, CellHeight), Index);
    }
    DrawPeerOverlay(PanesMin, ImVec2(PanesMin.x + Avail.x, PanesMin.y + Avail.y), Gap);
    ImGui::EndChild();
}

void App::BuildAndRun(Session& CurrentSession)
{
    const std::string Dir = CurrentSession.Rec.Cwd;
    if (BuildScripts::BothExist(Dir) == false)
    {
        const std::string Created = BuildScripts::CreateMissing(Dir);
        SendPrompt(CurrentSession, BuildScripts::SetupRequest(Created));
        for (const std::unique_ptr<Session>& Other : Sessions_)
        {
            if (Other.get() == &CurrentSession || Other->Rec.Cwd != Dir || Other->CurrentStatus != Status::Idle)
            {
                continue;
            }
            SendPrompt(*Other, BuildScripts::Guidance());
        }
        ShowToast("Created " + Created + " and asked the session to finish them. Click Build & Run again when it's done.");
        return;
    }
    Platform::RunScripts(Dir, "call build.bat && call run.bat");
}

float App::FeedbackPanelHeight()
{
    if (ProjectScopeId_.empty() == true || FindProject(ProjectScopeId_) == nullptr)
    {
        return 0.0f;
    }
    const float Scale = UiScale_;
    if (FeedbackOpen_ == false)
    {
        return ImGui::GetFrameHeight() + 12 * Scale;
    }
    return 200.0f * Scale;
}

void App::DrawMainColumn(const float Width)
{
    const float PanelHeight = FeedbackPanelHeight();
    ImGui::BeginGroup();
    DrawMain(Width, PanelHeight > 0.0f ? ImGui::GetContentRegionAvail().y - PanelHeight : 0.0f);
    if (PanelHeight > 0.0f)
    {
        DrawFeedbackPanel(Width);
    }
    ImGui::EndGroup();
}

void App::StartFeedbackSessions()
{
    const std::string ProjectId = ProjectScopeId_;
    std::vector<std::array<char, FEEDBACK_LINE_CAPACITY>>& Lines = FeedbackLines_[ProjectId];
    int Started = 0;
    for (const std::array<char, FEEDBACK_LINE_CAPACITY>& Line : Lines)
    {
        const std::string Text = Util::Trim(Line.data());
        if (Text.empty() == true)
        {
            continue;
        }
        std::string Err;
        const std::string Name = "Feedback: " + (Text.size() > 30 ? Text.substr(0, 30) + "..." : Text);
        Session* const NewSession = CreateSession(ProjectId, Name, false, false, Err);
        if (NewSession == nullptr)
        {
            ShowToast("Could not start feedback session: " + Err, true);
            ProjectScopeId_ = ProjectId;
            return;
        }
        NewSession->PendingPrompt = "Please address the following feedback for this project:\n\n" + Text;
        OpenSession(NewSession->Rec.Id);
        Started++;
    }
    Lines.clear();
    ProjectScopeId_ = ProjectId;
    ShowToast("Started " + std::to_string(Started) + " feedback session" + (Started == 1 ? "" : "s"));
}

std::vector<std::string> App::ParseFeedbackList(const std::string& Text)
{
    static const char* const BULLETS[] = {"- ", "* ", "\xE2\x80\xA2 ", "\xE2\x80\x93 ", "\xE2\x80\x94 "};
    std::vector<std::string> Items;
    int MarkerCount = 0;
    size_t Start = 0;
    while (Start <= Text.size())
    {
        size_t End = Text.find_first_of("\r\n", Start);
        if (End == std::string::npos)
        {
            End = Text.size();
        }
        const std::string Line = Util::Trim(Text.substr(Start, End - Start));
        Start = End + 1;
        if (Line.empty() == true)
        {
            continue;
        }
        size_t MarkerLength = 0;
        for (const char* const Bullet : BULLETS)
        {
            if (Line.rfind(Bullet, 0) == 0)
            {
                MarkerLength = std::strlen(Bullet);
                break;
            }
        }
        if (MarkerLength == 0)
        {
            size_t Digits = 0;
            while (Digits < Line.size() && std::isdigit(static_cast<unsigned char>(Line[Digits])) != 0)
            {
                Digits++;
            }
            const bool HasSeparator = Digits > 0 && Digits + 1 < Line.size() && (Line[Digits] == '.' || Line[Digits] == ')') && Line[Digits + 1] == ' ';
            if (HasSeparator == true)
            {
                MarkerLength = Digits + 2;
            }
        }
        if (MarkerLength > 0)
        {
            MarkerCount++;
            Items.push_back(Util::Trim(Line.substr(MarkerLength)));
            continue;
        }
        if (Items.empty() == true)
        {
            Items.push_back(Line);
            continue;
        }
        Items.back() += " " + Line;
    }
    if (MarkerCount < 2)
    {
        Items.clear();
    }
    return Items;
}

bool App::PasteFeedbackList(std::vector<std::array<char, FEEDBACK_LINE_CAPACITY>>& Lines, const size_t Index)
{
    const char* const Clipboard = ImGui::GetClipboardText();
    if (Clipboard == nullptr)
    {
        return false;
    }
    const std::vector<std::string> Items = ParseFeedbackList(Clipboard);
    if (Items.empty() == true)
    {
        return false;
    }
    size_t Target = Index;
    if (Lines[Index][0] != '\0')
    {
        Target = Index + 1;
        Lines.emplace(Lines.begin() + static_cast<std::ptrdiff_t>(Target));
    }
    for (size_t ItemIndex = 0; ItemIndex < Items.size(); ItemIndex++)
    {
        if (ItemIndex > 0)
        {
            Target++;
            Lines.emplace(Lines.begin() + static_cast<std::ptrdiff_t>(Target));
        }
        Lines[Target].fill('\0');
        std::strncpy(Lines[Target].data(), Items[ItemIndex].c_str(), FEEDBACK_LINE_CAPACITY - 1);
    }
    Lines.emplace(Lines.begin() + static_cast<std::ptrdiff_t>(Target) + 1);
    Lines[Target + 1].fill('\0');
    FeedbackFocusIndex_ = static_cast<int>(Target) + 1;
    return true;
}

void App::DrawFeedbackPanel(const float Width)
{
    const float Scale = UiScale_;
    std::vector<std::array<char, FEEDBACK_LINE_CAPACITY>>& Lines = FeedbackLines_[ProjectScopeId_];
    if (Lines.empty() == true)
    {
        Lines.emplace_back();
        Lines.back().fill('\0');
    }
    int FilledCount = 0;
    for (const std::array<char, FEEDBACK_LINE_CAPACITY>& Line : Lines)
    {
        if (Line[0] != '\0')
        {
            FilledCount++;
        }
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 4 * Scale));
    ImGui::BeginChild("##feedback", ImVec2(Width, FeedbackPanelHeight()), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::AlignTextToFramePadding();
    if (ImGui::Button(FeedbackOpen_ == true ? "v##feedbackToggle" : "^##feedbackToggle", ImVec2(ImGui::GetFrameHeight(), 0)) == true)
    {
        FeedbackOpen_ = FeedbackOpen_ == false;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Feedback");
    ImGui::SameLine();
    ImGui::TextDisabled("%d", FilledCount);

    const std::string StartLabel = "Start " + std::to_string(FilledCount) + " session" + (FilledCount == 1 ? "" : "s") + "###feedbackStart";
    const float StartWidth = ImGui::CalcTextSize(StartLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - StartWidth - 8 * Scale);
    ImGui::BeginDisabled(FilledCount == 0);
    const bool StartClicked = ImGui::Button(StartLabel.c_str());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
    {
        ImGui::SetTooltip("Open a new window and Claude session for each line of feedback");
    }

    if (FeedbackOpen_ == true)
    {
        ImGui::BeginChild("##feedbackLines", ImVec2(0, 0), ImGuiChildFlags_None);
        const float RemoveWidth = ImGui::GetFrameHeight();
        for (size_t Index = 0; Index < Lines.size(); Index++)
        {
            ImGui::PushID(static_cast<int>(Index));
            const bool PasteRequested = ImGui::GetIO().KeyCtrl == true && ImGui::IsKeyPressed(ImGuiKey_V) == true;
            if (PasteRequested == true && ImGui::GetActiveID() == ImGui::GetID("##line") && PasteFeedbackList(Lines, Index) == true)
            {
                ImGui::PopID();
                break;
            }
            if (FeedbackFocusIndex_ == static_cast<int>(Index))
            {
                ImGui::SetKeyboardFocusHere();
                FeedbackFocusIndex_ = -1;
            }
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - RemoveWidth - ImGui::GetStyle().ItemSpacing.x);
            const bool Submitted = ImGui::InputTextWithHint("##line", "Type feedback, press Enter for another", Lines[Index].data(), Lines[Index].size(),
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool Removed = ImGui::Button("x", ImVec2(RemoveWidth, 0));
            ImGui::PopID();
            if (Submitted == true)
            {
                Lines.emplace(Lines.begin() + static_cast<std::ptrdiff_t>(Index) + 1);
                Lines[Index + 1].fill('\0');
                FeedbackFocusIndex_ = static_cast<int>(Index) + 1;
                break;
            }
            if (Removed == true)
            {
                Lines.erase(Lines.begin() + static_cast<std::ptrdiff_t>(Index));
                break;
            }
        }
        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    if (StartClicked == true)
    {
        StartFeedbackSessions();
    }
}

void App::DrawMainAndPanel()
{
    const float Scale = UiScale_;
    constexpr float STRIP_WIDTH = 28.0f;
    constexpr float MIN_PANEL_WIDTH = 240.0f;
    constexpr float MAX_PANEL_WIDTH = 640.0f;
    const float SplitterWidth = 5.0f * Scale;

    if (State_.Ui.ScreenshotPanelOpen == false)
    {
        DrawMainColumn(ImGui::GetContentRegionAvail().x - STRIP_WIDTH * Scale);
        ImGui::SameLine(0, 0);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * Scale, 10 * Scale));
        ImGui::BeginChild("##shotsCollapsed", ImVec2(STRIP_WIDTH * Scale, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::Button("<", ImVec2(-1, 0)) == true)
        {
            State_.Ui.ScreenshotPanelOpen = true;
            UnseenScreenshots_ = 0;
            MarkDirty();
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Show screenshots");
        }
        if (UnseenScreenshots_ > 0)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, AccentColorV4_);
            ImGui::Text("%d", UnseenScreenshots_);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    const float PanelWidth = std::clamp(State_.Ui.ScreenshotPanelWidth, MIN_PANEL_WIDTH * Scale, MAX_PANEL_WIDTH * Scale);
    LayoutDebug_.Sample("mainAndPanel.availX", ImGui::GetContentRegionAvail().x);
    LayoutDebug_.Sample("panel.width", PanelWidth);
    DrawMainColumn(ImGui::GetContentRegionAvail().x - PanelWidth - SplitterWidth);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleColor(ImGuiCol_Button, PaneBorderColorV4_);
    ImGui::InvisibleButton("##shotsSplitter", ImVec2(SplitterWidth, -1));
    const bool SplitterActive = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() == true || SplitterActive == true)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (SplitterActive == true)
    {
        State_.Ui.ScreenshotPanelWidth = std::clamp(PanelWidth - ImGui::GetIO().MouseDelta.x, MIN_PANEL_WIDTH * Scale, MAX_PANEL_WIDTH * Scale);
        MarkDirty();
    }
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), SplitterActive == true ? AccentColor_ : PaneBorderColorU32_);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);

    DrawScreenshotPanel(PanelWidth);
}
