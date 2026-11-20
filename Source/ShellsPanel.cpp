#include "App.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "Platform.h"
#include "UiHelpers.h"

ShellRow App::BuildShellRow(const TranscriptScanner::ActiveCommand& Command, const std::time_t Now) const
{
    using ActiveCommand = TranscriptScanner::ActiveCommand;
    constexpr double SLOWER_THAN_USUAL = 1.15;
    ShellRow Row;
    const double Elapsed = static_cast<double>(std::max<std::time_t>(0, Now - Command.StartedAt));
    Row.Kind = Command.Type == ActiveCommand::Kind::Shell ? "shell" : Command.Type == ActiveCommand::Kind::Background ? "background" : "monitor";
    Row.Title = Command.Description.empty() == false ? Command.Description : Command.Command;
    const size_t LineEnd = Row.Title.find_first_of("\r\n");
    if (LineEnd != std::string::npos)
    {
        Row.Title = Row.Title.substr(0, LineEnd);
    }
    Row.Elapsed = FormatDuration(Elapsed);
    const double Typical = Command.Type == ActiveCommand::Kind::Monitor ? -1 : CommandTiming_.Typical(CommandTiming::KeyFor(Command.Command));
    const double Limit = Command.Deadline != 0 ? static_cast<double>(Command.Deadline - Command.StartedAt) : 0;
    const std::string LimitText = Command.Type == ActiveCommand::Kind::Shell && Limit > 0 ? "  -  stops at the " + FormatRoughDuration(Limit) + " limit" : "";
    if (Command.Type == ActiveCommand::Kind::Monitor && Limit > 0)
    {
        Row.Fraction = static_cast<float>(Elapsed / Limit);
        Row.Fill = AccentColor_;
        Row.Hint = "watching  -  ends on its own in " + FormatRoughDuration(std::max(0.0, Limit - Elapsed));
    }
    else if (Typical > 0 && Elapsed > Typical * SLOWER_THAN_USUAL)
    {
        Row.Fraction = 1.0f;
        Row.Fill = UsageWarningColor_;
        Row.Hint = "taking longer than usual (~" + FormatRoughDuration(Typical) + ")" + LimitText;
    }
    else if (Typical > 0)
    {
        Row.Fraction = static_cast<float>(Elapsed / Typical);
        Row.Fill = AccentColor_;
        Row.Hint = "usually ~" + FormatRoughDuration(Typical) + ", about " + FormatRoughDuration(std::max(0.0, Typical - Elapsed)) + " left" + LimitText;
    }
    else if (Command.Type == ActiveCommand::Kind::Shell && Limit > 0)
    {
        Row.Fraction = static_cast<float>(Elapsed / Limit);
        Row.Fill = UiHelpers::WithAlpha(TextDisabledColorU32_, 170);
        Row.Hint = "no estimate yet" + LimitText;
    }
    else
    {
        Row.Indeterminate = true;
        Row.Fill = UiHelpers::WithAlpha(AccentColor_, 200);
        Row.Hint = Command.Type == ActiveCommand::Kind::Monitor ? "watching" : "no estimate yet  -  runs until it finishes";
    }
    Row.Fraction = std::clamp(Row.Fraction, 0.0f, 1.0f);
    return Row;
}

void App::DrawShellsTab()
{
    const bool ProjectSelected = ProjectScopeId_.empty() == false;
    const std::vector<const Session*> Scoped = ShellPanelSessions();
    static const std::vector<TranscriptScanner::ActiveCommand> NO_COMMANDS;

    ImGui::TextDisabled("%s", ProjectSelected == true ? "From every session in this project" : Scoped.size() > 1 ? "From the selected session and its children" : "From the selected session");

    ImGui::BeginChild("##shellList", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const float CardWidth = ImGui::GetContentRegionAvail().x;
    bool AnyShown = false;
    for (const Session* const Owner : Scoped)
    {
        const std::string& OwnerId = Owner->Rec.Id;
        const std::map<std::string, std::vector<TranscriptScanner::ActiveCommand>>::const_iterator Found = RunningCommands_.find(OwnerId);
        const std::vector<TranscriptScanner::ActiveCommand>& Running = Found == RunningCommands_.end() ? NO_COMMANDS : Found->second;
        const std::vector<ShellHistory::Finished>& Done = ShellHistory_.For(OwnerId);
        if (Running.empty() == true && Done.empty() == true)
        {
            continue;
        }
        AnyShown = true;
        ImGui::PushID(OwnerId.c_str());
        if (Scoped.size() > 1)
        {
            ImGui::TextUnformatted(Owner->Rec.Name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%d running", static_cast<int>(Running.size()));
        }
        for (size_t Index = 0; Index < Running.size(); Index++)
        {
            ImGui::PushID(static_cast<int>(Index));
            DrawShellCard(Running[Index], CardWidth, false, 0);
            ImGui::PopID();
        }
        if (Done.empty() == false)
        {
            ImGui::TextDisabled("Finished");
            for (size_t Index = 0; Index < Done.size(); Index++)
            {
                ImGui::PushID(static_cast<int>(Index) + 1000);
                DrawShellCard(Done[Index].Command, CardWidth, true, Done[Index].EndedAt);
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    if (AnyShown == false)
    {
        ImGui::TextDisabled("%s", ProjectSelected == true ? "No shell commands from this project's sessions." : "No shell commands running in this session.");
    }
    ImGui::EndChild();
}

void App::DrawShellCard(const TranscriptScanner::ActiveCommand& Command, const float Width, const bool Finished, const std::time_t EndedAt)
{
    constexpr size_t MAX_OUTPUT_LINES = 8;
    const float Scale = UiScale_;
    const float Pad = 8 * Scale;
    const float LineHeight = ImGui::GetTextLineHeight();
    const float BarHeight = 6 * Scale;
    const float Gap = 4 * Scale;

    ShellRow Row = BuildShellRow(Command, Finished == true ? EndedAt : std::time(nullptr));
    if (Finished == true)
    {
        Row.Hint = "finished";
    }

    std::vector<std::string> OutputLines;
    if (Command.OutputFile.empty() == false)
    {
        const std::vector<std::string>& Tail = ShellOutputTail_.Lines(Command.OutputFile, Now_);
        const size_t First = Tail.size() > MAX_OUTPUT_LINES ? Tail.size() - MAX_OUTPUT_LINES : 0;
        OutputLines.assign(Tail.begin() + static_cast<std::ptrdiff_t>(First), Tail.end());
    }

    float Height = Pad * 2 + LineHeight + Gap + LineHeight;
    if (Finished == false)
    {
        Height += BarHeight + Gap;
    }
    if (OutputLines.empty() == false)
    {
        Height += Gap + static_cast<float>(OutputLines.size()) * LineHeight;
    }
    if (Command.OutputFile.empty() == false)
    {
        Height += Gap + ImGui::GetFrameHeight();
    }

    const ImVec2 Min = ImGui::GetCursorScreenPos();
    const ImVec2 Max(Min.x + Width, Min.y + Height);
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    DrawList->AddRectFilled(Min, Max, IM_COL32(12, 14, 18, 255), 6 * Scale);
    DrawList->AddRect(Min, Max, Finished == true ? UiHelpers::WithAlpha(TextDisabledColorU32_, 90) : UiHelpers::WithAlpha(AccentColor_, 200), 6 * Scale);

    const float Left = Min.x + Pad;
    const float Inner = Width - Pad * 2;
    float Y = Min.y + Pad;
    const float KindWidth = ImGui::CalcTextSize(Row.Kind.c_str()).x;
    const float ElapsedWidth = ImGui::CalcTextSize(Row.Elapsed.c_str()).x;
    DrawList->AddRectFilled(ImVec2(Left - 3 * Scale, Y - Scale), ImVec2(Left + KindWidth + 3 * Scale, Y + LineHeight + Scale), UiHelpers::WithAlpha(AccentColor_, 70), 3 * Scale);
    DrawList->AddText(ImVec2(Left, Y), TextColorU32_, Row.Kind.c_str());
    const std::string Title = TruncateToWidth(Row.Title, Inner - KindWidth - ElapsedWidth - 20 * Scale);
    DrawList->AddText(ImVec2(Left + KindWidth + 12 * Scale, Y), TextColorU32_, Title.c_str());
    DrawList->AddText(ImVec2(Left + Inner - ElapsedWidth, Y), TextColorU32_, Row.Elapsed.c_str());
    Y += LineHeight + Gap;

    if (Finished == false)
    {
        const ImVec2 TrackMin(Left, Y);
        DrawList->AddRectFilled(TrackMin, ImVec2(Left + Inner, Y + BarHeight), UiHelpers::WithAlpha(TextDisabledColorU32_, 45), BarHeight * 0.5f);
        if (Row.Indeterminate == true)
        {
            const float Segment = Inner * 0.25f;
            const float Phase = static_cast<float>(std::fmod(Now_ * 0.6, 2.0));
            const float Travel = Phase < 1.0f ? Phase : 2.0f - Phase;
            const float SegmentLeft = Left + (Inner - Segment) * Travel;
            DrawList->AddRectFilled(ImVec2(SegmentLeft, Y), ImVec2(SegmentLeft + Segment, Y + BarHeight), Row.Fill, BarHeight * 0.5f);
        }
        else if (Row.Fraction > 0)
        {
            DrawList->AddRectFilled(TrackMin, ImVec2(Left + Inner * Row.Fraction, Y + BarHeight), Row.Fill, BarHeight * 0.5f);
        }
        Y += BarHeight + Gap;
    }

    const std::string Hint = TruncateToWidth(Row.Hint, Inner);
    DrawList->AddText(ImVec2(Left, Y), TextDisabledColorU32_, Hint.c_str());
    Y += LineHeight;

    if (OutputLines.empty() == false)
    {
        Y += Gap;
        for (const std::string& Line : OutputLines)
        {
            const std::string Shown = TruncateToWidth(Line, Inner);
            DrawList->AddText(ImVec2(Left, Y), TextColorU32_, Shown.c_str());
            Y += LineHeight;
        }
    }

    ImGui::SetCursorScreenPos(Min);
    ImGui::InvisibleButton("##shellCard", ImVec2(Width, Height));
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        if (Command.Description.empty() == false)
        {
            ImGui::TextUnformatted(Command.Description.c_str());
        }
        ImGui::TextDisabled("%s", Command.Command.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    if (Command.OutputFile.empty() == false)
    {
        ImGui::SetCursorScreenPos(ImVec2(Left, Y + Gap));
        if (ImGui::SmallButton("Show file") == true)
        {
            Platform::Reveal(Command.OutputFile);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy path") == true)
        {
            ImGui::SetClipboardText(Command.OutputFile.c_str());
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(Min.x, Max.y));
    ImGui::Dummy(ImVec2(Width, 0));
}
