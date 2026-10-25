#pragma once
#include <string>

#include <GLFW/glfw3.h>

enum class KeyAction
{
    OpenSearch,
    FindInTerminal,
    SelectAll,
    SelectInput,
    NewSession,
    ToggleMaximize,
    HideSession,
    Copy,
    Paste,
    FontIncrease,
    FontDecrease,
    FontReset,
    Count
};

struct KeyChord
{
    int Key = 0;  // GLFW_KEY_*, 0 = unbound
    int Mods = 0; // GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SHIFT only
};

// The app-level shortcuts that never reach Claude, and the chord each one is currently bound to.
class KeyBindings
{
public:
    static constexpr int ACTION_COUNT = static_cast<int>(KeyAction::Count);
    static constexpr int MOD_MASK = GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SHIFT;

    KeyBindings() { ResetAll(); }

    static KeyAction ActionAt(int Index) { return static_cast<KeyAction>(Index); }

    static const char* ActionName(KeyAction Action)
    {
        switch (Action)
        {
            case KeyAction::OpenSearch: return "Search";
            case KeyAction::FindInTerminal: return "Find in terminal";
            case KeyAction::SelectAll: return "Select all text";
            case KeyAction::SelectInput: return "Select input text";
            case KeyAction::NewSession: return "New session";
            case KeyAction::ToggleMaximize: return "Maximize / restore";
            case KeyAction::HideSession: return "Hide pane";
            case KeyAction::Copy: return "Copy";
            case KeyAction::Paste: return "Paste";
            case KeyAction::FontIncrease: return "Font larger";
            case KeyAction::FontDecrease: return "Font smaller";
            case KeyAction::FontReset: return "Font reset";
            default: return "";
        }
    }

    static const char* ActionId(KeyAction Action)
    {
        switch (Action)
        {
            case KeyAction::OpenSearch: return "openSearch";
            case KeyAction::FindInTerminal: return "findInTerminal";
            case KeyAction::SelectAll: return "selectAll";
            case KeyAction::SelectInput: return "selectInput";
            case KeyAction::NewSession: return "newSession";
            case KeyAction::ToggleMaximize: return "toggleMaximize";
            case KeyAction::HideSession: return "hideSession";
            case KeyAction::Copy: return "copy";
            case KeyAction::Paste: return "paste";
            case KeyAction::FontIncrease: return "fontIncrease";
            case KeyAction::FontDecrease: return "fontDecrease";
            case KeyAction::FontReset: return "fontReset";
            default: return "";
        }
    }

    static KeyChord DefaultChord(KeyAction Action)
    {
        constexpr int CTRL_SHIFT = GLFW_MOD_CONTROL | GLFW_MOD_SHIFT;
        switch (Action)
        {
            case KeyAction::OpenSearch: return {GLFW_KEY_O, GLFW_MOD_ALT | GLFW_MOD_SHIFT};
            case KeyAction::FindInTerminal: return {GLFW_KEY_F, CTRL_SHIFT};
            case KeyAction::SelectAll: return {GLFW_KEY_A, CTRL_SHIFT};
            case KeyAction::SelectInput: return {GLFW_KEY_A, GLFW_MOD_CONTROL};
            case KeyAction::NewSession: return {GLFW_KEY_N, CTRL_SHIFT};
            case KeyAction::ToggleMaximize: return {GLFW_KEY_M, CTRL_SHIFT};
            case KeyAction::HideSession: return {GLFW_KEY_W, CTRL_SHIFT};
            case KeyAction::Copy: return {GLFW_KEY_C, CTRL_SHIFT};
            case KeyAction::Paste: return {GLFW_KEY_V, CTRL_SHIFT};
            case KeyAction::FontIncrease: return {GLFW_KEY_EQUAL, GLFW_MOD_CONTROL};
            case KeyAction::FontDecrease: return {GLFW_KEY_MINUS, GLFW_MOD_CONTROL};
            case KeyAction::FontReset: return {GLFW_KEY_0, GLFW_MOD_CONTROL};
            default: return {};
        }
    }

    const KeyChord& Get(KeyAction Action) const { return Chords_[static_cast<int>(Action)]; }

    void Set(KeyAction Action, const KeyChord& Chord) { Chords_[static_cast<int>(Action)] = Chord; }

    void Reset(KeyAction Action) { Set(Action, DefaultChord(Action)); }

    void ResetAll()
    {
        for (int Index = 0; Index < ACTION_COUNT; Index++)
        {
            Reset(ActionAt(Index));
        }
    }

    bool Matches(KeyAction Action, int Key, int Mods) const
    {
        const KeyChord& Chord = Get(Action);
        if (Chord.Key == 0 || Normalize(Key) != Normalize(Chord.Key))
        {
            return false;
        }
        int EventMods = Mods & MOD_MASK;
        if (Action == KeyAction::FontIncrease && (Chord.Mods & GLFW_MOD_SHIFT) == 0)
        {
            EventMods &= ~GLFW_MOD_SHIFT;
        }
        return EventMods == Chord.Mods;
    }

    // True if another action already uses this chord.
    bool FindConflict(KeyAction Except, const KeyChord& Chord, KeyAction& Conflict) const
    {
        if (Chord.Key == 0)
        {
            return false;
        }
        for (int Index = 0; Index < ACTION_COUNT; Index++)
        {
            const KeyAction Other = ActionAt(Index);
            if (Other == Except || Normalize(Get(Other).Key) != Normalize(Chord.Key) || Get(Other).Mods != Chord.Mods)
            {
                continue;
            }
            Conflict = Other;
            return true;
        }
        return false;
    }

    // App shortcuts swallow the key, so they need Ctrl or Alt (or be a function key) to leave normal typing alone.
    static bool IsAllowed(const KeyChord& Chord)
    {
        const bool IsFunctionKey = Chord.Key >= GLFW_KEY_F1 && Chord.Key <= GLFW_KEY_F12;
        return IsFunctionKey == true || (Chord.Mods & (GLFW_MOD_CONTROL | GLFW_MOD_ALT)) != 0;
    }

    static bool IsModifierKey(int Key) { return Key >= GLFW_KEY_LEFT_SHIFT && Key <= GLFW_KEY_RIGHT_SUPER; }

    static std::string Describe(const KeyChord& Chord)
    {
        if (Chord.Key == 0)
        {
            return "Unbound";
        }
        std::string Text;
        if ((Chord.Mods & GLFW_MOD_CONTROL) != 0)
        {
            Text += "Ctrl+";
        }
        if ((Chord.Mods & GLFW_MOD_ALT) != 0)
        {
            Text += "Alt+";
        }
        if ((Chord.Mods & GLFW_MOD_SHIFT) != 0)
        {
            Text += "Shift+";
        }
        return Text + KeyName(Chord.Key);
    }

private:
    static int Normalize(int Key)
    {
        switch (Key)
        {
            case GLFW_KEY_KP_ADD: return GLFW_KEY_EQUAL;
            case GLFW_KEY_KP_SUBTRACT: return GLFW_KEY_MINUS;
            case GLFW_KEY_KP_0: return GLFW_KEY_0;
            default: return Key;
        }
    }

    static std::string KeyName(int Key)
    {
        if ((Key >= GLFW_KEY_A && Key <= GLFW_KEY_Z) || (Key >= GLFW_KEY_0 && Key <= GLFW_KEY_9))
        {
            return std::string(1, static_cast<char>(Key));
        }
        if (Key >= GLFW_KEY_F1 && Key <= GLFW_KEY_F12)
        {
            return "F" + std::to_string(Key - GLFW_KEY_F1 + 1);
        }
        switch (Key)
        {
            case GLFW_KEY_EQUAL: return "=";
            case GLFW_KEY_MINUS: return "-";
            case GLFW_KEY_SPACE: return "Space";
            case GLFW_KEY_ENTER: return "Enter";
            case GLFW_KEY_TAB: return "Tab";
            case GLFW_KEY_BACKSPACE: return "Backspace";
            case GLFW_KEY_DELETE: return "Delete";
            case GLFW_KEY_INSERT: return "Insert";
            case GLFW_KEY_HOME: return "Home";
            case GLFW_KEY_END: return "End";
            case GLFW_KEY_PAGE_UP: return "PgUp";
            case GLFW_KEY_PAGE_DOWN: return "PgDn";
            case GLFW_KEY_UP: return "Up";
            case GLFW_KEY_DOWN: return "Down";
            case GLFW_KEY_LEFT: return "Left";
            case GLFW_KEY_RIGHT: return "Right";
            case GLFW_KEY_COMMA: return ",";
            case GLFW_KEY_PERIOD: return ".";
            case GLFW_KEY_SLASH: return "/";
            case GLFW_KEY_SEMICOLON: return ";";
            case GLFW_KEY_APOSTROPHE: return "'";
            case GLFW_KEY_BACKSLASH: return "\\";
            case GLFW_KEY_LEFT_BRACKET: return "[";
            case GLFW_KEY_RIGHT_BRACKET: return "]";
            case GLFW_KEY_GRAVE_ACCENT: return "`";
            default: return "Key " + std::to_string(Key);
        }
    }

    KeyChord Chords_[ACTION_COUNT];
};
