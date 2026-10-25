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


void App::DrawReview()
{
    const float Scale = UiScale_;
    const ImGuiStyle& Style = ImGui::GetStyle();
    const float ButtonRowHeight = ImGui::GetFrameHeight() + Style.ItemSpacing.y;
    const float PendingHeight = Review_.Pending != GitReview::Action::None ? ImGui::GetTextLineHeightWithSpacing() * 2.0f + Style.ItemSpacing.y : 0.0f;
    const float FooterHeight = ButtonRowHeight + PendingHeight;
    const Session* const ReviewSession = FindSession(ModalTarget_);
    if (Review_.Error.empty() == false)
    {
        ImGui::TextColored(WaitingColorV4_, "%s", Review_.Error.c_str());
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetContentRegionAvail().y - ButtonRowHeight);
        return;
    }
    if (Review_.Upstream.empty() == false)
    {
        ImGui::TextDisabled("Branch history - commits marked \"not pushed\" are only on this computer (upstream: %s)", Review_.Upstream.c_str());
    }
    else
    {
        ImGui::TextDisabled("This branch has no upstream yet - nothing is marked as pushed, so only Revert is offered for older commits");
    }

    const float ListWidth = 320.0f * Scale;
    const float BodyHeight = ImGui::GetContentRegionAvail().y - FooterHeight;
    ImGui::BeginChild("##reviewList", ImVec2(ListWidth, BodyHeight), ImGuiChildFlags_Borders);
    if (Review_.Entries.empty() == true)
    {
        ImGui::TextDisabled("Nothing to review.");
    }
    int ClickedIndex = -1;
    for (size_t Index = 0; Index < Review_.Entries.size(); Index++)
    {
        const GitReview::Entry& Item = Review_.Entries[Index];
        ImGui::PushID(static_cast<int>(Index));
        const std::string Label = Item.IsWorkingTree == true ? Item.Subject : Item.ShortHash + "  " + Item.Subject;
        if (ImGui::Selectable(Label.c_str(), Review_.Selected == static_cast<int>(Index)) == true)
        {
            ClickedIndex = static_cast<int>(Index);
        }
        if (Item.IsWorkingTree == false)
        {
            ImGui::TextDisabled("    %s, %s", Item.Author.c_str(), Item.When.c_str());
            if (Item.Unpushed == true)
            {
                ImGui::SameLine();
                ImGui::TextColored(AccentColorV4_, "not pushed");
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ClickedIndex >= 0)
    {
        Review_.Select(ClickedIndex);
        Review_.Pending = GitReview::Action::None;
    }

    ImGui::SameLine();
    ImGui::BeginChild("##reviewDiff", ImVec2(0, BodyHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (ReviewShowActivity_ == true)
    {
        if (Review_.ActivityLog.empty() == true)
        {
            ImGui::TextDisabled("No actions yet. Reverts and discards you run here are listed in this log (this window session only).");
        }
        for (const std::string& LogLine : Review_.ActivityLog)
        {
            ImGui::TextUnformatted(LogLine.c_str());
        }
    }
    else
    {
        ImGuiListClipper Clipper;
        Clipper.Begin(static_cast<int>(Review_.DiffLines.size()));
        while (Clipper.Step() == true)
        {
            for (int LineIndex = Clipper.DisplayStart; LineIndex < Clipper.DisplayEnd; LineIndex++)
            {
                const std::string& Line = Review_.DiffLines[(size_t)LineIndex];
                ImVec4 LineColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                if (Line.rfind("+++", 0) == 0 || Line.rfind("---", 0) == 0 || Line.rfind("diff --git", 0) == 0 || Line.rfind("index ", 0) == 0)
                {
                    LineColor = TextDisabledColorV4_;
                }
                else if (Line.rfind("@@", 0) == 0)
                {
                    LineColor = AccentColorV4_;
                }
                else if (Line.empty() == false && Line[0] == '+')
                {
                    LineColor = ImGui::ColorConvertU32ToFloat4(DoneColor_);
                }
                else if (Line.empty() == false && Line[0] == '-')
                {
                    LineColor = WaitingColorV4_;
                }
                ImGui::TextColored(LineColor, "%s", Line.c_str());
            }
        }
    }
    ImGui::EndChild();

    if (Review_.Pending != GitReview::Action::None)
    {
        ImGui::TextColored(WaitingColorV4_, "%s", GitReview::ActionTitle(Review_.Pending));
        ImGui::TextDisabled("%s", GitReview::ActionDetail(Review_.Pending));
        if (ImGui::Button("Yes, do it") == true)
        {
            const std::string Outcome = Review_.RunPending();
            ShowToast(Outcome, Outcome.rfind("Failed", 0) == 0);
            MarkDirty();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") == true)
        {
            Review_.Pending = GitReview::Action::None;
        }
        return;
    }

    const std::function<void(const char*, GitReview::Action, bool, const char*)> ActionButton =
        [&](const char* Label, const GitReview::Action Value, const bool Enabled, const char* DisabledReason)
    {
        ImGui::BeginDisabled(Enabled == false);
        const bool Clicked = ImGui::Button(Label);
        ImGui::EndDisabled();
        if (Enabled == false && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
        {
            ImGui::SetTooltip("%s", DisabledReason);
        }
        if (Clicked == true)
        {
            Review_.Pending = Value;
        }
        ImGui::SameLine();
    };
    ActionButton("Revert commit", GitReview::Action::Revert, Review_.CanRevert(), "Select a commit to revert");
    ActionButton("Undo commit", GitReview::Action::UndoCommit, Review_.CanRewriteHead(), "Only the newest commit can be undone, and only before it is pushed");
    ActionButton("Discard commit", GitReview::Action::DiscardCommit, Review_.CanRewriteHead(), "Only the newest commit can be discarded, and only before it is pushed");
    ActionButton("Discard changes", GitReview::Action::DiscardChanges, Review_.CanDiscardChanges(), "Select \"Uncommitted changes\" to discard them");

    ImGui::Checkbox("Activity log", &ReviewShowActivity_);
    ImGui::SameLine();

    const bool GitHubReady = State_.AppSettings.GitHubEnabled == true && ReadGitHubStatus().Value == Git::GitHubStatus::State::LoggedIn;
    const bool CanPush = GitHubReady == true && ReviewSession != nullptr && ReviewSession->IsRunning() == true;
    ImGui::BeginDisabled(CanPush == false);
    const bool PushClicked = ImGui::Button("Push");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) == true)
    {
        ImGui::SetTooltip("%s", CanPush == true ? "Ask Claude to push the current branch to GitHub" : "Needs GitHub access and a running session");
    }
    if (PushClicked == true)
    {
        Session* const PushSession = FindSession(ModalTarget_);
        if (PushSession != nullptr)
        {
            SendPrompt(*PushSession, PUSH_PROMPT);
        }
        Modal_ = Modal::None;
        ImGui::CloseCurrentPopup();
        return;
    }
    ImGui::SameLine();
}
