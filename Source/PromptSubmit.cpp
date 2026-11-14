#include "App.h"

#include <cctype>

namespace
{
struct SentTextMatcher
{
    static std::string Compact(const std::string& Text)
    {
        std::string Result;
        for (const char Character : Text)
        {
            if (std::isspace(static_cast<unsigned char>(Character)) == 0)
            {
                Result += Character;
            }
        }
        return Result;
    }

    // The box text must be part of the prompt the app sent, allowing for wrapping and a trailing ellipsis.
    static bool Matches(const std::string& BoxText, const std::string& SentText)
    {
        std::string Box = Compact(BoxText);
        static const std::string ELLIPSIS = "\xE2\x80\xA6";
        if (Box.size() >= ELLIPSIS.size() && Box.compare(Box.size() - ELLIPSIS.size(), ELLIPSIS.size(), ELLIPSIS) == 0)
        {
            Box.erase(Box.size() - ELLIPSIS.size());
        }
        if (Box.empty() == true)
        {
            return false;
        }
        if (Box.rfind("[Pastedtext", 0) == 0 || Box.rfind("[Image", 0) == 0)
        {
            return true;
        }
        return Compact(SentText).find(Box) != std::string::npos;
    }
};
}

// Claude Code sometimes drops the Enter that follows a pasted prompt, so a sent prompt is checked a moment later
// and Enter is sent again while the text is still sitting in the prompt box.
void App::ConfirmSubmitted(Session& Target)
{
    static constexpr double RECHECK_SECONDS = 0.8;
    static constexpr double MAX_SUBMIT_SECONDS = 30.0;
    if (Target.SubmitCheckAt == 0 || Now_ < Target.SubmitCheckAt)
    {
        return;
    }
    if (Target.Term == nullptr || Target.CurrentStatus == Status::Exited || Target.LastInputAt() > Target.SubmitInputMark)
    {
        Target.SubmitCheckAt = 0;
        return;
    }
    int FirstRow = 0;
    int FirstCol = 0;
    int LastRow = 0;
    int LastCol = 0;
    const bool Found = Target.Term->FindInputText(FirstRow, FirstCol, LastRow, LastCol) == true
        && Target.Term->AltScreen() == false
        && SentTextMatcher::Matches(Target.Term->GetText(FirstRow, FirstCol, LastRow, LastCol), Target.SubmittedText) == true;
    if (Target.CurrentStatus == Status::Busy || Target.CurrentStatus == Status::Waiting)
    {
        Target.SubmitCheckAt = 0;
        // The prompt was accepted, but its text can stay in the box; Ctrl+U clears one line, so send one per line.
        if (Target.CurrentStatus == Status::Busy && Found == true)
        {
            const int LineCount = LastRow - FirstRow + 1;
            for (int LineIndex = 0; LineIndex < LineCount; LineIndex++)
            {
                Target.SendText("\x15", Now_, StatusCallback());
            }
        }
        return;
    }
    if (Found == false || Now_ - Target.SubmitStartedAt >= MAX_SUBMIT_SECONDS)
    {
        Target.SubmitCheckAt = 0;
        return;
    }
    Target.SubmitCheckAt = Now_ + RECHECK_SECONDS;
    Target.SendText("\r", Now_, StatusCallback());
    Target.SubmitInputMark = Target.LastInputAt();
}
