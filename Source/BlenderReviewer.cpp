#include "App.h"

#include <string>

#include "PeerNames.h"

std::string App::BlenderReviewerName(const Session& Mesh) const
{
    return Mesh.Rec.Name + " reviewer";
}

bool App::IsBlenderReviewer(const Session& Candidate) const
{
    if (Candidate.Rec.ParentId.empty() == true)
    {
        return false;
    }
    const Session* const Parent = FindSession(Candidate.Rec.ParentId);
    return Parent != nullptr && Candidate.Rec.Name == BlenderReviewerName(*Parent);
}

Session* App::FindBlenderReviewer(const Session& Mesh) const
{
    const std::string Name = BlenderReviewerName(Mesh);
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        if (Candidate->Rec.ParentId == Mesh.Rec.Id && Candidate->Rec.Name == Name)
        {
            return Candidate.get();
        }
    }
    return nullptr;
}

bool App::InBlenderReview(const std::string& SessionId) const
{
    if (BlenderReviewId_.empty() == true)
    {
        return false;
    }
    if (SessionId == BlenderReviewId_)
    {
        return true;
    }
    const Session* const Mesh = FindSession(BlenderReviewId_);
    if (Mesh == nullptr)
    {
        return false;
    }
    const Session* const Reviewer = FindBlenderReviewer(*Mesh);
    return Reviewer != nullptr && Reviewer->Rec.Id == SessionId;
}

std::string App::BlenderReviewerBrief(const Session& Mesh, const Session& Reviewer)
{
    const std::string MeshTag = ScreenshotFinder::SessionTag(Mesh.Rec.Id);
    const std::string MeshPeer = PeerNames::For(Mesh.Rec.Name, Mesh.Rec.Id);
    const std::string LivePicture = BlenderLive::FramePathFor(BlenderOwnDirectory(Mesh), MeshTag);
    return "You are the 3D model reviewer for the session '" + Mesh.Rec.Name + "' (its ListAgents name is '" + MeshPeer + "', tag " + MeshTag +
           "). That session makes and edits a 3D model in Blender and is your parent; the terminal app started you from its Blender review window "
           "and shows you to the user next to a live picture of the model. The user marks what they dislike on pictures of the model (drawn lines, "
           "rectangles and circles, each with a number and a note) and the app sends the marked pictures to you as files. Your job: "
           "(1) when a review arrives, Read each marked picture, look at every numbered mark and judge what is wrong in modelling terms; "
           "(2) turn every number into one precise instruction (what to change, where on the model, roughly how much), keep the user's numbers, "
           "and send them all to '" + MeshPeer + "' with one SendMessage; "
           "(3) when that session reports back, check the result by reading the newest live picture of the model at " + LivePicture +
           " (it is refreshed while the review window is open; you can also ask '" + MeshPeer + "' to take a screenshot), then tell the user per number what "
           "is fixed and what still needs work, and send a follow-up to '" + MeshPeer + "' for what is not right yet. "
           "A review may come with extra lines per picture: scene facts, a .blend copy of the scene at that moment, a one-line Python call that returns "
           "Blender's 3D view to that exact angle and saves a new picture, an enlarged crop for each mark, and reference images the user dropped on a mark "
           "(open those with Read: they show what the user wants that part to look like). Later reviews are numbered rounds that list earlier notes; point "
           "out anything that has come back. Pass what the model session needs to it. "
           "Do not change the model yourself and do not use the Blender tools: '" + MeshPeer + "' owns Blender. '" + MeshPeer +
           "' can message you any time with questions or to ask you to look at the model again, and the user can talk to you directly: answer them. "
           "Find '" + MeshPeer + "' in ListAgents when you first need it (its name ends with " + MeshTag + "). Keep messages short. "
           "Reply now with one word, Ready, and then wait for the user.";
}

Session* App::EnsureBlenderReviewer(Session& Mesh)
{
    Session* const Existing = FindBlenderReviewer(Mesh);
    if (Existing != nullptr)
    {
        if (Existing->IsRunning() == false)
        {
            Existing->Rec.WasRunning = true;
        }
        return Existing;
    }
    std::string Err;
    Session* const Fresh = CreateSession(Mesh.Rec.ProjectId, BlenderReviewerName(Mesh), false, false, Err);
    if (Fresh == nullptr)
    {
        ShowToast("Could not start the reviewer session: " + Err, true);
        return nullptr;
    }
    Fresh->Rec.Cwd = Mesh.Rec.Cwd;
    Fresh->Rec.ParentId = Mesh.Rec.Id;
    Fresh->Rec.Model = BLENDER_REVIEWER_MODEL;
    Fresh->PendingPrompt = BlenderReviewerBrief(Mesh, *Fresh);
    Fresh->Rec.WasRunning = true;
    State_.Ui.Collapsed[Mesh.Rec.ProjectId] = false;
    MarkDirty();
    return Fresh;
}

void App::SendToBlenderReviewer(Session& Reviewer, const std::string& Text)
{
    if (Reviewer.PendingPrompt.empty() == false || Reviewer.IsRunning() == false)
    {
        Reviewer.PendingPrompt += (Reviewer.PendingPrompt.empty() == true ? "" : "\n\n") + Text;
        Reviewer.Rec.WasRunning = true;
        return;
    }
    SendWhenSafe(Reviewer, Text);
}

void App::DrawBlenderRightHeader(Session& Mesh, Session* const Reviewer)
{
    const float Scale = UiScale_;
    ImGui::SetCursorPos(ImVec2(4.0f * Scale, 3.0f * Scale));
    if (Reviewer == nullptr)
    {
        if (ImGui::Button("Start Opus reviewer") == true)
        {
            EnsureBlenderReviewer(Mesh);
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Starts an Opus session under %s that reviews your marks and talks to it for you", Mesh.Rec.Name.c_str());
        }
        return;
    }
    const std::string ReviewerLabel = "Reviewer (" + ModelLabelFor(*Reviewer) + ")###reviewerTab";
    const std::string MeshLabel = Mesh.Rec.Name + "###meshTab";
    const Session* const Shown = BlenderRightShowsMesh_ == true ? &Mesh : Reviewer;
    for (int Index = 0; Index < 2; Index++)
    {
        Session& Choice = Index == 0 ? *Reviewer : Mesh;
        const bool Selected = Shown == &Choice;
        if (Index > 0)
        {
            ImGui::SameLine();
        }
        if (Selected == true)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
        }
        if (ImGui::Button(Index == 0 ? ReviewerLabel.c_str() : MeshLabel.c_str()) == true)
        {
            BlenderRightShowsMesh_ = Index == 1;
            FocusSession(Choice.Rec.Id);
        }
        if (Selected == true)
        {
            ImGui::PopStyleColor();
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("They message each other; you can talk to either");
}
