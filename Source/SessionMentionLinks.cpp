#include "App.h"

#include <string>

#include "PeerNames.h"
#include "ScreenshotFinder.h"
#include "SessionMentions.h"

std::string App::FindSessionMention(const Session& Self, const Terminal& Term, const int AbsRow, const int Col) const
{
    SessionMentions Mentions;
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        if (Candidate->Rec.Id == Self.Rec.Id)
        {
            continue;
        }
        Mentions.Add(Candidate->Rec.Name, Candidate->Rec.Id);
        Mentions.Add(PeerNames::For(Candidate->Rec.Name, Candidate->Rec.Id), Candidate->Rec.Id);
        Mentions.Add(ScreenshotFinder::SessionTag(Candidate->Rec.Id), Candidate->Rec.Id);
        Mentions.Add(Candidate->Rec.ClaudeSessionId, Candidate->Rec.Id);
    }
    return Mentions.Find(Term, AbsRow, Col);
}

// Runs once per frame outside pane drawing, because opening a session changes the pane layout.
void App::ApplyMentionFocus()
{
    if (PendingMentionFocusId_.empty() == true)
    {
        return;
    }
    const std::string Id = PendingMentionFocusId_;
    PendingMentionFocusId_.clear();
    OpenSessionFromSidebar(Id);
}
