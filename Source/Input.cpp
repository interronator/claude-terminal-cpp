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


void App::DeleteSelectedInput(Session& Target)
{
    Target.InputSelected = false;
    Target.HasSelection = false;
    if (Target.Term == nullptr || Target.IsRunning() == false)
    {
        return;
    }
    const Terminal& Term = *Target.Term;
    int FirstRow = Term.AbsRow(Target.SelectionStartRow);
    int FirstCol = Target.SelectionStartCol;
    int LastRow = Term.AbsRow(Target.SelectionEndRow);
    int LastCol = Target.SelectionEndCol;
    if (FirstRow > LastRow || (FirstRow == LastRow && FirstCol > LastCol))
    {
        std::swap(FirstRow, LastRow);
        std::swap(FirstCol, LastCol);
    }
    const std::string Text = Term.GetText(FirstRow, FirstCol, LastRow, LastCol);
    size_t Count = 2;
    for (const char Character : Text)
    {
        if ((static_cast<unsigned char>(Character) & 0xC0) != 0x80)
        {
            Count++;
        }
    }
    // The cursor can be anywhere in the text: backspaces clear what is before it, deletes what is after it.
    std::string Bytes(Count, '\x7f');
    for (size_t Index = 0; Index < Count; Index++)
    {
        Bytes += "\x1b[3~";
    }
    Target.ScrollOffset = 0;
    Target.SendText(Bytes, Now_, StatusCallback());
}

void App::CaptureKeyBinding(const InputEvent& CurrentEvent)
{
    const int PressedKey = CurrentEvent.Key;
    if (KeyBindings::IsModifierKey(PressedKey) == true)
    {
        return;
    }
    if (PressedKey == GLFW_KEY_ESCAPE)
    {
        KeyCaptureIndex_ = -1;
        KeyCaptureEscapeFrame_ = ImGui::GetFrameCount();
        KeyCaptureError_.clear();
        return;
    }
    const KeyAction Action = KeyBindings::ActionAt(KeyCaptureIndex_);
    const int PressedMods = CurrentEvent.Mods & KeyBindings::MOD_MASK;
    if (PressedKey == GLFW_KEY_BACKSPACE && PressedMods == 0)
    {
        KeysEdit_.Set(Action, KeyChord{});
        KeyCaptureIndex_ = -1;
        KeyCaptureError_.clear();
        return;
    }
    const KeyChord Chord{PressedKey, PressedMods};
    if (KeyBindings::IsAllowed(Chord) == false)
    {
        KeyCaptureError_ = "Include Ctrl or Alt (or use an F-key) so normal typing keeps working.";
        return;
    }
    KeyAction Conflict = Action;
    if (KeysEdit_.FindConflict(Action, Chord, Conflict) == true)
    {
        KeyCaptureError_ = std::string("Already used by \"") + KeyBindings::ActionName(Conflict) + "\".";
        return;
    }
    KeysEdit_.Set(Action, Chord);
    KeyCaptureIndex_ = -1;
    KeyCaptureError_.clear();
}

void App::DrawKeybindingEditor()
{
    const float Scale = UiScale_;
    ImGui::Separator();
    ImGui::TextUnformatted("Keybindings");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset all") == true)
    {
        KeysEdit_.ResetAll();
        KeyCaptureIndex_ = -1;
        KeyCaptureError_.clear();
    }
    ImGui::BeginChild("##keybindings", ImVec2(-1, 230.0f * Scale), ImGuiChildFlags_Borders);
    for (int Index = 0; Index < KeyBindings::ACTION_COUNT; Index++)
    {
        const KeyAction Action = KeyBindings::ActionAt(Index);
        const bool Capturing = KeyCaptureIndex_ == Index;
        ImGui::PushID(Index);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(KeyBindings::ActionName(Action));
        ImGui::SameLine(170.0f * Scale);
        const std::string Label = (Capturing == true ? std::string("Press keys...") : KeyBindings::Describe(KeysEdit_.Get(Action))) + "###chord";
        if (Capturing == true)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.06f, 0.04f, 1));
        }
        if (ImGui::Button(Label.c_str(), ImVec2(140.0f * Scale, 0)) == true)
        {
            KeyCaptureIndex_ = Capturing == true ? -1 : Index;
            KeyCaptureError_.clear();
        }
        if (Capturing == true)
        {
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset") == true)
        {
            KeysEdit_.Reset(Action);
            KeyCaptureIndex_ = KeyCaptureIndex_ == Index ? -1 : KeyCaptureIndex_;
            KeyCaptureError_.clear();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (KeyCaptureError_.empty() == false)
    {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(WaitingColorV4_, "%s", KeyCaptureError_.c_str());
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("Click a shortcut, then press the new keys. Backspace unbinds, Esc cancels.");
}

// ---------------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------------

void App::PushChar(uint32_t CodePoint)
{
    bool const Ctrl = glfwGetKey(Window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(Window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    bool const Alt = glfwGetKey(Window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(Window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    if ((Ctrl || Alt) && (Ctrl == false || Alt == false))
    {
        return;  // handled from the key event (AltGr = ctrl+alt delivers chars)
    }
    InputEvent CurrentEvent;
    CurrentEvent.Type = InputEvent::EventType::Char;
    CurrentEvent.Cp = CodePoint;
    Events_.push_back(CurrentEvent);
}

void App::PushKey(int CurrentKey, int Action, int Mods)
{
    if (Action == GLFW_RELEASE)
    {
        return;
    }
    InputEvent CurrentEvent;
    CurrentEvent.Type = InputEvent::EventType::Key;
    CurrentEvent.Key = CurrentKey;
    CurrentEvent.Mods = Mods;
    CurrentEvent.Action = Action;
    Events_.push_back(CurrentEvent);
}

void App::PushDrop(const std::vector<std::string>& Paths)
{
    Session* const ActiveSession = InputSession();
    std::string FilePaths;
    for (const std::string& Path : Paths)
    {
        std::error_code ErrorCode;
        if (fs::is_directory(Path, ErrorCode))
        {
            std::string Err, Id;
            if (AddProjectPath(Path, Err, &Id))
            {
                State_.Ui.Collapsed[Id] = false;
                ShowToast("Added " + Util::BaseName(Path));
            }
            continue;
        }

        if (FilePaths.empty() == false)
        {
            FilePaths += ' ';
        }
        FilePaths += '"' + Path + '"';
    }

    if (FilePaths.empty() == false && ActiveSession != nullptr && ActiveSession->IsRunning())
    {
        ActiveSession->SendText(FilePaths, Now_, StatusCallback());
    }
}

void App::HandleEvents()
{
    ImGuiIO const& Io = ImGui::GetIO();
    bool const Blocked = Modal_ != Modal::None || Io.WantTextInput;
    while (Events_.empty() == false)
    {
        InputEvent const CurrentEvent = Events_.front();
        Events_.pop_front();
        if (CurrentEvent.Type == InputEvent::EventType::Key && KeyCaptureIndex_ >= 0 && Modal_ == Modal::Settings)
        {
            CaptureKeyBinding(CurrentEvent);
            continue;
        }
        if (Blocked)
        {
            continue;
        }
        if (CurrentEvent.Type == InputEvent::EventType::Key)
        {
            HandleKey(CurrentEvent);
        }
        else if (CurrentEvent.Cp >= 32 && CurrentEvent.Cp != 127)
        {
            Session* CurrentSession = InputSession();
            if (CurrentSession != nullptr && CurrentSession->IsRunning())
            {
                if (CurrentSession->InputSelected == true && CurrentSession->HasSelection == true)
                {
                    DeleteSelectedInput(*CurrentSession);
                }
                CurrentSession->InputSelected = false;
                CurrentSession->FindOpen = false;
                CurrentSession->ScrollOffset = 0;
                CurrentSession->SendText(Util::Utf8(CurrentEvent.Cp), Now_, StatusCallback());
            }
        }
    }
}

void App::CopySelection(Session& CurrentSession)
{
    if (CurrentSession.Term == nullptr || CurrentSession.HasSelection == false)
    {
        return;
    }
    int RowA = CurrentSession.Term->AbsRow(CurrentSession.SelectionStartRow), RowB = CurrentSession.Term->AbsRow(CurrentSession.SelectionEndRow);
    int ColA = CurrentSession.SelectionStartCol, ColB = CurrentSession.SelectionEndCol;
    if (RowA > RowB || (RowA == RowB && ColA > ColB))
    {
        std::swap(RowA, RowB);
        std::swap(ColA, ColB);
    }
    std::string const Text = CurrentSession.Term->GetText(RowA, ColA, RowB, ColB);
    if (Text.empty() == false)
    {
        glfwSetClipboardString(Window_, Text.c_str());
    }
    CurrentSession.HasSelection = false;
}

void App::Paste(Session& CurrentSession)
{
    if (CurrentSession.IsRunning() == false)
    {
        return;
    }

    const char* const Clipboard = glfwGetClipboardString(Window_);
    const bool HasText = Clipboard != nullptr && *Clipboard != '\0';
    if (HasText == false)
    {
        if (Platform::ClipboardHasImage() == false)
        {
            return;
        }
        // Claude Code reads the OS clipboard itself when it sees a raw Alt+V keystroke (its image-paste
        // trigger on Windows) rather than receiving image bytes over the pty, so deliver that same byte
        // sequence here instead of the clipboard text path below.
        CurrentSession.SendText("\x1bv", Now_, StatusCallback());
        return;
    }

    std::string Text;
    for (const char* Character = Clipboard; *Character != '\0'; Character++)
    {
        if (*Character == '\x1b')
        {
            continue;  // never let pasted text inject escape sequences
        }
        if (*Character == '\r')
        {
            Text += '\r';
            if (Character[1] == '\n')
            {
                Character++;
            }
            continue;
        }
        if (*Character == '\n')
        {
            Text += '\r';
            continue;
        }
        Text += *Character;
    }
    Session::StatusFn Callback = StatusCallback();
    double Now = Now_;
    CurrentSession.Typing(Now, Callback,
                   [&](Terminal& CurrentTerminal)
                   {
                       CurrentTerminal.StartPaste();
                       CurrentSession.SendText(Text, Now, Callback);
                       CurrentTerminal.EndPaste();
                   });
}

void App::SendPrompt(Session& CurrentSession, const std::string& Text)
{
    const Session::StatusFn Callback = StatusCallback();
    const double Now = Now_;
    CurrentSession.ScrollOffset = 0;
    CurrentSession.Typing(Now, Callback,
                          [&](Terminal& CurrentTerminal)
                          {
                              CurrentTerminal.StartPaste();
                              CurrentSession.SendText(Text, Now, Callback);
                              CurrentTerminal.EndPaste();
                          });
    CurrentSession.SendText("\r", Now, Callback);
}

void App::HandleKey(const InputEvent& CurrentEvent)
{
    const bool Ctrl = (CurrentEvent.Mods & GLFW_MOD_CONTROL) != 0, Alt = (CurrentEvent.Mods & GLFW_MOD_ALT) != 0, Shift = (CurrentEvent.Mods & GLFW_MOD_SHIFT) != 0;
    const int CurrentKey = CurrentEvent.Key;
    Session* CurrentSession = InputSession();

    const KeyBindings& Keys = State_.AppSettings.Keys;

    // ---- app shortcuts (never reach Claude) ----
    if (Keys.Matches(KeyAction::OpenSearch, CurrentKey, CurrentEvent.Mods) == true)
    {
        OpenSearch();
        return;
    }
    if (Alt && Ctrl == false && Shift == false && CurrentKey >= GLFW_KEY_1 && CurrentKey <= GLFW_KEY_9)
    {
        std::vector<std::string> Visible = VisibleIds();
        size_t const Index = (size_t)(CurrentKey - GLFW_KEY_1);
        if (Index < Visible.size())
        {
            FocusSession(Visible[Index]);
        }
        return;
    }
    if (Keys.Matches(KeyAction::FindInTerminal, CurrentKey, CurrentEvent.Mods) == true)
    {
        if (CurrentSession != nullptr && CurrentSession->Term != nullptr)
        {
            CurrentSession->FindOpen = true;
            CurrentSession->FindFocus = true;
        }
        return;
    }
    if (Keys.Matches(KeyAction::SelectAll, CurrentKey, CurrentEvent.Mods) == true)
    {
        const bool TerminalHasFocus = Focused_ == true && Modal_ == Modal::None && ImGui::GetIO().WantTextInput == false;
        if (TerminalHasFocus == false)
        {
            return;
        }
        if (CurrentSession != nullptr && CurrentSession->Term != nullptr)
        {
            const Terminal& Term = *CurrentSession->Term;
            CurrentSession->SelectionStartRow = Term.StableRow(0);
            CurrentSession->SelectionStartCol = 0;
            CurrentSession->SelectionEndRow = Term.StableRow(Term.ScrollbackSize() + Term.Rows() - 1);
            CurrentSession->SelectionEndCol = Term.Cols() - 1;
            CurrentSession->Selecting = false;
            CurrentSession->HasSelection = true;
            CurrentSession->InputSelected = false;
        }
        return;
    }
    if (Keys.Matches(KeyAction::SelectInput, CurrentKey, CurrentEvent.Mods) == true && CurrentSession != nullptr && CurrentSession->Term != nullptr)
    {
        Terminal& Term = *CurrentSession->Term;
        int FirstRow = 0;
        int FirstCol = 0;
        int LastRow = 0;
        int LastCol = 0;
        if (Term.AltScreen() == false && Term.FindInputText(FirstRow, FirstCol, LastRow, LastCol) == true)
        {
            CurrentSession->SelectionStartRow = Term.StableRow(FirstRow);
            CurrentSession->SelectionStartCol = FirstCol;
            CurrentSession->SelectionEndRow = Term.StableRow(LastRow);
            CurrentSession->SelectionEndCol = LastCol;
            CurrentSession->Selecting = false;
            CurrentSession->HasSelection = true;
            CurrentSession->InputSelected = true;
            CurrentSession->ScrollOffset = 0;
            return;
        }
    }
    if (Keys.Matches(KeyAction::FontIncrease, CurrentKey, CurrentEvent.Mods) == true)
    {
        return ChangeFontSize(+1);
    }
    if (Keys.Matches(KeyAction::FontDecrease, CurrentKey, CurrentEvent.Mods) == true)
    {
        return ChangeFontSize(-1);
    }
    if (Keys.Matches(KeyAction::FontReset, CurrentKey, CurrentEvent.Mods) == true)
    {
        return ChangeFontSize(0);
    }
    if (Keys.Matches(KeyAction::NewSession, CurrentKey, CurrentEvent.Mods) == true)
    {
        if (State_.Projects.empty() == true)
        {
            OpenModal(Modal::AddProject);
            return;
        }
        ModalTarget_ = CurrentSession != nullptr ? CurrentSession->Rec.ProjectId : State_.Projects.front().Id;
        OpenModal(Modal::NewSession);
        return;
    }
    if (Keys.Matches(KeyAction::ToggleMaximize, CurrentKey, CurrentEvent.Mods) == true)
    {
        ToggleMaximize(State_.Ui.ActiveId);
        return;
    }
    if (Keys.Matches(KeyAction::HideSession, CurrentKey, CurrentEvent.Mods) == true)
    {
        HideSession(State_.Ui.ActiveId);
        return;
    }
    if (Keys.Matches(KeyAction::Copy, CurrentKey, CurrentEvent.Mods) == true)
    {
        if (CurrentSession != nullptr)
        {
            CopySelection(*CurrentSession);
        }
        return;
    }
    if (Keys.Matches(KeyAction::Paste, CurrentKey, CurrentEvent.Mods) == true)
    {
        if (CurrentSession != nullptr)
        {
            Paste(*CurrentSession);
        }
        return;
    }

    if (CurrentSession == nullptr || CurrentSession->IsRunning() == false)
    {
        return;
    }
    Session::StatusFn Callback = StatusCallback();
    const std::function<void(const std::string&)> Send = [&](const std::string& Bytes)
    {
        CurrentSession->FindOpen = false;
        CurrentSession->ScrollOffset = 0;
        CurrentSession->SendText(Bytes, Now_, Callback);
    };

    if (Ctrl && Shift == false && Alt == false && CurrentKey == GLFW_KEY_C && CurrentSession->HasSelection)
    {
        return CopySelection(*CurrentSession);  // otherwise: interrupt
    }
    if (Ctrl && Alt == false && CurrentKey == GLFW_KEY_V)
    {
        return Paste(*CurrentSession);
    }
    if (Ctrl && Alt)
    {
        return;  // AltGr combinations arrive as characters
    }

    // Local scrollback navigation
    if (Shift && Ctrl == false && Alt == false && CurrentSession->Term && (CurrentKey == GLFW_KEY_PAGE_UP || CurrentKey == GLFW_KEY_PAGE_DOWN))
    {
        int const Page = std::max(1, CurrentSession->Term->Rows() - 1);
        CurrentSession->ScrollOffset = std::clamp(CurrentSession->ScrollOffset + (CurrentKey == GLFW_KEY_PAGE_UP ? Page : -Page), 0,
                                           CurrentSession->Term->ScrollbackSize());
        return;
    }

    if (CurrentSession->InputSelected == true)
    {
        const bool IsDelete = (CurrentKey == GLFW_KEY_BACKSPACE || CurrentKey == GLFW_KEY_DELETE) && Ctrl == false && Alt == false;
        if (CurrentSession->HasSelection == true && IsDelete == true)
        {
            DeleteSelectedInput(*CurrentSession);
            return;
        }
        CurrentSession->InputSelected = false;
        CurrentSession->HasSelection = false;
    }

    VTermModifier VtermMod =(VTermModifier)((Shift ? VTERM_MOD_SHIFT : 0) | (Alt ? VTERM_MOD_ALT : 0) | (Ctrl ? VTERM_MOD_CTRL : 0));
    VTermKey VtermKey = VTERM_KEY_NONE;
    switch (CurrentKey)
    {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            if (Shift && Ctrl == false && Alt == false)
            {
                return Send("\x1b\r");  // newline in the prompt
            }
            VtermKey = VTERM_KEY_ENTER;
            break;
        case GLFW_KEY_TAB: VtermKey = VTERM_KEY_TAB; break;
        case GLFW_KEY_BACKSPACE: VtermKey = VTERM_KEY_BACKSPACE; break;
        case GLFW_KEY_ESCAPE: VtermKey = VTERM_KEY_ESCAPE; break;
        case GLFW_KEY_UP: VtermKey = VTERM_KEY_UP; break;
        case GLFW_KEY_DOWN: VtermKey = VTERM_KEY_DOWN; break;
        case GLFW_KEY_LEFT: VtermKey = VTERM_KEY_LEFT; break;
        case GLFW_KEY_RIGHT: VtermKey = VTERM_KEY_RIGHT; break;
        case GLFW_KEY_INSERT: VtermKey = VTERM_KEY_INS; break;
        case GLFW_KEY_DELETE: VtermKey = VTERM_KEY_DEL; break;
        case GLFW_KEY_HOME: VtermKey = VTERM_KEY_HOME; break;
        case GLFW_KEY_END: VtermKey = VTERM_KEY_END; break;
        case GLFW_KEY_PAGE_UP: VtermKey = VTERM_KEY_PAGEUP; break;
        case GLFW_KEY_PAGE_DOWN: VtermKey = VTERM_KEY_PAGEDOWN; break;
        default:
            if (CurrentKey >= GLFW_KEY_F1 && CurrentKey <= GLFW_KEY_F12)
            {
                VtermKey = (VTermKey)(VTERM_KEY_FUNCTION_0 + (CurrentKey - GLFW_KEY_F1 + 1));
            }
            break;
    }
    if (VtermKey != VTERM_KEY_NONE)
    {
        CurrentSession->FindOpen = false;
        CurrentSession->Typing(Now_, Callback, [&](Terminal& CurrentTerminal) { CurrentTerminal.SendKey(VtermKey, VtermMod); });
        return;
    }

    // Ctrl/Alt + letter: send control bytes ourselves (libvterm would use CSI-u for some of them).
    if ((Ctrl || Alt) && CurrentKey >= GLFW_KEY_A && CurrentKey <= GLFW_KEY_Z)
    {
        char const Letter = (char)('a' + (CurrentKey - GLFW_KEY_A));
        std::string Out;
        if (Alt)
        {
            Out += '\x1b';
        }
        if (Ctrl)
        {
            Out += (char)(Letter - 'a' + 1);
        }
        else
        {
            Out += Shift ? (char)std::toupper(Letter) : Letter;
        }
        return Send(Out);
    }
    if (Ctrl && Alt == false)
    {
        switch (CurrentKey)
        {
            case GLFW_KEY_SPACE: return Send(std::string(1, '\0'));
            case GLFW_KEY_LEFT_BRACKET: return Send("\x1b");
            case GLFW_KEY_BACKSLASH: return Send("\x1c");
            case GLFW_KEY_RIGHT_BRACKET: return Send("\x1d");
            case GLFW_KEY_SLASH: return Send("\x1f");
            default: break;
        }
    }
    if (Alt && Ctrl == false && CurrentKey >= 32 && CurrentKey <= 96)
    {  // Alt+digit / punctuation
        return Send(std::string("\x1b") + (char)CurrentKey);
    }
}
