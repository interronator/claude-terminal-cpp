#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

#include "git.h"
#include "build_scripts.h"
#include "image_cache.h"
#include "platform.h"
#include "style.h"
#include "ui_helpers.h"
#include "util.h"

namespace fs = std::filesystem;


// ---------------------------------------------------------------------------------------------
// construction, fonts
// ---------------------------------------------------------------------------------------------

App::App(GLFWwindow* Window, Store& Store, float UiScale)
    : Window_(Window), Store_(Store), State_(Store.AppState), UiScale_(UiScale), Images_(std::make_unique<ImageCache>())
{
    for (const SessionRec& Rec : State_.Sessions)
    {
        Sessions_.push_back(std::make_unique<Session>(Rec));
    }
    UiState& Ui = State_.Ui;
    Ui.OpenIds.erase(std::remove_if(Ui.OpenIds.begin(), Ui.OpenIds.end(),
                                    [&](const std::string& Id) { return FindSession(Id) == nullptr; }),
                     Ui.OpenIds.end());
    if (FindSession(Ui.ActiveId) == nullptr)
    {
        Ui.ActiveId = Ui.OpenIds.empty() ? "" : Ui.OpenIds.front();
    }
    if (Ui.Mode != "tile" && Ui.Mode != "tabs")
    {
        Ui.Mode = "tile";
    }
    Session::Wake = [] { glfwPostEmptyEvent(); };
}

App::~App()
{
}

bool App::RebuildFontsIfNeeded()
{
    if (FontsDirty_ == false)
    {
        return false;
    }
    FontsDirty_ = false;
    ImGuiIO& Io = ImGui::GetIO();
    Io.Fonts->Clear();

    const std::string Windir = UiHelpers::EnvOr("WINDIR").empty() ? "C:\\Windows" : UiHelpers::EnvOr("WINDIR");
    const std::string UiPath = UiHelpers::FirstExisting({UiHelpers::EnvOr("CT_FONT_UI"), Windir + "\\Fonts\\segoeui.ttf",
                                              "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                              "/System/Library/Fonts/Helvetica.ttc"});
    const std::string MonoPath = UiHelpers::FirstExisting({UiHelpers::EnvOr("CT_FONT_MONO"), Windir + "\\Fonts\\CascadiaMono.ttf",
                                                Windir + "\\Fonts\\consola.ttf",
                                                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                                "/System/Library/Fonts/Menlo.ttc"});
    const std::string BoldPath = UiHelpers::FirstExisting({UiHelpers::EnvOr("CT_FONT_MONO_BOLD"), Windir + "\\Fonts\\consolab.ttf",
                                                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"});
    const std::string SymbolPath = UiHelpers::FirstExisting({Windir + "\\Fonts\\seguisym.ttf"});

    // Glyphs Claude Code uses: Latin, Greek/Cyrillic, punctuation, arrows, box drawing, blocks,
    // geometric shapes, dingbats (spinner stars, prompt chevron), braille, powerline.
    ImFontGlyphRangesBuilder RangesBuilder;
    RangesBuilder.AddRanges(Io.Fonts->GetGlyphRangesDefault());
    static const ImWchar EXTRA_RANGES[] = {0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x04FF, 0x2000, 0x2BFF, 0xE0A0, 0xE0D7, 0};
    RangesBuilder.AddRanges(EXTRA_RANGES);
    MonoRanges_.clear();
    RangesBuilder.BuildRanges(&MonoRanges_);

    const float UiPx = std::round(15.0f * UiScale_);
    MonoFontPx_ = std::round((float)State_.AppSettings.FontSize * UiScale_);

    ImFontConfig UiConfig;
    UiConfig.OversampleH = 2;
    UiFont_ = UiPath.empty() ? Io.Fonts->AddFontDefault() : Io.Fonts->AddFontFromFileTTF(UiPath.c_str(), UiPx, &UiConfig);

    ImFontConfig MonoConfig;
    MonoConfig.OversampleH = 1;
    MonoConfig.OversampleV = 1;
    MonoConfig.PixelSnapH = true;
    if (MonoPath.empty())
    {
        MonoFont_ = Io.Fonts->AddFontDefault();
    }
    else
    {
        MonoFont_ = Io.Fonts->AddFontFromFileTTF(MonoPath.c_str(), MonoFontPx_, &MonoConfig, MonoRanges_.Data);
        if (SymbolPath.empty() == false)
        {
            ImFontConfig MergeConfig = MonoConfig;
            MergeConfig.MergeMode = true;
            static const ImWchar SYMBOL_RANGES[] = {0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF,
                                                    0x2600, 0x27BF, 0x2B00, 0x2BFF, 0};
            Io.Fonts->AddFontFromFileTTF(SymbolPath.c_str(), MonoFontPx_, &MergeConfig, SYMBOL_RANGES);
        }
    }
    MonoBoldFont_ = nullptr;
    if (BoldPath.empty() == false)
    {
        static const ImWchar BOLD_RANGES[] = {0x20, 0xFF, 0};
        MonoBoldFont_ = Io.Fonts->AddFontFromFileTTF(BoldPath.c_str(), MonoFontPx_, &MonoConfig, BOLD_RANGES);
    }
    Io.Fonts->Build();

    ImVec2 const GlyphSize = MonoFont_->CalcTextSizeA(MonoFont_->FontSize, FLT_MAX, 0.0f, "M");
    CellSize_.x = std::max(1.0f, std::round(GlyphSize.x));
    CellSize_.y = std::ceil(MonoFont_->FontSize * 1.18f);
    return true;
}

void App::SyncTheme()
{
    const ThemeColors& Theme = State_.AppSettings.Theme;
    const std::function<ImU32(const ThemeColor&)> ToU32 = [](const ThemeColor& Color) { return ImGui::ColorConvertFloat4ToU32(Style::ToImVec4(Color)); };
    AccentColor_ = ToU32(Theme.Accent);
    BusyColor_ = ToU32(Theme.StatusBusy);
    WaitingColor_ = ToU32(Theme.StatusWaiting);
    DoneColor_ = ToU32(Theme.StatusDone);
    IdleColor_ = ToU32(Theme.StatusIdle);
    UsageWarningColor_ = ToU32(Theme.UsageWarning);
    UsageFullColor_ = ToU32(Theme.UsageFull);
    SurfaceColorU32_ = ToU32(Theme.Surface);
    PaneBgColorU32_ = ToU32(Theme.PaneBg);
    PaneBorderColorU32_ = ToU32(Theme.PaneBorder);
    TerminalBgColorU32_ = ToU32(Theme.TerminalBg);
    ChipBgColorU32_ = ImGui::ColorConvertFloat4ToU32(Style::ShadeColor(Theme.Surface, 0.06f));
    TextColorU32_ = ToU32(Theme.Text);
    TextDisabledColorU32_ = ToU32(Theme.TextDisabled);
    SelectionColorU32_ = ToU32(Theme.Selection);
    CursorFillColorU32_ = ToU32(Theme.Cursor);
    CursorOutlineColorU32_ = ImGui::ColorConvertFloat4ToU32(
        ImVec4(Theme.Cursor.Red, Theme.Cursor.Green, Theme.Cursor.Blue, std::clamp(Theme.Cursor.Alpha + 0.12f, 0.0f, 1.0f)));

    BackgroundColorV4_ = Style::ToImVec4(Theme.Background);
    SurfaceColorV4_ = Style::ToImVec4(Theme.Surface);
    PaneBgColorV4_ = Style::ToImVec4(Theme.PaneBg);
    PaneBorderColorV4_ = Style::ToImVec4(Theme.PaneBorder);
    AccentColorV4_ = Style::ToImVec4(Theme.Accent);
    AccentHoverColorV4_ = Style::ShadeColor(Theme.Accent, 0.05f);
    AccentActiveColorV4_ = Style::ShadeColor(Theme.Accent, -0.07f);
    WaitingColorV4_ = Style::ToImVec4(Theme.StatusWaiting);
    TextColorV4_ = Style::ToImVec4(Theme.Text);
    TextDisabledColorV4_ = Style::ToImVec4(Theme.TextDisabled);
}

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

void App::OpenSearch()
{
    if (Modal_ != Modal::None)
    {
        return;
    }
    SearchBuffer_[0] = 0;
    SearchLastQuery_ = "\x01";
    SearchSelected_ = 0;
    SearchFocus_ = true;
    SearchResults_.clear();

    const Session* const Active = ActiveSession();
    std::string ProjectId = Active != nullptr ? Active->Rec.ProjectId : State_.Ui.TabFilterProjectId;
    if (ProjectId.empty() == true && State_.Projects.empty() == false)
    {
        ProjectId = State_.Projects.front().Id;
    }
    SearchRootProjectId_ = ProjectId;
    const Project* const Root = FindProject(ProjectId);
    if (Active != nullptr && Active->Rec.Cwd.empty() == false)
    {
        FileIndex_.Start(Active->Rec.Cwd);
    }
    else if (Root != nullptr)
    {
        FileIndex_.Start(Root->Path);
    }
    OpenModal(Modal::Search);
}

void App::RefreshSearchResults()
{
    SearchResults_.clear();
    const std::string Query = FileIndex::Lower(Util::Trim(SearchBuffer_));

    for (const Project& Candidate : State_.Projects)
    {
        if (Query.empty() == false && FileIndex::Lower(Candidate.Name).find(Query) == std::string::npos &&
            FileIndex::Lower(Candidate.Path).find(Query) == std::string::npos)
        {
            continue;
        }
        SearchResults_.push_back({SearchResult::Kind::Project, Candidate.Name, Candidate.Path, Candidate.Id});
    }

    size_t SessionCount = 0;
    for (const std::unique_ptr<Session>& Candidate : Sessions_)
    {
        const Project* const Owner = FindProject(Candidate->Rec.ProjectId);
        const std::string OwnerName = Owner != nullptr ? Owner->Name : std::string();
        if (Query.empty() == false && FileIndex::Lower(Candidate->Rec.Name).find(Query) == std::string::npos &&
            FileIndex::Lower(OwnerName).find(Query) == std::string::npos)
        {
            continue;
        }
        SearchResults_.push_back({SearchResult::Kind::Session, Candidate->Rec.Name, OwnerName, Candidate->Rec.Id});
        SessionCount++;
        if (SessionCount >= 20)
        {
            break;
        }
    }

    if (Query.empty() == true)
    {
        return;
    }
    std::vector<std::string> FilePaths;
    FileIndex_.Search(Query, 50, FilePaths);
    for (const std::string& FilePath : FilePaths)
    {
        const size_t NameStart = FilePath.find_last_of('/') == std::string::npos ? 0 : FilePath.find_last_of('/') + 1;
        const std::string Directory = NameStart == 0 ? std::string() : FilePath.substr(0, NameStart - 1);
        SearchResults_.push_back({SearchResult::Kind::File, FilePath.substr(NameStart), Directory, FilePath});
    }
}

void App::ActivateSearchResult(const SearchResult& Result)
{
    ImGui::CloseCurrentPopup();
    Modal_ = Modal::None;
    switch (Result.Type)
    {
        case SearchResult::Kind::Project:
        {
            State_.Ui.Collapsed[Result.Target] = false;
            std::string TargetSessionId;
            for (const std::unique_ptr<Session>& Candidate : Sessions_)
            {
                if (Candidate->Rec.ProjectId != Result.Target)
                {
                    continue;
                }
                if (TargetSessionId.empty() == true)
                {
                    TargetSessionId = Candidate->Rec.Id;
                }
                if (std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), Candidate->Rec.Id) != State_.Ui.OpenIds.end())
                {
                    TargetSessionId = Candidate->Rec.Id;
                    break;
                }
            }
            MarkDirty();
            if (TargetSessionId.empty() == false)
            {
                OpenSession(TargetSessionId);
                return;
            }
            ModalTarget_ = Result.Target;
            OpenModal(Modal::NewSession);
            return;
        }
        case SearchResult::Kind::Session:
        {
            OpenSession(Result.Target);
            return;
        }
        case SearchResult::Kind::File:
        {
            Session* const Active = ActiveSession();
            if (Active == nullptr || Active->IsRunning() == false)
            {
                ShowToast("Open a running session first to add a file to its context.", true);
                return;
            }
            const bool NeedsQuotes = Result.Target.find(' ') != std::string::npos;
            const std::string Reference = "@" + (NeedsQuotes ? "\"" + Result.Target + "\"" : Result.Target) + " ";
            Active->ScrollOffset = 0;
            Active->SendText(Reference, Now_, StatusCallback());
            return;
        }
    }
}

void App::DrawSearch(bool Confirm)
{
    const float Scale = UiScale_;
    ImGui::TextDisabled("Search projects, sessions and files");
    ImGui::SetNextItemWidth(-1);
    if (SearchFocus_ == true || ImGui::IsWindowAppearing() == true)
    {
        ImGui::SetKeyboardFocusHere();
        SearchFocus_ = false;
    }
    ImGui::InputTextWithHint("##search", "Type a name or part of a path...", SearchBuffer_, sizeof SearchBuffer_);

    const bool QueryChanged = SearchLastQuery_ != SearchBuffer_;
    const size_t IndexSize = FileIndex_.Size();
    if (QueryChanged == true || IndexSize != SearchLastIndexSize_)
    {
        SearchLastQuery_ = SearchBuffer_;
        SearchLastIndexSize_ = IndexSize;
        RefreshSearchResults();
        if (QueryChanged == true)
        {
            SearchSelected_ = 0;
        }
    }

    const int ResultCount = static_cast<int>(SearchResults_.size());
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) == true && SearchSelected_ + 1 < ResultCount)
    {
        SearchSelected_++;
        SearchScrollToSelected_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) == true && SearchSelected_ > 0)
    {
        SearchSelected_--;
        SearchScrollToSelected_ = true;
    }
    SearchSelected_ = std::clamp(SearchSelected_, 0, std::max(0, ResultCount - 1));

    int ClickedIndex = -1;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, PaneBgColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Border, PaneBorderColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * Scale);
    ImGui::BeginChild("##searchResults", ImVec2(-1, 300.0f * Scale), ImGuiChildFlags_Borders);
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const float RowHeight = ImGui::GetFrameHeight();
    const float TagWidth = 64.0f * Scale;
    for (int Index = 0; Index < ResultCount; Index++)
    {
        const SearchResult& Entry = SearchResults_[static_cast<size_t>(Index)];
        ImGui::PushID(Index);
        const ImVec2 RowPos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", Index == SearchSelected_, ImGuiSelectableFlags_None, ImVec2(0, RowHeight)) == true)
        {
            ClickedIndex = Index;
        }
        if (SearchScrollToSelected_ == true && Index == SearchSelected_)
        {
            ImGui::SetScrollHereY();
            SearchScrollToSelected_ = false;
        }
        const char* const Tag = Entry.Type == SearchResult::Kind::Project ? "Project" : (Entry.Type == SearchResult::Kind::Session ? "Session" : "File");
        const float TextY = RowPos.y + (RowHeight - ImGui::GetFontSize()) * 0.5f;
        DrawList->AddText(ImVec2(RowPos.x + 8.0f * Scale, TextY), TextDisabledColorU32_, Tag);
        DrawList->AddText(ImVec2(RowPos.x + TagWidth + 8.0f * Scale, TextY), TextColorU32_, Entry.Label.c_str());
        const float LabelWidth = ImGui::CalcTextSize(Entry.Label.c_str()).x;
        DrawList->AddText(ImVec2(RowPos.x + TagWidth + 18.0f * Scale + LabelWidth, TextY), TextDisabledColorU32_, Entry.Detail.c_str());
        ImGui::PopID();
    }
    if (ResultCount == 0)
    {
        ImGui::TextDisabled(SearchBuffer_[0] == 0 ? "Nothing to show yet." : "No matches.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    std::string Footer = "Up/Down to move, Enter to open, Esc to close";
    if (FileIndex_.Scanning() == true)
    {
        Footer += "   -   indexing files (" + std::to_string(IndexSize) + ")...";
    }
    ImGui::TextDisabled("%s", Footer.c_str());
    ImGui::TextDisabled("Files are added to the active session as an @ reference.");

    if (ClickedIndex < 0 && Confirm == true && ResultCount > 0)
    {
        ClickedIndex = SearchSelected_;
    }
    if (ClickedIndex >= 0)
    {
        const SearchResult Picked = SearchResults_[static_cast<size_t>(ClickedIndex)];
        ActivateSearchResult(Picked);
    }
}

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

void App::ChangeFontSize(int Delta)
{
    int const NewSize = Delta == 0 ? 16 : std::clamp(State_.AppSettings.FontSize + Delta, 8, 36);
    if (NewSize == State_.AppSettings.FontSize)
    {
        return;
    }
    State_.AppSettings.FontSize = NewSize;
    FontsDirty_ = true;
    MarkDirty();
}

// ---------------------------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------------------------

Session* App::FindSession(const std::string& Id) const
{
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.Id == Id)
        {
            return CurrentSession.get();
        }
    }
    return nullptr;
}

Project* App::FindProject(const std::string& Id)
{
    for (Project& CurrentProject : State_.Projects)
    {
        if (CurrentProject.Id == Id)
        {
            return &CurrentProject;
        }
    }
    return nullptr;
}

Session* App::ActiveSession() const
{
    return FindSession(State_.Ui.ActiveId);
}

Session* App::InputSession() const
{
    Session* const Active = ActiveSession();
    if (Active == nullptr)
    {
        return nullptr;
    }
    const std::vector<std::string> Visible = VisibleIds();
    if (std::find(Visible.begin(), Visible.end(), Active->Rec.Id) == Visible.end())
    {
        return nullptr;
    }
    return Active;
}

void App::EnsureActiveVisible()
{
    const std::vector<std::string> Visible = VisibleIds();
    if (Visible.empty() == true || std::find(Visible.begin(), Visible.end(), State_.Ui.ActiveId) != Visible.end())
    {
        return;
    }
    State_.Ui.ActiveId = Visible.front();
    MarkDirty();
}

// Open sessions passing the Running/Requires-action filter only (project filter NOT applied) —
// this is what the toolbar's tab bar shows, so every project's group stays visible when one is selected.
std::vector<std::string> App::ActionFilteredIds() const
{
    std::vector<std::string> Result;
    for (const std::string& Id : State_.Ui.OpenIds)
    {
        Session* const SessionPtr = FindSession(Id);
        if (SessionPtr == nullptr)
        {
            continue;
        }
        // The active session and sessions not yet started always stay visible: a hidden pane is never
        // drawn, and drawing is what starts a session.
        const bool AlwaysShown = Id == State_.Ui.ActiveId || SessionPtr->Attempted == false;
        if (AlwaysShown == false && State_.Ui.ActionFilter == "running" && SessionPtr->IsRunning() == false)
        {
            continue;
        }
        if (AlwaysShown == false && State_.Ui.ActionFilter == "action" && SessionPtr->RequiresAction() == false)
        {
            continue;
        }
        Result.push_back(Id);
    }
    return Result;
}

// Same as ActionFilteredIds(), plus the project filter — this is what's actually rendered as panes/tiles.
std::vector<std::string> App::FilteredOpenIds() const
{
    std::vector<std::string> Result;
    for (const std::string& Id : ActionFilteredIds())
    {
        Session* const SessionPtr = FindSession(Id);
        if (SessionPtr == nullptr)
        {
            continue;
        }
        const bool InExtras = std::find(FilterExtraIds_.begin(), FilterExtraIds_.end(), Id) != FilterExtraIds_.end();
        if (State_.Ui.TabFilterProjectId.empty() == false && SessionPtr->Rec.ProjectId != State_.Ui.TabFilterProjectId && InExtras == false)
        {
            continue;
        }
        Result.push_back(Id);
    }
    return Result;
}

std::vector<std::string> App::VisibleIds() const
{
    std::vector<std::string> Open = FilteredOpenIds();
    if (State_.Ui.Mode == "tabs")
    {
        if (Open.empty())
        {
            return {};
        }
        bool const HasActive = std::find(Open.begin(), Open.end(), State_.Ui.ActiveId) != Open.end();
        return {HasActive ? State_.Ui.ActiveId : Open.front()};
    }
    if (MaximizedId_.empty() == false && std::find(Open.begin(), Open.end(), MaximizedId_) != Open.end())
    {
        return {MaximizedId_};
    }
    return Open;
}

void App::FocusSession(const std::string& Id)
{
    if (std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), Id) == State_.Ui.OpenIds.end())
    {
        return;
    }
    Session* const CurrentSession = FindSession(Id);
    if (CurrentSession == nullptr)
    {
        return;
    }
    ProjectScopeId_.clear();
    if (State_.Ui.ActiveId != Id)
    {
        State_.Ui.ActiveId = Id;
        MarkDirty();
    }
    CurrentSession->Unseen = false;
}

void App::OpenSession(const std::string& Id)
{
    Session* const CurrentSession = FindSession(Id);
    if (CurrentSession == nullptr)
    {
        return;
    }
    std::vector<std::string>& Open = State_.Ui.OpenIds;
    if (std::find(Open.begin(), Open.end(), Id) == Open.end())
    {
        Open.push_back(Id);
    }
    if (MaximizedId_.empty() == false && MaximizedId_ != Id)
    {
        MaximizedId_.clear();
    }
    const bool OutsideFilter = State_.Ui.TabFilterProjectId.empty() == false && State_.Ui.TabFilterProjectId != CurrentSession->Rec.ProjectId;
    if (OutsideFilter == true && std::find(FilterExtraIds_.begin(), FilterExtraIds_.end(), Id) == FilterExtraIds_.end())
    {
        FilterExtraIds_.push_back(Id);
    }
    FocusSession(Id);
    MarkDirty();
}

void App::HideSession(const std::string Id)
{
    std::vector<std::string>& Open = State_.Ui.OpenIds;
    Open.erase(std::remove(Open.begin(), Open.end(), Id), Open.end());
    FilterExtraIds_.erase(std::remove(FilterExtraIds_.begin(), FilterExtraIds_.end(), Id), FilterExtraIds_.end());
    if (State_.Ui.ActiveId == Id)
    {
        State_.Ui.ActiveId = Open.empty() ? "" : Open.back();
    }
    if (MaximizedId_ == Id)
    {
        MaximizedId_.clear();
    }
    if (Session* const CurrentSession = FindSession(State_.Ui.ActiveId))
    {
        CurrentSession->Unseen = false;
    }
    MarkDirty();
}

void App::ToggleMaximize(const std::string& Id)
{
    if (State_.Ui.Mode == "tabs" || Id.empty())
    {
        return;
    }
    MaximizedId_ = MaximizedId_ == Id ? "" : Id;
}

Session* App::CreateSession(const std::string& ProjectId, const std::string& Name, bool CurrentWorktree, bool ContinueLast, std::string& Err)
{
    Project* const CurrentProject = FindProject(ProjectId);
    if (CurrentProject == nullptr)
    {
        Err = "Project not found";
        return nullptr;
    }
    if (UiHelpers::FileExists(CurrentProject->Path) == false)
    {
        Err = "Folder no longer exists: " + CurrentProject->Path;
        return nullptr;
    }
    int Count = 0;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId == ProjectId)
        {
            Count++;
        }
    }
    std::string FinalName = Util::Trim(Name);
    if (FinalName.empty())
    {
        FinalName = "Session " + std::to_string(Count + 1);
    }

    SessionRec CurrentRec;
    CurrentRec.Id = Util::Uuid4();
    CurrentRec.ProjectId = ProjectId;
    CurrentRec.Name = FinalName;
    CurrentRec.Cwd = CurrentProject->Path;
    CurrentRec.ClaudeSessionId = Util::Uuid4();
    CurrentRec.CreatedAt = (long long)std::time(nullptr);
    if (ContinueLast == true && CurrentWorktree == false)
    {
        std::string const PreviousId = Util::LatestClaudeSessionId(CurrentRec.Cwd);
        if (PreviousId.empty() == true)
        {
            Err = "No earlier conversation found for this folder";
            return nullptr;
        }
        CurrentRec.ClaudeSessionId = PreviousId;
        CurrentRec.Started = true;
    }
    if (CurrentWorktree)
    {
        Git::Worktree WorktreeInfo;
        if (Git::CreateWorktree(CurrentProject->Path, FinalName, WorktreeInfo, Err) == false)
        {
            return nullptr;
        }
        CurrentRec.WorktreeInfo = Worktree{WorktreeInfo.RepoRoot, WorktreeInfo.Branch, WorktreeInfo.Path};
        CurrentRec.Cwd = WorktreeInfo.Cwd;
    }
    Sessions_.push_back(std::make_unique<Session>(CurrentRec));
    MarkDirty();
    return Sessions_.back().get();
}

void App::CloseSession(const std::string& Id, int WorktreeAction)
{
    Session* CurrentSession = FindSession(Id);
    if (CurrentSession == nullptr)
    {
        return;
    }
    CurrentSession->Kill(Now_, nullptr);
    ScreenshotFeed_.RemoveSession(Id);
    TranscriptScanners_.erase(Id);
    ScreenshotPollAt_.erase(Id);
    const Project* const Owner = FindProject(CurrentSession->Rec.ProjectId);
    State_.Archive.push_back({CurrentSession->Rec.Id, CurrentSession->Rec.Name, Owner != nullptr ? Owner->Name : std::string(), CurrentSession->Rec});
    std::string Warning;
    if (CurrentSession->Rec.WorktreeInfo && WorktreeAction > 0)
    {
        bool BranchKept = false;
        std::string Err;
        if (Git::RemoveWorktree(CurrentSession->Rec.WorktreeInfo->RepoRoot, CurrentSession->Rec.WorktreeInfo->Path, CurrentSession->Rec.WorktreeInfo->Branch,
                                WorktreeAction == 2, BranchKept, Err))
        {
            if (BranchKept)
            {
                Warning = "Worktree removed. Branch " + CurrentSession->Rec.WorktreeInfo->Branch + " was kept (unmerged commits).";
            }
        }
        else
        {
            Warning = "Could not remove the worktree (" + Err + "). It was left at " + CurrentSession->Rec.WorktreeInfo->Path;
        }
    }
    HideSession(Id);
    Sessions_.erase(std::remove_if(Sessions_.begin(), Sessions_.end(),
                                   [&](const std::unique_ptr<Session>& CurrentSession) { return CurrentSession->Rec.Id == Id; }),
                    Sessions_.end());
    MarkDirty();
    if (Warning.empty() == false)
    {
        ShowToast(Warning);
    }
}

void App::RequestDeleteAllArchived()
{
    if (State_.Ui.CloseWithoutConfirm == true)
    {
        State_.Archive.clear();
        MarkDirty();
        return;
    }
    ConfirmTitle_ = "Delete all archived sessions?";
    ConfirmText_ = "This permanently removes every entry from the Archive.";
    ConfirmYesAction_ = [this]
    {
        State_.Archive.clear();
        MarkDirty();
    };
    OpenModal(Modal::Confirm);
}

void App::RestoreArchivedSession(const size_t ArchiveIndex)
{
    if (ArchiveIndex >= State_.Archive.size())
    {
        return;
    }
    const SessionRec Rec = State_.Archive[ArchiveIndex].Rec;
    if (Rec.ClaudeSessionId.empty() == true || FindProject(Rec.ProjectId) == nullptr)
    {
        ShowToast("This session can't be restored (its project or history was not saved).", true);
        return;
    }
    if (FindSession(Rec.Id) != nullptr || UiHelpers::FileExists(Rec.Cwd) == false)
    {
        ShowToast("This session can't be restored (its folder no longer exists).", true);
        return;
    }
    State_.Archive.erase(State_.Archive.begin() + static_cast<std::ptrdiff_t>(ArchiveIndex));
    Sessions_.push_back(std::make_unique<Session>(Rec));
    OpenSession(Rec.Id);
}

void App::RequestCloseSession(const std::string& Id)
{
    const Session* const Target = FindSession(Id);
    if (Target == nullptr)
    {
        return;
    }
    if (State_.Ui.CloseWithoutConfirm == true)
    {
        PendingCloseId_ = Id;
        return;
    }
    ModalTarget_ = Id;
    if (Target->Rec.WorktreeInfo)
    {
        CloseChoice_ = 0;
        CloseDirty_ = Git::IsDirty(Target->Rec.WorktreeInfo->Path);
        OpenModal(Modal::CloseSession);
        return;
    }
    ConfirmTitle_ = "Close \"" + Target->Rec.Name + "\"?";
    ConfirmText_ = "This stops Claude in this session and moves it to the Archive. The conversation stays in Claude's own history.";
    ConfirmYesAction_ = [this, Id] { CloseSession(Id, 0); };
    OpenModal(Modal::Confirm);
}

void App::CloseUnopenedSessions(const std::string& ProjectId)
{
    std::vector<std::string> IdsToClose;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId != ProjectId)
        {
            continue;
        }
        const bool IsOpen = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), CurrentSession->Rec.Id) != State_.Ui.OpenIds.end();
        if (IsOpen == true)
        {
            continue;
        }
        IdsToClose.push_back(CurrentSession->Rec.Id);
    }
    for (const std::string& SessionId : IdsToClose)
    {
        CloseSession(SessionId, 0);
    }
}

void App::RequestCloseUnopenedSessions(const std::string& ProjectId)
{
    int UnopenedCount = 0;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId != ProjectId)
        {
            continue;
        }
        const bool IsOpen = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), CurrentSession->Rec.Id) != State_.Ui.OpenIds.end();
        if (IsOpen == false)
        {
            UnopenedCount++;
        }
    }
    if (UnopenedCount == 0)
    {
        ShowToast("No unopened sessions in this project.");
        return;
    }
    if (State_.Ui.CloseWithoutConfirm == true)
    {
        CloseUnopenedSessions(ProjectId);
        return;
    }
    ConfirmTitle_ = "Close " + std::to_string(UnopenedCount) + " unopened session(s)?";
    ConfirmText_ = "This stops these sessions and moves them to the Archive. Sessions shown in a pane stay open.\nGit worktrees on disk are left untouched.";
    ConfirmYesAction_ = [this, ProjectId] { CloseUnopenedSessions(ProjectId); };
    OpenModal(Modal::Confirm);
}

void App::RemoveProject(const std::string& Id)
{
    std::vector<std::string> IdsToClose;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Rec.ProjectId == Id)
        {
            IdsToClose.push_back(CurrentSession->Rec.Id);
        }
    }
    for (const std::string& SessionId : IdsToClose)
    {
        CloseSession(SessionId, 0);
    }
    State_.Projects.erase(
        std::remove_if(State_.Projects.begin(), State_.Projects.end(), [&](const Project& CurrentProject) { return CurrentProject.Id == Id; }),
        State_.Projects.end());
    MarkDirty();
}

bool App::AddProjectPath(const std::string& RawPath, std::string& Err, std::string* OutId)
{
    std::string Path = Util::Trim(RawPath);
    if (Path.size() >= 2 && Path.front() == '"' && Path.back() == '"')
    {
        Path = Path.substr(1, Path.size() - 2);
    }
    std::error_code ErrorCode;
    if (Path.empty() || fs::is_directory(Path, ErrorCode) == false)
    {
        Err = "That folder doesn't exist.";
        return false;
    }
    std::string Normalized = fs::path(Path).lexically_normal().make_preferred().string();
    while (Normalized.size() > 1 && (Normalized.back() == '/' || Normalized.back() == '\\'))
    {
        Normalized.pop_back();
    }
    for (const Project& CurrentProject : State_.Projects)
    {
        if (CurrentProject.Path == Normalized)
        {
            if (OutId != nullptr)
            {
                *OutId = CurrentProject.Id;
            }
            return true;
        }
    }
    Project const CurrentProject{Util::Uuid4(), Util::BaseName(Normalized), Normalized};
    State_.Projects.push_back(CurrentProject);
    if (OutId != nullptr)
    {
        *OutId = CurrentProject.Id;
    }
    MarkDirty();
    return true;
}

Session::StatusFn App::StatusCallback()
{
    return [this](Session& CurrentSession, Status OldStatus, Status NewStatus, double BusySeconds)
    { OnStatus(CurrentSession, OldStatus, NewStatus, BusySeconds); };
}

void App::OnStatus(Session& CurrentSession, Status, Status NewStatus, double BusySeconds)
{
    bool Visible = false;
    for (const std::string& Id : VisibleIds())
    {
        if (Id == CurrentSession.Rec.Id)
        {
            Visible = true;
        }
    }
    bool const Watching = Focused_ && State_.Ui.ActiveId == CurrentSession.Rec.Id && Visible;
    bool const Waiting = NewStatus == Status::Waiting;
    bool const Done = NewStatus == Status::Idle && BusySeconds >= 4.0;
    if ((Waiting || Done) && Watching == false)
    {
        CurrentSession.Unseen = true;
        if (Focused_ == false)
        {
            Project* const CurrentProject = FindProject(CurrentSession.Rec.ProjectId);
            if (State_.AppSettings.Notifications)
            {
                Platform::Notify(Window_, (CurrentProject != nullptr ? CurrentProject->Name : std::string("Claude")) + " \xC2\xB7 " + CurrentSession.Rec.Name,
                                 Waiting ? "Waiting for your input or approval" : "Finished — ready for your next prompt");
            }
            Platform::Flash(Window_, true);
        }
    }
    if (NewStatus == Status::Busy || NewStatus == Status::Exited)
    {
        CurrentSession.Unseen = false;
    }
}

void App::SaveNow()
{
    State_.Sessions.clear();
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (ShuttingDown_ == false && CurrentSession->Attempted == true)
        {
            CurrentSession->Rec.WasRunning = CurrentSession->IsRunning();
        }
        State_.Sessions.push_back(CurrentSession->Rec);
    }
    Store_.Save();
    Dirty_ = false;
    LastSave_ = Now_;
}

void App::ShowToast(const std::string& Text, bool IsError)
{
    Toasts_.push_back({Text, Now_ + (IsError ? 9.0 : 6.0), IsError});
}

void App::Shutdown()
{
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Attempted == true)
        {
            CurrentSession->Rec.WasRunning = CurrentSession->IsRunning();
        }
    }
    ShuttingDown_ = true;
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        CurrentSession->Kill(Now_, nullptr);
    }
    SaveNow();
    Platform::Shutdown(Window_);
}

double App::FrameWaitSeconds() const
{
    constexpr double ANIMATION_WAIT_SECONDS = 1.0 / 30.0;
    constexpr double IDLE_WAIT_SECONDS = 0.1;
    if (Toasts_.empty() == false)
    {
        return ANIMATION_WAIT_SECONDS;
    }
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->CurrentStatus == Status::Busy)
        {
            return ANIMATION_WAIT_SECONDS;
        }
    }
    return IDLE_WAIT_SECONDS;
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

// ---------------------------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------------------------

void App::TickSessions(double Now)
{
    Now_ = Now;
    Session::StatusFn const Callback = StatusCallback();
    for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
    {
        if (CurrentSession->Attempted == false && CurrentSession->Rec.WasRunning == true && IsSessionOpen(CurrentSession->Rec.Id) == false)
        {
            CurrentSession->Attempted = true;
            std::string Err;
            if (CurrentSession->Start(State_.AppSettings.ClaudeCommand, BACKGROUND_COLS, BACKGROUND_ROWS, Now_, Err) == false)
            {
                ShowToast("Could not launch Claude: " + Err, true);
            }
            MarkDirty();
        }
        CurrentSession->Tick(Now_, Callback);
        PollScreenshots(*CurrentSession);
    }
}

void App::Frame(double Now)
{
    Now_ = Now;
    if (PendingCloseId_.empty() == false)
    {
        const std::string CloseId = PendingCloseId_;
        PendingCloseId_.clear();
        CloseSession(CloseId, 0);
    }
    SyncTheme();
    Images_->BeginFrame();
    bool const Focused = glfwGetWindowAttrib(Window_, GLFW_FOCUSED) != 0;
    if (Focused && Focused_ == false)
    {
        Platform::Flash(Window_, false);
        if (Session* const Active = ActiveSession())
        {
            Active->Unseen = false;
        }
    }
    Focused_ = Focused;

    TickSessions(Now);
    EnsureActiveVisible();

    constexpr double USAGE_REFRESH_INTERVAL_SECONDS = 300.0;
    bool UsageInFlight = false, UsageDone = false;
    UsageInfo CurrentUsage;
    ReadUsageFetch(UsageInFlight, UsageDone, CurrentUsage);
    if (UsageInFlight == false && Now_ - UsageLastFetchAt_ > USAGE_REFRESH_INTERVAL_SECONDS)
    {
        UsageLastFetchAt_ = Now_;
        StartUsageFetch();
    }

    constexpr double WINDOW_PING_RETRY_SECONDS = 120.0;
    bool const WindowIsFree = CurrentUsage.Ok == true &&
                              (CurrentUsage.SessionResetsAt.empty() == true || UiHelpers::SecondsUntil(CurrentUsage.SessionResetsAt) == 0);
    if (State_.AppSettings.AutoStartWindow == true && WindowIsFree == true && WindowPingBusy_->load() == false &&
        Now_ - WindowPingLastAt_ > WINDOW_PING_RETRY_SECONDS)
    {
        StartWindowPing();
        UsageLastFetchAt_ = Now_ - USAGE_REFRESH_INTERVAL_SECONDS + 20.0;
    }

    if (State_.AppSettings.GitHubEnabled == true)
    {
        const double GitHubInterval = Modal_ == Modal::GitHub ? 3.0 : 60.0;
        if (Now_ - GitHubLastCheckAt_ > GitHubInterval)
        {
            GitHubLastCheckAt_ = Now_;
            StartGitHubCheck();
        }
    }

    HandleEvents();
    DrawRoot();
    DrawModals();
    DrawToasts();

    if (Dirty_ && Now_ - LastSave_ > 0.5)
    {
        SaveNow();
    }
}

void App::DrawStatusDot(ImDrawList* DrawList, ImVec2 Center, float Radius, const Session& CurrentSession) const
{
    switch (CurrentSession.CurrentStatus)
    {
        case Status::Busy:
        {
            float const Pulse = 0.55f + 0.45f * std::sin((float)Now_ * 6.0f);
            DrawList->AddCircleFilled(Center, Radius * (0.8f + 0.2f * Pulse), UiHelpers::WithAlpha(BusyColor_, (int)(110 + 145 * Pulse)));
            break;
        }
        case Status::Starting: DrawList->AddCircleFilled(Center, Radius, UiHelpers::WithAlpha(BusyColor_, 150)); break;
        case Status::Waiting:
            DrawList->AddCircleFilled(Center, Radius + 3, UiHelpers::WithAlpha(WaitingColor_, 60));
            DrawList->AddCircleFilled(Center, Radius, WaitingColor_);
            break;
        case Status::Idle:
            if (CurrentSession.Unseen)
            {
                DrawList->AddCircleFilled(Center, Radius + 3, UiHelpers::WithAlpha(DoneColor_, 60));
                DrawList->AddCircleFilled(Center, Radius, DoneColor_);
            }
            else
            {
                DrawList->AddCircleFilled(Center, Radius, UiHelpers::WithAlpha(DoneColor_, 130));
            }
            break;
        case Status::Exited: DrawList->AddCircle(Center, Radius - 0.5f, IdleColor_, 0, 1.5f); break;
    }
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

void App::DrawRoot()
{
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(Viewport->WorkPos);
    ImGui::SetNextWindowSize(Viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, BackgroundColorV4_);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);

    if (State_.Ui.SidebarCollapsed == true)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * UiScale_, 10 * UiScale_));
        ImGui::BeginChild("##sideCollapsed", ImVec2(28.0f * UiScale_, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::Button(">", ImVec2(-1, 0)) == true)
        {
            State_.Ui.SidebarCollapsed = false;
            MarkDirty();
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Expand sidebar");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 0);
        DrawMainAndPanel();
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    float const SidebarWidth = std::clamp(State_.Ui.SidebarWidth, 200.0f * UiScale_, 520.0f * UiScale_);
    DrawSidebar(SidebarWidth);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleColor(ImGuiCol_Button, PaneBorderColorV4_);
    ImGui::InvisibleButton("##splitter", ImVec2(5.0f * UiScale_, -1));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (ImGui::IsItemActive())
    {
        State_.Ui.SidebarWidth = std::clamp(SidebarWidth + ImGui::GetIO().MouseDelta.x, 200.0f * UiScale_, 520.0f * UiScale_);
        MarkDirty();
    }
    ImVec2 SplitterMin = ImGui::GetItemRectMin(), SplitterMax = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(SplitterMin, SplitterMax, ImGui::IsItemActive() ? AccentColor_ : PaneBorderColorU32_);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);

    DrawMainAndPanel();
    ImGui::End();
    ImGui::PopStyleColor();
}

void App::DrawSidebar(float Width)
{
    const float Scale = UiScale_;

    // A row icon button (the "+"/"x" on project and session rows): transparent at rest so it reads
    // as part of the row, and on hover/press it fills with the exact same flat rectangle the row
    // itself uses for its own hover/selected highlight, instead of looking like a separate pill button.
    const std::function<bool(const char*)> IconButton = [&](const char* Label)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        bool const Clicked = ImGui::Button(Label);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        return Clicked;
    };

    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * Scale, 10 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::BeginChild("##side", ImVec2(Width, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, AccentColorV4_);
    ImGui::TextUnformatted("*");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted("Claude Terminal");
    ImGui::SameLine();
    const float CollapseButtonWidth = ImGui::CalcTextSize("<").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - CollapseButtonWidth);
    if (IconButton("<") == true)
    {
        State_.Ui.SidebarCollapsed = true;
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Collapse sidebar");
    }
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, AccentColorV4_);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHoverColorV4_);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActiveColorV4_);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.06f, 0.04f, 1));
    if (ImGui::Button("+ Add project folder", ImVec2(-1, 0)))
    {
        PathBuffer_[0] = 0;
        ModalError_.clear();
        OpenModal(Modal::AddProject);
    }
    ImGui::PopStyleColor(4);
    ImGui::Spacing();

    const float FooterHeight = ImGui::GetTextLineHeightWithSpacing() * 3.2f + ImGui::GetFrameHeightWithSpacing();
    const float ArchiveHeight = State_.Ui.ArchiveOpen == true ? 170.0f * Scale : ImGui::GetFrameHeightWithSpacing() + 6.0f * Scale;
    ImGui::BeginChild("##tree", ImVec2(0, -(FooterHeight + ArchiveHeight + ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (State_.Projects.empty())
    {
        ImGui::TextDisabled("No projects yet.");
    }

    size_t DragSourceIndex = SIZE_MAX;
    size_t DragTargetIndex = SIZE_MAX;
    std::string SessionDragSourceId;
    std::string SessionDragTargetId;
    for (size_t ProjectIndex = 0; ProjectIndex < State_.Projects.size(); ProjectIndex++)
    {
        Project const& CurrentProject = State_.Projects[ProjectIndex];
        ImGui::PushID(CurrentProject.Id.c_str());
        bool const Collapsed = State_.Ui.Collapsed[CurrentProject.Id];

        // attention summary for the header dot
        const Session* Worst = nullptr;
        for (const std::unique_ptr<Session>& CurrentSession : Sessions_)
        {
            if (CurrentSession->Rec.ProjectId != CurrentProject.Id)
            {
                continue;
            }
            if (CurrentSession->CurrentStatus == Status::Waiting)
            {
                Worst = CurrentSession.get();
                break;
            }
            if (CurrentSession->Unseen)
            {
                Worst = CurrentSession.get();
            }
            else if (Worst == nullptr && CurrentSession->CurrentStatus == Status::Busy)
            {
                Worst = CurrentSession.get();
            }
        }

        ImGui::SetNextItemOpen(Collapsed == false, ImGuiCond_Always);
        bool const Open = ImGui::TreeNodeEx(CurrentProject.Name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding |
                                                                 ImGuiTreeNodeFlags_AllowOverlap);
        const float ArrowWidth = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) == true && ImGui::GetMousePos().x - ImGui::GetItemRectMin().x > ArrowWidth)
        {
            ProjectScopeId_ = CurrentProject.Id;
        }
        if (ImGui::IsItemToggledOpen())
        {
            State_.Ui.Collapsed[CurrentProject.Id] = Open == false;
            MarkDirty();
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID) == true)
        {
            ImGui::SetDragDropPayload("PROJECT_REORDER", &ProjectIndex, sizeof(size_t));
            ImGui::TextUnformatted(CurrentProject.Name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget() == true)
        {
            const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload("PROJECT_REORDER");
            if (Payload != nullptr)
            {
                DragSourceIndex = *static_cast<const size_t*>(Payload->Data);
                DragTargetIndex = ProjectIndex;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(0) == false)
        {
            ImGui::SetTooltip("%s", CurrentProject.Path.c_str());
        }
        if (ImGui::BeginPopupContextItem("##projmenu"))
        {
            if (ImGui::MenuItem("New session"))
            {
                ModalTarget_ = CurrentProject.Id;
                OpenModal(Modal::NewSession);
            }
            if (ImGui::MenuItem("Rename project"))
            {
                ModalTarget_ = CurrentProject.Id;
                RenameIsProject_ = true;
                std::snprintf(NameBuffer_, sizeof NameBuffer_, "%s", CurrentProject.Name.c_str());
                OpenModal(Modal::Rename);
            }
            if (ImGui::MenuItem("Reveal folder"))
            {
                Platform::Reveal(CurrentProject.Path);
            }
            if (ImGui::MenuItem("Close unopened sessions..."))
            {
                RequestCloseUnopenedSessions(CurrentProject.Id);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove project"))
            {
                std::string Id = CurrentProject.Id, Name = CurrentProject.Name;
                ConfirmTitle_ = "Remove \"" + Name + "\"?";
                ConfirmText_ = "This stops its sessions and removes the project from the list.\nFiles and git worktrees on disk are left untouched.";
                ConfirmYesAction_ = [this, Id] { RemoveProject(Id); };
                OpenModal(Modal::Confirm);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 54 * Scale);
        if (IconButton("+"))
        {
            ModalTarget_ = CurrentProject.Id;
            OpenModal(Modal::NewSession);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("New session");
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 28 * Scale);
        if (IconButton("++"))
        {
            std::string Err;
            Session* const ContinuedSession = CreateSession(CurrentProject.Id, "", false, true, Err);
            if (ContinuedSession != nullptr)
            {
                State_.Ui.Collapsed[CurrentProject.Id] = false;
                OpenSession(ContinuedSession->Rec.Id);
            }
            else
            {
                ShowToast(Err, true);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("New session continuing the last conversation");
        }
        if (Worst != nullptr)
        {
            ImVec2 const DotCenter(ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - 68 * Scale,
                             ImGui::GetItemRectMin().y + ImGui::GetItemRectSize().y * 0.5f);
            DrawStatusDot(ImGui::GetWindowDrawList(), DotCenter, 4.5f * Scale, *Worst);
        }

        if (Open)
        {
            ImGui::Separator();
            ImDrawList* const GroupDrawList = ImGui::GetWindowDrawList();
            ImDrawListSplitter GroupSplitter;
            GroupSplitter.Split(GroupDrawList, 2);
            GroupSplitter.SetCurrentChannel(GroupDrawList, 1);
            ImVec2 const GroupStart = ImGui::GetCursorScreenPos();
            float const GroupWidth = ImGui::GetContentRegionAvail().x;
            bool Any = false;
            for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
            {
                Session& CurrentSession = *SessionPtr;
                if (CurrentSession.Rec.ProjectId != CurrentProject.Id)
                {
                    continue;
                }
                Any = true;
                ImGui::PushID(CurrentSession.Rec.Id.c_str());
                bool const IsOpen = std::find(State_.Ui.OpenIds.begin(), State_.Ui.OpenIds.end(), CurrentSession.Rec.Id) != State_.Ui.OpenIds.end();
                bool const IsActive = State_.Ui.ActiveId == CurrentSession.Rec.Id;
                float const RowHeight = ImGui::GetFrameHeight();
                float const RowWidth = ImGui::GetContentRegionAvail().x - 26 * Scale;
                ImGui::Indent(12 * Scale);
                ImVec2 const RowPos = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##s", IsActive, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(RowWidth - 12 * Scale, RowHeight)))
                {
                    OpenSessionFromSidebar(CurrentSession.Rec.Id);
                    if (ImGui::IsMouseDoubleClicked(0))
                    {
                        ModalTarget_ = CurrentSession.Rec.Id;
                        RenameIsProject_ = false;
                        std::snprintf(NameBuffer_, sizeof NameBuffer_, "%s", CurrentSession.Rec.Name.c_str());
                        OpenModal(Modal::Rename);
                    }
                }
                ImDrawList* DrawList = ImGui::GetWindowDrawList();
                if (IsOpen && IsActive == false)
                {
                    DrawList->AddRectFilled(ImVec2(RowPos.x - 4 * Scale, RowPos.y + 3), ImVec2(RowPos.x - 2 * Scale, RowPos.y + RowHeight - 3),
                                            IdleColor_);
                }
                DrawStatusDot(DrawList, ImVec2(RowPos.x + 9 * Scale, RowPos.y + RowHeight * 0.5f), 4.5f * Scale, CurrentSession);
                DrawList->AddText(ImVec2(RowPos.x + 22 * Scale, RowPos.y + (RowHeight - ImGui::GetFontSize()) * 0.5f), TextColorU32_,
                                  CurrentSession.Rec.Name.c_str());
                if (CurrentSession.Rec.WorktreeInfo)
                {
                    const char* const Tag = "wt";
                    ImVec2 const TagSize = ImGui::CalcTextSize(Tag);
                    DrawList->AddText(ImVec2(RowPos.x + RowWidth - 12 * Scale - TagSize.x - 6 * Scale, RowPos.y + (RowHeight - TagSize.y) * 0.5f),
                                      TextDisabledColorU32_, Tag);
                }
                if (ImGui::BeginDragDropSource() == true)
                {
                    ImGui::SetDragDropPayload("SESSION_REORDER", CurrentSession.Rec.Id.c_str(), CurrentSession.Rec.Id.size() + 1);
                    ImGui::TextUnformatted(CurrentSession.Rec.Name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget() == true)
                {
                    const ImGuiPayload* const Payload = ImGui::AcceptDragDropPayload("SESSION_REORDER");
                    if (Payload != nullptr)
                    {
                        SessionDragSourceId = static_cast<const char*>(Payload->Data);
                        SessionDragTargetId = CurrentSession.Rec.Id;
                    }
                    ImGui::EndDragDropTarget();
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(0) == false)
                {
                    ImGui::SetTooltip("%s%s%s", CurrentSession.Rec.Cwd.c_str(), CurrentSession.Rec.WorktreeInfo ? "\nbranch: " : "",
                                      CurrentSession.Rec.WorktreeInfo ? CurrentSession.Rec.WorktreeInfo->Branch.c_str() : "");
                }
                if (ImGui::BeginPopupContextItem("##sessmenu"))
                {
                    if (ImGui::MenuItem("Open"))
                    {
                        OpenSessionFromSidebar(CurrentSession.Rec.Id);
                    }
                    if (ImGui::MenuItem("Rename"))
                    {
                        ModalTarget_ = CurrentSession.Rec.Id;
                        RenameIsProject_ = false;
                        std::snprintf(NameBuffer_, sizeof NameBuffer_, "%s", CurrentSession.Rec.Name.c_str());
                        OpenModal(Modal::Rename);
                    }
                    if (IsOpen && ImGui::MenuItem("Hide pane"))
                    {
                        HideSession(CurrentSession.Rec.Id);
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Close session..."))
                    {
                        RequestCloseSession(CurrentSession.Rec.Id);
                    }
                    ImGui::EndPopup();
                }
                ImGui::SameLine(0, 0);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 2 * Scale);
                if (IconButton("x"))
                {
                    RequestCloseSession(CurrentSession.Rec.Id);
                }
                ImGui::Unindent(12 * Scale);
                ImGui::PopID();
            }
            if (Any == false)
            {
                ImGui::Indent(12 * Scale);
                ImGui::TextDisabled("No sessions yet");
                ImGui::Unindent(12 * Scale);
            }
            GroupSplitter.SetCurrentChannel(GroupDrawList, 0);
            GroupDrawList->AddRectFilled(GroupStart, ImVec2(GroupStart.x + GroupWidth, ImGui::GetCursorScreenPos().y),
                                         UiHelpers::WithAlpha(TextColorU32_, 14), 4 * Scale);
            GroupSplitter.Merge(GroupDrawList);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (DragSourceIndex != DragTargetIndex && DragSourceIndex < State_.Projects.size() && DragTargetIndex < State_.Projects.size())
    {
        Project Moved = std::move(State_.Projects[DragSourceIndex]);
        State_.Projects.erase(State_.Projects.begin() + DragSourceIndex);
        State_.Projects.insert(State_.Projects.begin() + DragTargetIndex, std::move(Moved));
        MarkDirty();
    }
    if (SessionDragSourceId.empty() == false && SessionDragSourceId != SessionDragTargetId)
    {
        const auto FindSession = [this](const std::string& Id)
        {
            return std::find_if(Sessions_.begin(), Sessions_.end(), [&Id](const std::unique_ptr<Session>& Item) { return Item->Rec.Id == Id; });
        };
        const auto SourceIt = FindSession(SessionDragSourceId);
        const auto TargetIt = FindSession(SessionDragTargetId);
        if (SourceIt != Sessions_.end() && TargetIt != Sessions_.end() && (*SourceIt)->Rec.ProjectId == (*TargetIt)->Rec.ProjectId)
        {
            const size_t TargetIndex = static_cast<size_t>(TargetIt - Sessions_.begin());
            std::unique_ptr<Session> Moved = std::move(*SourceIt);
            Sessions_.erase(SourceIt);
            Sessions_.insert(Sessions_.begin() + TargetIndex, std::move(Moved));
            MarkDirty();
        }
    }
    ImGui::EndChild();

    ImGui::BeginChild("##archive", ImVec2(0, ArchiveHeight), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::SetNextItemOpen(State_.Ui.ArchiveOpen, ImGuiCond_Always);
    const std::string ArchiveLabel = "Archive (" + std::to_string(State_.Archive.size()) + ")";
    const bool ArchiveExpanded = ImGui::TreeNodeEx(ArchiveLabel.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    if (ImGui::IsItemToggledOpen() == true)
    {
        State_.Ui.ArchiveOpen = ArchiveExpanded;
        MarkDirty();
    }
    if (ImGui::BeginPopupContextItem("##archiveHeaderMenu") == true)
    {
        if (ImGui::MenuItem("Delete all archived", nullptr, false, State_.Archive.empty() == false) == true)
        {
            RequestDeleteAllArchived();
        }
        ImGui::EndPopup();
    }
    size_t ArchiveDeleteIndex = SIZE_MAX;
    size_t ArchiveRestoreIndex = SIZE_MAX;
    if (ArchiveExpanded == true)
    {
        if (State_.Archive.empty() == true)
        {
            ImGui::Indent(12 * Scale);
            ImGui::TextDisabled("Nothing archived");
            ImGui::Unindent(12 * Scale);
        }
        else if (ImGui::SmallButton("Delete all archived") == true)
        {
            RequestDeleteAllArchived();
        }
        for (size_t ArchiveIndex = 0; ArchiveIndex < State_.Archive.size(); ArchiveIndex++)
        {
            const ArchivedSession& Archived = State_.Archive[ArchiveIndex];
            ImGui::PushID(static_cast<int>(ArchiveIndex));
            ImGui::Indent(12 * Scale);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Archived.Name.c_str());
            if (Archived.ProjectName.empty() == false)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", Archived.ProjectName.c_str());
            }
            if (ImGui::BeginPopupContextItem("##archivemenu") == true)
            {
                if (ImGui::MenuItem("Restore", nullptr, false, Archived.Rec.ClaudeSessionId.empty() == false) == true)
                {
                    ArchiveRestoreIndex = ArchiveIndex;
                }
                if (ImGui::MenuItem("Delete permanently") == true)
                {
                    ArchiveDeleteIndex = ArchiveIndex;
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 24 * Scale);
            if (IconButton("x") == true)
            {
                ArchiveDeleteIndex = ArchiveIndex;
            }
            if (ImGui::IsItemHovered() == true)
            {
                ImGui::SetTooltip("Delete permanently");
            }
            ImGui::Unindent(12 * Scale);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ArchiveRestoreIndex < State_.Archive.size())
    {
        RestoreArchivedSession(ArchiveRestoreIndex);
        MarkDirty();
    }
    else if (ArchiveDeleteIndex < State_.Archive.size())
    {
        State_.Archive.erase(State_.Archive.begin() + ArchiveDeleteIndex);
        MarkDirty();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Checkbox("Close without confirmation", &State_.Ui.CloseWithoutConfirm) == true)
    {
        MarkDirty();
    }
    ImGui::TextDisabled("Alt+1-9  focus pane");
    ImGui::TextDisabled("Ctrl+Shift+N  new session");
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void App::DrawToolbar()
{
    const float Scale = UiScale_;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 6 * Scale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    float const ToolbarHeight = ImGui::GetFrameHeight() + 12 * Scale;
    ImGui::BeginChild("##toolbar", ImVec2(0, ToolbarHeight), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* DrawList = ImGui::GetWindowDrawList();

    // right-aligned controls first so chips know how much room is left
    const char* const AllLabel = "All";
    const char* const RunningLabel = "Running";
    const char* const ActionLabel = "Requires action";
    const char* const TileLabel = "Tiles";
    const char* const TabLabel = "Tabs";
    float const PadX = ImGui::GetStyle().FramePadding.x * 2;
    const float SessionMeterWidth = 120.0f * Scale;
    const float WeeklyMeterWidth = 140.0f * Scale;
    const bool GitHubShown = State_.AppSettings.GitHubEnabled;
    const std::string GitHubText = GitHubShown == true ? GitHubLabel() : std::string();
    float RightWidth = ImGui::CalcTextSize(AllLabel).x + ImGui::CalcTextSize(RunningLabel).x + ImGui::CalcTextSize(ActionLabel).x +
                       ImGui::CalcTextSize(TileLabel).x + ImGui::CalcTextSize(TabLabel).x + ImGui::CalcTextSize("Settings").x + ImGui::CalcTextSize("Focus project").x + ImGui::GetFrameHeight() +
                       SessionMeterWidth + WeeklyMeterWidth + ImGui::CalcTextSize("Session:").x + ImGui::CalcTextSize("Weekly:").x +
                       2.0f * Scale * UiHelpers::USAGE_LABEL_GAP + 10.0f * Scale + 1.0f * Scale + ImGui::GetStyle().ItemSpacing.x + PadX * 6 + 48 * Scale;
    if (GitHubShown == true)
    {
        RightWidth += ImGui::CalcTextSize(GitHubText.c_str()).x + PadX + ImGui::GetStyle().ItemSpacing.x;
    }
    float const Avail = ImGui::GetContentRegionAvail().x;

    // Group open session chips under a header per project, enclosed in one visible container box
    // so the children read as nested inside their parent. Clicking a header narrows the tab/tile
    // pane area down to that project (State_.Ui.TabFilterProjectId), but every project's group
    // stays visible here — only the selected one's box is highlighted, so the others aren't hidden.
    const std::vector<std::string> BarIds = ActionFilteredIds();
    std::vector<std::string> ProjectOrder;
    for (const std::string& OpenId : BarIds)
    {
        const Session* const SessionPtr = FindSession(OpenId);
        if (SessionPtr == nullptr)
        {
            continue;
        }
        if (std::find(ProjectOrder.begin(), ProjectOrder.end(), SessionPtr->Rec.ProjectId) == ProjectOrder.end())
        {
            ProjectOrder.push_back(SessionPtr->Rec.ProjectId);
        }
    }

    const float ItemGap = ImGui::GetStyle().ItemSpacing.x;
    const float GroupPadding = 3.0f * Scale;
    const float GroupGap = 14.0f * Scale;
    bool AnyChip = false;
    for (const std::string& GroupId : ProjectOrder)
    {
        const Project* const ProjectRecord = FindProject(GroupId);
        if (ProjectRecord == nullptr)
        {
            continue;
        }

        std::vector<const Session*> ChildSessions;
        for (const std::string& ChildId : BarIds)
        {
            const Session* const SessionPtr = FindSession(ChildId);
            if (SessionPtr != nullptr && SessionPtr->Rec.ProjectId == GroupId)
            {
                ChildSessions.push_back(SessionPtr);
            }
        }

        const ImVec2 HeaderTextSize = ImGui::CalcTextSize(ProjectRecord->Name.c_str());
        const float HeaderCloseWidth = 20.0f * Scale;
        const float HeaderTextAreaWidth = HeaderTextSize.x + 16.0f * Scale;
        const float HeaderWidth = HeaderTextAreaWidth + HeaderCloseWidth;
        float GroupInnerWidth = HeaderWidth;
        for (const Session* const ChildSession : ChildSessions)
        {
            GroupInnerWidth += ItemGap + ImGui::CalcTextSize(ChildSession->Rec.Name.c_str()).x + 40.0f * Scale;
        }

        if (ImGui::GetCursorPosX() + GroupInnerWidth + GroupPadding * 2.0f > Avail - RightWidth)
        {
            break;
        }
        AnyChip = true;

        const bool IsSelected = State_.Ui.TabFilterProjectId == ProjectRecord->Id;
        const ImU32 ContainerBg = IsSelected ? UiHelpers::WithAlpha(AccentColor_, 35) : UiHelpers::WithAlpha(PaneBgColorU32_, 160);
        const ImU32 ContainerBorder = IsSelected ? AccentColor_ : UiHelpers::WithAlpha(TextDisabledColorU32_, 110);
        const ImVec2 GroupStart = ImGui::GetCursorScreenPos();
        const ImVec2 BoxMin(GroupStart.x - GroupPadding, GroupStart.y - GroupPadding);
        const ImVec2 BoxMax(GroupStart.x + GroupInnerWidth + GroupPadding, GroupStart.y + ImGui::GetFrameHeight() + GroupPadding);
        DrawList->AddRectFilled(BoxMin, BoxMax, ContainerBg, 8.0f);
        DrawList->AddRect(BoxMin, BoxMax, ContainerBorder, 8.0f, 0, IsSelected ? 2.0f : 1.0f);

        ImGui::PushID(ProjectRecord->Id.c_str());
        const ImVec2 ClosePos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##projclose", ImVec2(HeaderCloseWidth, ImGui::GetFrameHeight())))
        {
            for (const Session* const ChildSession : ChildSessions)
            {
                HideSession(ChildSession->Rec.Id);
            }
            if (IsSelected == true)
            {
                SetTabFilter(std::string());
            }
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Close %s's open tabs", ProjectRecord->Name.c_str());
        }
        const ImU32 CloseColor = ImGui::IsItemHovered() == true ? WaitingColor_ : TextDisabledColorU32_;
        const ImVec2 CloseTextSize = ImGui::CalcTextSize("x");
        DrawList->AddText(ImVec2(ClosePos.x + (HeaderCloseWidth - CloseTextSize.x) * 0.5f,
                                 ClosePos.y + (ImGui::GetFrameHeight() - CloseTextSize.y) * 0.5f - ImGui::GetFontSize() * 0.12f),
                          CloseColor, "x");

        ImGui::SameLine(0, 0);
        const ImVec2 HeaderPos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##projchip", ImVec2(HeaderTextAreaWidth, ImGui::GetFrameHeight())))
        {
            SetTabFilter(IsSelected ? std::string() : ProjectRecord->Id);
            ProjectScopeId_ = IsSelected == true ? std::string() : ProjectRecord->Id;
        }
        const ImU32 HeaderColor = IsSelected ? AccentColor_ : TextDisabledColorU32_;
        DrawList->AddText(ImVec2(HeaderPos.x + 4.0f * Scale, HeaderPos.y + (ImGui::GetFrameHeight() - HeaderTextSize.y) * 0.5f), HeaderColor,
                          ProjectRecord->Name.c_str());
        ImGui::PopID();
        ImGui::SameLine();

        for (const Session* const ChildSession : ChildSessions)
        {
            const bool IsActiveChip = ChildSession->Rec.Id == State_.Ui.ActiveId && ProjectScopeId_.empty() == true;
            ImGui::PushID(ChildSession->Rec.Id.c_str());
            const ImVec2 ChipPos = ImGui::GetCursorScreenPos();
            const ImVec2 ChipTextSize = ImGui::CalcTextSize(ChildSession->Rec.Name.c_str());
            const float ChipWidth = ChipTextSize.x + 40.0f * Scale;
            const float ChipCloseWidth = 20.0f * Scale;
            if (ImGui::InvisibleButton("##chip", ImVec2(ChipWidth - ChipCloseWidth, ImGui::GetFrameHeight())))
            {
                const std::vector<std::string>::iterator ExtraAt = std::find(FilterExtraIds_.begin(), FilterExtraIds_.end(), ChildSession->Rec.Id);
                if (IsActiveChip == true && ExtraAt != FilterExtraIds_.end())
                {
                    FilterExtraIds_.erase(ExtraAt);
                    EnsureActiveVisible();
                    MarkDirty();
                }
                else
                {
                    OpenSession(ChildSession->Rec.Id);
                }
            }
            if (ImGui::BeginPopupContextItem("##chipmenu") == true)
            {
                if (ImGui::MenuItem("Close") == true)
                {
                    RequestCloseSession(ChildSession->Rec.Id);
                }
                if (ImGui::MenuItem("Close All") == true)
                {
                    for (const Session* const SiblingSession : ChildSessions)
                    {
                        HideSession(SiblingSession->Rec.Id);
                    }
                    if (IsSelected == true)
                    {
                        SetTabFilter(std::string());
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine(0, 0);
            const ImVec2 ChipClosePos = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##chipclose", ImVec2(ChipCloseWidth, ImGui::GetFrameHeight())) == true)
            {
                HideSession(ChildSession->Rec.Id);
            }
            const bool ChipCloseHovered = ImGui::IsItemHovered();
            if (ChipCloseHovered == true)
            {
                ImGui::SetTooltip("Hide tab");
            }
            DrawList->AddRectFilled(ChipPos, ImVec2(ChipPos.x + ChipWidth, ChipPos.y + ImGui::GetFrameHeight()), ChipBgColorU32_, 6);
            if (IsActiveChip == true)
            {
                DrawList->AddRect(ChipPos, ImVec2(ChipPos.x + ChipWidth, ChipPos.y + ImGui::GetFrameHeight()), AccentColor_, 6);
            }
            DrawStatusDot(DrawList, ImVec2(ChipPos.x + 12.0f * Scale, ChipPos.y + ImGui::GetFrameHeight() * 0.5f), 4.0f * Scale, *ChildSession);
            DrawList->AddText(ImVec2(ChipPos.x + 24.0f * Scale, ChipPos.y + (ImGui::GetFrameHeight() - ChipTextSize.y) * 0.5f), TextColorU32_,
                              ChildSession->Rec.Name.c_str());
            const ImVec2 ChipXSize = ImGui::CalcTextSize("x");
            DrawList->AddText(ImVec2(ChipClosePos.x + (ChipCloseWidth - ChipXSize.x) * 0.5f,
                                     ChipClosePos.y + (ImGui::GetFrameHeight() - ChipXSize.y) * 0.5f - ImGui::GetFontSize() * 0.12f),
                              ChipCloseHovered == true ? WaitingColor_ : TextDisabledColorU32_, "x");
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::SameLine(0, GroupGap);
    }
    if (AnyChip == false)
    {
        ImGui::TextDisabled("No open sessions");
    }

    ImGui::SameLine(Avail - RightWidth + 8 * Scale);
    const std::function<void(const char*, const char*)> FilterButton = [&](const char* Label, const char* Mode)
    {
        bool const Active = State_.Ui.ActionFilter == Mode;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              Active ? ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        bool const Clicked = ImGui::Button(Label);
        ImGui::PopStyleColor();
        if (Clicked)
        {
            State_.Ui.ActionFilter = Active ? "all" : Mode;
            MarkDirty();
        }
    };
    FilterButton(AllLabel, "all");
    ImGui::SameLine(0, 2);
    FilterButton(RunningLabel, "running");
    ImGui::SameLine(0, 2);
    FilterButton(ActionLabel, "action");
    ImGui::SameLine();
    bool const TileMode = State_.Ui.Mode == "tile";
    ImGui::PushStyleColor(ImGuiCol_Button,
                          TileMode ? ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button(TileLabel))
    {
        State_.Ui.Mode = "tile";
        MaximizedId_.clear();
        MarkDirty();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 2);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          TileMode == false ? ImGui::ColorConvertU32ToFloat4(UiHelpers::WithAlpha(AccentColor_, 90)) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::Button(TabLabel))
    {
        State_.Ui.Mode = "tabs";
        MaximizedId_.clear();
        MarkDirty();
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Checkbox("Focus project", &State_.Ui.FocusProjectOnClick) == true)
    {
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Clicking a session in the sidebar shows only that project's open sessions");
    }
    ImGui::SameLine();
    const ImVec2 DividerPos = ImGui::GetCursorScreenPos();
    const float DividerHeight = ImGui::GetFrameHeight();
    ImGui::Dummy(ImVec2(1.0f * Scale, DividerHeight));
    DrawList->AddLine(ImVec2(DividerPos.x, DividerPos.y), ImVec2(DividerPos.x, DividerPos.y + DividerHeight),
                      ImGui::GetColorU32(ImGuiCol_Separator), 1.0f * Scale);
    ImGui::SameLine();

    bool UsageInFlight = false, UsageDone = false;
    UsageInfo CurrentUsage;
    ReadUsageFetch(UsageInFlight, UsageDone, CurrentUsage);
    const std::string UsageError = UsageDone == true && CurrentUsage.Ok == false
                                        ? (CurrentUsage.Error.empty() == false ? CurrentUsage.Error : "Could not fetch usage.")
                                        : std::string();
    const std::string SessionResetText = UiHelpers::FormatTimeUntil(CurrentUsage.SessionResetsAt);
    const std::string WeeklyResetText = UiHelpers::FormatTimeUntil(CurrentUsage.WeeklyResetsAt);
    const std::string WeeklyResetSuffix = WeeklyResetText.empty() == false ? "  |  " + WeeklyResetText : std::string();
    const std::string SessionResetSuffix = SessionResetText.empty() == false ? "  |  " + SessionResetText : std::string();
    bool UsageMeterClicked = false;
    if (DrawUsageMeter(DrawList, SessionMeterWidth, "Session", CurrentUsage.HasSession, CurrentUsage.SessionPct,
                       UsageFillColor(CurrentUsage.SessionPct),
                       UiHelpers::UsageResetProgress(CurrentUsage.SessionPct, UiHelpers::SecondsUntil(CurrentUsage.SessionResetsAt), UiHelpers::SESSION_WINDOW_SECONDS),
                       SessionResetSuffix, UsageError) == true)
    {
        UsageMeterClicked = true;
    }
    ImGui::SameLine(0, 10 * Scale);
    if (DrawUsageMeter(DrawList, WeeklyMeterWidth, "Weekly", CurrentUsage.HasWeekly, CurrentUsage.WeeklyPct,
                       UsageFillColor(CurrentUsage.WeeklyPct),
                       UiHelpers::UsageResetProgress(CurrentUsage.WeeklyPct, UiHelpers::SecondsUntil(CurrentUsage.WeeklyResetsAt), UiHelpers::WEEKLY_WINDOW_SECONDS),
                       WeeklyResetSuffix, UsageError) == true)
    {
        UsageMeterClicked = true;
    }
    if (UsageMeterClicked == true)
    {
        OpenModal(Modal::Usage);
    }
    ImGui::SameLine();
    if (GitHubShown == true)
    {
        const Git::GitHubStatus::State GitHubState = ReadGitHubStatus().Value;
        const ImVec4 GitHubColor = GitHubState == Git::GitHubStatus::State::LoggedIn ? ImGui::ColorConvertU32ToFloat4(DoneColor_)
                                   : GitHubState == Git::GitHubStatus::State::Unknown ? TextDisabledColorV4_
                                                                                      : WaitingColorV4_;
        ImGui::PushStyleColor(ImGuiCol_Text, GitHubColor);
        const bool GitHubClicked = ImGui::Button(GitHubText.c_str());
        ImGui::PopStyleColor();
        if (GitHubClicked == true)
        {
            GitHubLastCheckAt_ = -1.0e9;
            OpenModal(Modal::GitHub);
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Settings"))
    {
        std::snprintf(CommandBuffer_, sizeof CommandBuffer_, "%s", State_.AppSettings.ClaudeCommand.c_str());
        FontSizeEdit_ = State_.AppSettings.FontSize;
        PaneTitleScaleEdit_ = State_.AppSettings.PaneTitleScale;
        NotifyEdit_ = State_.AppSettings.Notifications;
        AutoStartEdit_ = State_.AppSettings.AutoStartWindow;
        GitHubEdit_ = State_.AppSettings.GitHubEnabled;
        KeysEdit_ = State_.AppSettings.Keys;
        KeyCaptureIndex_ = -1;
        KeyCaptureError_.clear();
        ThemeEdit_ = State_.AppSettings.Theme;
        SessionNameEdits_.clear();
        for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
        {
            std::array<char, 256> Buffer{};
            std::snprintf(Buffer.data(), Buffer.size(), "%s", SessionPtr->Rec.Name.c_str());
            SessionNameEdits_[SessionPtr->Rec.Id] = Buffer;
        }
        OpenModal(Modal::Settings);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void App::DrawMain(const float Width)
{
    const float Scale = UiScale_;
    ImGui::BeginChild("##main", ImVec2(Width, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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

void App::PollScreenshots(Session& CurrentSession)
{
    constexpr double POLL_INTERVAL_SECONDS = 1.5;
    constexpr double FIRST_POLL_STAGGER_SECONDS = 0.05;
    const std::string& Id = CurrentSession.Rec.Id;
    std::map<std::string, double>::iterator NextPoll = ScreenshotPollAt_.find(Id);
    if (NextPoll == ScreenshotPollAt_.end())
    {
        ScreenshotPollAt_[Id] = Now_ + FIRST_POLL_STAGGER_SECONDS * static_cast<double>(ScreenshotPollAt_.size());
        return;
    }
    if (Now_ < NextPoll->second)
    {
        return;
    }
    NextPoll->second = Now_ + POLL_INTERVAL_SECONDS;

    const std::vector<TranscriptScanner::Shot> Shots = TranscriptScanners_[Id].Poll(CurrentSession.Rec.ClaudeSessionId, CurrentSession.Rec.Cwd, Now_);
    for (const TranscriptScanner::Shot& Found : Shots)
    {
        if (ScreenshotFeed_.Add(Id, Found.Path, Found.ModifiedAt) == true && State_.Ui.ScreenshotPanelOpen == false)
        {
            UnseenScreenshots_++;
        }
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
        DrawMain(ImGui::GetContentRegionAvail().x - STRIP_WIDTH * Scale);
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
    DrawMain(ImGui::GetContentRegionAvail().x - PanelWidth - SplitterWidth);
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

void App::DrawScreenshotPanel(const float Width)
{
    const float Scale = UiScale_;
    const bool ProjectSelected = ProjectScopeId_.empty() == false;

    std::vector<const ScreenshotFeed::Entry*> Visible;
    for (const ScreenshotFeed::Entry& Item : ScreenshotFeed_.Entries())
    {
        if (ProjectSelected == true)
        {
            const Session* const Owner = FindSession(Item.SessionId);
            if (Owner == nullptr || Owner->Rec.ProjectId != ProjectScopeId_)
            {
                continue;
            }
        }
        else if (Item.SessionId != State_.Ui.ActiveId)
        {
            continue;
        }
        if (State_.Ui.HiddenScreenshots.count(Item.Path) > 0)
        {
            continue;
        }
        Visible.push_back(&Item);
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, SurfaceColorV4_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Scale, 8 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * Scale, 6 * Scale));
    ImGui::BeginChild("##shots", ImVec2(Width, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Screenshots");
    ImGui::SameLine();
    ImGui::TextDisabled("%d", static_cast<int>(Visible.size()));
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetFrameHeight() - 8 * Scale);
    if (ImGui::Button(">", ImVec2(ImGui::GetFrameHeight(), 0)) == true)
    {
        State_.Ui.ScreenshotPanelOpen = false;
        MarkDirty();
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetTooltip("Hide screenshots");
    }

    ImGui::TextDisabled("%s", ProjectSelected == true ? "From every session in this project" : "From the selected session");

    if (State_.Ui.HiddenScreenshots.empty() == false)
    {
        const std::string RestoreLabel = "Restore " + std::to_string(State_.Ui.HiddenScreenshots.size()) + " hidden###shotRestore";
        if (ImGui::SmallButton(RestoreLabel.c_str()) == true)
        {
            State_.Ui.HiddenScreenshots.clear();
            MarkDirty();
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Show every screenshot you've hidden again");
        }
        ImGui::SameLine();
    }
    if (Visible.empty() == false)
    {
        const std::string DeleteLabel = "Delete " + std::to_string(Visible.size()) + " photos###shotDelete";
        if (ImGui::SmallButton(DeleteLabel.c_str()) == true)
        {
            std::vector<std::string> Paths;
            for (const ScreenshotFeed::Entry* const Item : Visible)
            {
                Paths.push_back(Item->Path);
            }
            ConfirmTitle_ = "Delete " + std::to_string(Paths.size()) + " photos?";
            ConfirmText_ = "This permanently deletes the image files shown in this panel from disk.";
            ConfirmYesAction_ = [this, Paths]
            {
                for (const std::string& Path : Paths)
                {
                    std::error_code ErrorCode;
                    std::filesystem::remove(Path, ErrorCode);
                    ScreenshotFeed_.RemovePath(Path);
                    State_.Ui.HiddenScreenshots.erase(Path);
                }
                MarkDirty();
            };
            OpenModal(Modal::Confirm);
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetTooltip("Permanently delete the files listed below");
        }
    }

    ImGui::BeginChild("##shotList", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    if (Visible.empty() == true)
    {
        ImGui::TextDisabled("%s", ProjectSelected == true ? "No screenshots from this project's sessions yet." : "No screenshots from the selected session yet.");
    }
    const float CardWidth = ImGui::GetContentRegionAvail().x;
    std::vector<std::string> MissingPaths;
    for (const ScreenshotFeed::Entry* const Item : Visible)
    {
        if (DrawScreenshotCard(*Item, CardWidth) == false)
        {
            MissingPaths.push_back(Item->Path);
        }
    }
    for (const std::string& Path : MissingPaths)
    {
        ScreenshotFeed_.RemovePath(Path);
        ShotAspects_.erase(Path);
    }
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

bool App::DrawScreenshotCard(const ScreenshotFeed::Entry& Item, const float Width)
{
    const float Scale = UiScale_;
    const float Pad = 8.0f * Scale;
    const float Rounding = 6.0f * Scale;
    const float StripeWidth = 4.0f * Scale;
    const float MaxImageHeight = 260.0f * Scale;
    const float HeaderHeight = ImGui::GetFrameHeight();
    const float CaptionHeight = ImGui::GetTextLineHeight();

    const Session* const Owner = FindSession(Item.SessionId);
    const ImU32 Tint = UiHelpers::SessionTint(Item.SessionId);
    const float AvailableWidth = Width - StripeWidth - Pad * 2;
    constexpr float DEFAULT_ASPECT = 0.5625f;
    const std::map<std::string, float>::const_iterator KnownAspect = ShotAspects_.find(Item.Path);
    float Aspect = KnownAspect != ShotAspects_.end() ? KnownAspect->second : DEFAULT_ASPECT;

    // Only cards on screen ask the cache for a texture; asking for all of them every frame would
    // thrash the cache and make cards flip between "Loading..." and their image.
    const ImVec2 CardTopLeft = ImGui::GetCursorScreenPos();
    const float EstimatedHeight = HeaderHeight + Pad + std::min(AvailableWidth * Aspect, MaxImageHeight) + Pad * 0.5f + CaptionHeight + Pad;
    const bool OnScreen = ImGui::IsRectVisible(CardTopLeft, ImVec2(CardTopLeft.x + Width, CardTopLeft.y + EstimatedHeight));
    const ImageCache::Texture* const Image = OnScreen == true ? Images_->Get(Item.Path, Item.Time) : nullptr;
    const bool HasImage = Image != nullptr && Image->Failed == false;
    if (Image != nullptr && Image->Failed == true)
    {
        std::error_code ExistsError;
        if (std::filesystem::exists(Item.Path, ExistsError) == false)
        {
            return false;
        }
    }
    if (HasImage == true)
    {
        Aspect = static_cast<float>(Image->Height) / static_cast<float>(Image->Width);
        ShotAspects_[Item.Path] = Aspect;
    }

    const float ImageHeight = std::min(AvailableWidth * Aspect, MaxImageHeight);
    const float ImageWidth = HasImage == true ? ImageHeight / Aspect : AvailableWidth;
    const float CardHeight = HeaderHeight + Pad + ImageHeight + Pad * 0.5f + CaptionHeight + Pad;

    ImGui::PushID(Item.SessionId.c_str());
    ImGui::PushID(Item.Path.c_str());
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const ImVec2 Origin = ImGui::GetCursorScreenPos();
    const ImVec2 CardMax(Origin.x + Width, Origin.y + CardHeight);

    DrawList->AddRectFilled(Origin, CardMax, PaneBgColorU32_, Rounding);
    const bool IsHighlighted = Owner != nullptr && (State_.Ui.ActiveId == Item.SessionId || Owner->CurrentStatus == Status::Waiting);
    const ImU32 BorderColor = Owner != nullptr && Owner->CurrentStatus == Status::Waiting ? WaitingColor_
                              : IsHighlighted == true                                     ? AccentColor_
                                                                                          : UiHelpers::WithAlpha(Tint, 110);
    DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Width, Origin.y + HeaderHeight), UiHelpers::WithAlpha(Tint, IsHighlighted == true ? 110 : 60), Rounding,
                            ImDrawFlags_RoundCornersTop);
    DrawList->AddRectFilled(Origin, ImVec2(Origin.x + StripeWidth, CardMax.y), Tint, Rounding, ImDrawFlags_RoundCornersLeft);
    DrawList->AddRect(Origin, CardMax, BorderColor, Rounding, 0, IsHighlighted == true ? 2.0f * Scale : 1.0f);

    const float CloseWidth = HeaderHeight;
    const std::string AgoText = UiHelpers::FormatAgo(Item.Time);
    const float AgoWidth = ImGui::CalcTextSize(AgoText.c_str()).x + CloseWidth;
    const float TextY = Origin.y + (HeaderHeight - ImGui::GetTextLineHeight()) * 0.5f;
    const float TextLeft = Origin.x + StripeWidth + Pad;
    const float DotRadius = 4.5f * Scale;
    if (Owner != nullptr)
    {
        DrawStatusDot(DrawList, ImVec2(TextLeft + DotRadius, Origin.y + HeaderHeight * 0.5f), DotRadius, *Owner);
    }
    else
    {
        DrawList->AddCircle(ImVec2(TextLeft + DotRadius, Origin.y + HeaderHeight * 0.5f), DotRadius, Tint, 0, 1.5f * Scale);
    }

    const Project* const OwnerProject = Owner != nullptr ? FindProject(Owner->Rec.ProjectId) : nullptr;
    const std::string SessionName = Owner != nullptr ? Owner->Rec.Name : std::string("(closed session)");
    const std::string ProjectName = OwnerProject != nullptr ? OwnerProject->Name : std::string();
    const float NameX = TextLeft + DotRadius * 2 + 6 * Scale;
    DrawList->PushClipRect(ImVec2(NameX, Origin.y), ImVec2(Origin.x + Width - AgoWidth - Pad * 2, CardMax.y), true);
    DrawList->AddText(ImVec2(NameX, TextY), Tint, SessionName.c_str());
    if (ProjectName.empty() == false)
    {
        DrawList->AddText(ImVec2(NameX + ImGui::CalcTextSize(SessionName.c_str()).x + 8 * Scale, TextY), TextDisabledColorU32_, ProjectName.c_str());
    }
    DrawList->PopClipRect();
    DrawList->AddText(ImVec2(Origin.x + Width - AgoWidth - Pad, TextY), TextDisabledColorU32_, AgoText.c_str());

    ImGui::SetCursorScreenPos(Origin);
    if (ImGui::InvisibleButton("##header", ImVec2(Width - CloseWidth, HeaderHeight)) == true && Owner != nullptr)
    {
        OpenSessionFromSidebar(Item.SessionId);
    }
    if (ImGui::IsItemHovered() == true)
    {
        DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Width - CloseWidth, Origin.y + HeaderHeight), IM_COL32(255, 255, 255, 18), Rounding, ImDrawFlags_RoundCornersTopLeft);
        ImGui::SetTooltip(Owner != nullptr ? "Go to this session" : "This session was closed");
    }

    ImGui::SetCursorScreenPos(ImVec2(Origin.x + Width - CloseWidth, Origin.y));
    const bool Dismissed = ImGui::InvisibleButton("##hide", ImVec2(CloseWidth, HeaderHeight));
    const bool CloseHovered = ImGui::IsItemHovered();
    const ImVec2 CloseCenter(Origin.x + Width - CloseWidth * 0.5f, Origin.y + HeaderHeight * 0.5f);
    const float CrossSize = 4.0f * Scale;
    const ImU32 CrossColor = CloseHovered == true ? TextColorU32_ : TextDisabledColorU32_;
    DrawList->AddLine(ImVec2(CloseCenter.x - CrossSize, CloseCenter.y - CrossSize), ImVec2(CloseCenter.x + CrossSize, CloseCenter.y + CrossSize), CrossColor, 1.5f * Scale);
    DrawList->AddLine(ImVec2(CloseCenter.x - CrossSize, CloseCenter.y + CrossSize), ImVec2(CloseCenter.x + CrossSize, CloseCenter.y - CrossSize), CrossColor, 1.5f * Scale);
    if (CloseHovered == true)
    {
        ImGui::SetTooltip("Hide this screenshot");
    }

    const ImVec2 ImageMin(Origin.x + StripeWidth + Pad + (AvailableWidth - ImageWidth) * 0.5f, Origin.y + HeaderHeight + Pad);
    const ImVec2 ImageMax(ImageMin.x + ImageWidth, ImageMin.y + ImageHeight);
    if (HasImage == true)
    {
        DrawList->AddImage(Image->Id, ImageMin, ImageMax);
    }
    else
    {
        DrawList->AddRectFilled(ImageMin, ImageMax, SurfaceColorU32_, 4 * Scale);
        const char* const Message = Image == nullptr ? "Loading..." : "Can't preview this file";
        const ImVec2 MessageSize = ImGui::CalcTextSize(Message);
        DrawList->AddText(ImVec2(ImageMin.x + (ImageWidth - MessageSize.x) * 0.5f, ImageMin.y + (ImageHeight - MessageSize.y) * 0.5f), TextDisabledColorU32_, Message);
    }
    ImGui::SetCursorScreenPos(ImageMin);
    if (ImGui::InvisibleButton("##image", ImVec2(ImageWidth, ImageHeight)) == true)
    {
        Platform::Reveal(Item.Path);
    }
    if (ImGui::IsItemHovered() == true)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("%s\nClick to open", Item.Path.c_str());
    }
    bool DismissedSession = false;
    if (ImGui::BeginPopupContextItem("##imageMenu") == true)
    {
        if (ImGui::MenuItem("Delete all from session") == true)
        {
            DismissedSession = true;
        }
        ImGui::EndPopup();
    }

    const std::string FileName = std::filesystem::path(Item.Path).filename().string();
    DrawList->PushClipRect(ImVec2(ImageMin.x, ImageMax.y), ImVec2(Origin.x + Width - Pad, CardMax.y), true);
    DrawList->AddText(ImVec2(Origin.x + StripeWidth + Pad, ImageMax.y + Pad * 0.5f), TextDisabledColorU32_, FileName.c_str());
    DrawList->PopClipRect();

    ImGui::SetCursorScreenPos(Origin);
    ImGui::Dummy(ImVec2(Width, CardHeight));
    ImGui::PopID();
    ImGui::PopID();
    if (Dismissed == true)
    {
        State_.Ui.HiddenScreenshots.insert(Item.Path);
        MarkDirty();
    }
    if (DismissedSession == true)
    {
        for (const ScreenshotFeed::Entry& Other : ScreenshotFeed_.Entries())
        {
            if (Other.SessionId != Item.SessionId)
            {
                continue;
            }
            State_.Ui.HiddenScreenshots.insert(Other.Path);
        }
        MarkDirty();
    }
    return true;
}

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

    const char* const MaximizeLabel = MaximizedId_ == CurrentSession.Rec.Id ? "Restore" : "Max";
    const float ButtonPad = 5 * Scale * 2;
    const float ItemGap = 6 * Scale;
    const bool HasMaximize = State_.Ui.Mode == "tile";
    const float ButtonsWidth = ImGui::CalcTextSize("Build & Run").x + ImGui::CalcTextSize("Open").x + ImGui::CalcTextSize("Restart").x + ImGui::CalcTextSize("x").x +
                               ButtonPad * (HasMaximize ? 5 : 4) + ItemGap + (HasMaximize ? ImGui::CalcTextSize(MaximizeLabel).x : 0.0f);
    std::string const Shortcut = "Alt+" + std::to_string(Index + 1);
    const float ShortcutWidth = ImGui::CalcTextSize(Shortcut.c_str()).x;
    ImGui::SetCursorPos(ImVec2(Size.x - ButtonsWidth - ShortcutWidth - ItemGap * (HasMaximize ? 4 : 3) - 4 * Scale, (HeaderHeight - ImGui::GetFrameHeight()) * 0.5f));
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
    const float GitButtonsWidth = ImGui::CalcTextSize("Review").x + ImGui::CalcTextSize("Commit").x + ImGui::CalcTextSize("Push").x + GitPad * 3 + ItemGap * 2;
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

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

bool App::DrawBoxChar(ImDrawList* DrawList, float CellX, float CellY, float CellWidth, float CellHeight, uint32_t CodePoint, ImU32 CurrentColor)
{
    // blocks and shades
    if (CodePoint >= 0x2580 && CodePoint <= 0x259F)
    {
        const std::function<void(float, float, float, float, int)> RectWithAlpha = [&](float X0, float Y0, float X1, float Y1, int Alpha)
        {
            ImU32 const RectColor = (CurrentColor & 0x00FFFFFF) | ((ImU32)Alpha << 24);
            DrawList->AddRectFilled(ImVec2(CellX + X0 * CellWidth, CellY + Y0 * CellHeight), ImVec2(CellX + X1 * CellWidth, CellY + Y1 * CellHeight), RectColor);
        };
        const std::function<void(float, float, float, float)> Rect = [&](float X0, float Y0, float X1, float Y1) { RectWithAlpha(X0, Y0, X1, Y1, 255); };
        if (CodePoint == 0x2580)
        {
            Rect(0, 0, 1, 0.5f);
        }
        else if (CodePoint >= 0x2581 && CodePoint <= 0x2587)
        {
            Rect(0, 1.0f - (float)(CodePoint - 0x2580) / 8.0f, 1, 1);
        }
        else if (CodePoint == 0x2588)
        {
            Rect(0, 0, 1, 1);
        }
        else if (CodePoint >= 0x2589 && CodePoint <= 0x258F)
        {
            Rect(0, 0, (float)(0x2590 - CodePoint) / 8.0f, 1);
        }
        else if (CodePoint == 0x2590)
        {
            Rect(0.5f, 0, 1, 1);
        }
        else if (CodePoint == 0x2591)
        {
            RectWithAlpha(0, 0, 1, 1, 64);
        }
        else if (CodePoint == 0x2592)
        {
            RectWithAlpha(0, 0, 1, 1, 128);
        }
        else if (CodePoint == 0x2593)
        {
            RectWithAlpha(0, 0, 1, 1, 192);
        }
        else if (CodePoint == 0x2594)
        {
            Rect(0, 0, 1, 0.125f);
        }
        else if (CodePoint == 0x2595)
        {
            Rect(0.875f, 0, 1, 1);
        }
        else
        {
            return false;  // quadrants fall back to the font
        }
        return true;
    }

    int Left, Right, Up, Down;
    if (CodePoint < 0x2500 || CodePoint > 0x2577 || UiHelpers::BoxArms(CodePoint, Left, Right, Up, Down) == false)
    {
        return false;
    }
    float Thickness = std::max(1.0f, std::round(MonoFontPx_ / 14.0f));
    float CenterX = std::floor(CellX + CellWidth * 0.5f), CenterY = std::floor(CellY + CellHeight * 0.5f);
    const std::function<float(int)> Thick = [&](int Weight) { return Weight == 2 ? Thickness * 2 : Thickness; };

    bool const Rounded = CodePoint >= 0x256D && CodePoint <= 0x2570;
    if (Rounded)
    {
        float const Radius = std::min(CellWidth, CellHeight) * 0.5f;
        DrawList->PathClear();
        const float Pi = 3.14159265f;
        switch (CodePoint)
        {
            case 0x256D:  // down + right
                DrawList->PathLineTo(ImVec2(CenterX, CellY + CellHeight));
                DrawList->PathLineTo(ImVec2(CenterX, CenterY + Radius));
                DrawList->PathArcTo(ImVec2(CenterX + Radius, CenterY + Radius), Radius, Pi, 1.5f * Pi, 8);
                DrawList->PathLineTo(ImVec2(CellX + CellWidth, CenterY));
                break;
            case 0x256E:  // left + down
                DrawList->PathLineTo(ImVec2(CellX, CenterY));
                DrawList->PathLineTo(ImVec2(CenterX - Radius, CenterY));
                DrawList->PathArcTo(ImVec2(CenterX - Radius, CenterY + Radius), Radius, 1.5f * Pi, 2.0f * Pi, 8);
                DrawList->PathLineTo(ImVec2(CenterX, CellY + CellHeight));
                break;
            case 0x256F:  // up + left
                DrawList->PathLineTo(ImVec2(CenterX, CellY));
                DrawList->PathLineTo(ImVec2(CenterX, CenterY - Radius));
                DrawList->PathArcTo(ImVec2(CenterX - Radius, CenterY - Radius), Radius, 0.0f, 0.5f * Pi, 8);
                DrawList->PathLineTo(ImVec2(CellX, CenterY));
                break;
            default:  // 0x2570: right + up
                DrawList->PathLineTo(ImVec2(CellX + CellWidth, CenterY));
                DrawList->PathLineTo(ImVec2(CenterX + Radius, CenterY));
                DrawList->PathArcTo(ImVec2(CenterX + Radius, CenterY - Radius), Radius, 0.5f * Pi, Pi, 8);
                DrawList->PathLineTo(ImVec2(CenterX, CellY));
                break;
        }
        DrawList->PathStroke(CurrentColor, 0, Thickness);
        return true;
    }

    const std::function<void(float, float, int)> HorizontalBar = [&](float X0, float X1, int Weight)
    {
        float const BarThickness = Thick(Weight);
        DrawList->AddRectFilled(ImVec2(X0, CenterY - std::floor(BarThickness * 0.5f)),
                                ImVec2(X1, CenterY - std::floor(BarThickness * 0.5f) + BarThickness), CurrentColor);
    };
    const std::function<void(float, float, int)> VerticalBar = [&](float Y0, float Y1, int Weight)
    {
        float const BarThickness = Thick(Weight);
        DrawList->AddRectFilled(ImVec2(CenterX - std::floor(BarThickness * 0.5f), Y0),
                                ImVec2(CenterX - std::floor(BarThickness * 0.5f) + BarThickness, Y1), CurrentColor);
    };
    float JoinX = std::ceil(Thick(std::max(Up, Down)) * 0.5f), JoinY = std::ceil(Thick(std::max(Left, Right)) * 0.5f);
    if (Left != 0)
    {
        HorizontalBar(CellX, CenterX + JoinX, Left);
    }
    if (Right != 0)
    {
        HorizontalBar(CenterX - JoinX, CellX + CellWidth, Right);
    }
    if (Up != 0)
    {
        VerticalBar(CellY, CenterY + JoinY, Up);
    }
    if (Down != 0)
    {
        VerticalBar(CenterY - JoinY, CellY + CellHeight, Down);
    }
    return true;
}

void App::RunFind(Session& Target, bool Forward, bool KeepCurrent)
{
    Terminal const& Term = *Target.Term;
    const std::vector<uint32_t> Query = Terminal::FoldQuery(Target.FindBuffer);
    Target.FindNoMatch = false;
    if (Query.empty() == true)
    {
        Target.FindHasMatch = false;
        return;
    }

    int FromRow = Term.ScrollbackSize() - Target.ScrollOffset;
    int FromCol = Forward ? -1 : Term.Cols();
    if (Target.FindHasMatch == true)
    {
        FromRow = Term.AbsRow(Target.FindStableRow);
        FromCol = KeepCurrent ? Target.FindCol - 1 : Target.FindCol;
    }

    int MatchRow = 0;
    int MatchCol = 0;
    if (Term.Find(Query, FromRow, FromCol, Forward, MatchRow, MatchCol) == false)
    {
        Target.FindHasMatch = false;
        Target.FindNoMatch = true;
        return;
    }
    Target.FindHasMatch = true;
    Target.FindStableRow = Term.StableRow(MatchRow);
    Target.FindCol = MatchCol;

    const int ViewTop = Term.ScrollbackSize() - Target.ScrollOffset;
    if (MatchRow < ViewTop || MatchRow >= ViewTop + Term.Rows())
    {
        Target.ScrollOffset = std::clamp(Term.ScrollbackSize() - (MatchRow - Term.Rows() / 2), 0, Term.ScrollbackSize());
    }
}

void App::DrawFindBar(Session& Target, ImVec2 PanelPos, ImVec2 PanelSize)
{
    ImGuiIO const& Io = ImGui::GetIO();
    ImDrawList* const DrawList = ImGui::GetWindowDrawList();
    const float Scale = UiScale_;
    const float ButtonSize = ImGui::GetFrameHeight();
    const float Gap = 2.0f * Scale;
    const float Pad = 4.0f * Scale;
    const float BarWidth = std::min(PanelSize.x - 24.0f * Scale, 360.0f * Scale);
    const ImVec2 BarMin(PanelPos.x + PanelSize.x - BarWidth - 16.0f * Scale, PanelPos.y + 6.0f * Scale);
    const ImVec2 BarMax(BarMin.x + BarWidth, BarMin.y + ButtonSize + Pad * 2.0f);

    DrawList->AddRectFilled(BarMin, BarMax, PaneBgColorU32_, 4.0f * Scale);
    DrawList->AddRect(BarMin, BarMax, PaneBorderColorU32_, 4.0f * Scale);
    if (Target.FindNoMatch == true)
    {
        const char* const Message = "No matches";
        const ImVec2 MessageSize = ImGui::CalcTextSize(Message);
        DrawList->AddText(ImVec2(BarMin.x - MessageSize.x - 8.0f * Scale, BarMin.y + (BarMax.y - BarMin.y - MessageSize.y) * 0.5f),
                          TextDisabledColorU32_, Message);
    }

    const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
    ImGui::PushID(Target.Rec.Id.c_str());
    ImGui::PushID("find");
    ImGui::SetCursorScreenPos(ImVec2(BarMin.x + Pad, BarMin.y + Pad));
    ImGui::SetNextItemWidth(BarWidth - Pad * 2.0f - (ButtonSize + Gap) * 3.0f);
    if (Target.FindFocus == true)
    {
        ImGui::SetKeyboardFocusHere();
        Target.FindFocus = false;
    }
    const bool Edited = ImGui::InputText("##query", Target.FindBuffer, sizeof Target.FindBuffer, ImGuiInputTextFlags_AutoSelectAll);
    const bool InputActive = ImGui::IsItemActive();
    const bool InputFinished = InputActive == true || ImGui::IsItemDeactivated() == true;

    bool Close = false;
    bool GoNext = false;
    bool GoPrev = false;
    if (InputActive == true && ImGui::IsKeyPressed(ImGuiKey_Enter) == true)
    {
        GoNext = Io.KeyShift == false;
        GoPrev = Io.KeyShift;
    }
    if (InputFinished == true && ImGui::IsKeyPressed(ImGuiKey_Escape) == true)
    {
        Close = true;
    }
    if (InputActive == true && Io.KeyCtrl == true && ImGui::IsKeyPressed(ImGuiKey_F) == true)
    {
        Target.FindFocus = true;
    }

    ImGui::SameLine(0, Gap);
    GoPrev = ImGui::Button("^", ImVec2(ButtonSize, ButtonSize)) || GoPrev;
    ImGui::SameLine(0, Gap);
    GoNext = ImGui::Button("v", ImVec2(ButtonSize, ButtonSize)) || GoNext;
    ImGui::SameLine(0, Gap);
    Close = ImGui::Button("x", ImVec2(ButtonSize, ButtonSize)) || Close;
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(SavedCursor);

    if (Close == true)
    {
        Target.FindOpen = false;
        Target.FindHasMatch = false;
        Target.FindNoMatch = false;
        return;
    }
    if (Edited == true)
    {
        RunFind(Target, true, true);
        return;
    }
    if (GoNext == true || GoPrev == true)
    {
        RunFind(Target, GoNext, false);
    }
}

void App::DrawTerminal(Session& CurrentSession, ImVec2 Size, bool IsActive)
{
    ImGuiIO& Io = ImGui::GetIO();
    ImDrawList* DrawList = ImGui::GetWindowDrawList();
    ImVec2 PanelPos = ImGui::GetCursorScreenPos();
    const float Padding = 6 * UiScale_;
    const int Cols = std::max(10, (int)((Size.x - Padding * 2) / CellSize_.x));
    const int Rows = std::max(3, (int)((Size.y - 2) / CellSize_.y));

    if (CurrentSession.Attempted == false)
    {
        CurrentSession.Attempted = true;
        std::string Err;
        if (CurrentSession.Start(State_.AppSettings.ClaudeCommand, Cols, Rows, Now_, Err) == false)
        {
            ShowToast("Could not launch Claude: " + Err, true);
        }
        MarkDirty();
    }
    if (CurrentSession.Term)
    {
        CurrentSession.RequestResize(Cols, Rows, Now_);
    }

    DrawList->AddRectFilled(PanelPos, ImVec2(PanelPos.x + Size.x, PanelPos.y + Size.y), TerminalBgColorU32_);

    // Submitted before the terminal button: the first-submitted item under the mouse claims the press,
    // so the scrollbar must come first or the terminal would start a text selection instead.
    const bool ShowScrollbar = CurrentSession.Term != nullptr && CurrentSession.Term->ScrollbackSize() > 0 && CurrentSession.Term->AltScreen() == false;
    const float TrackWidth = 18.0f * UiScale_;
    const float TrackInset = 8.0f * UiScale_;
    const float TrackLeft = PanelPos.x + Size.x - TrackWidth - 4.0f * UiScale_;
    const float TrackTop = PanelPos.y + TrackInset;
    const float TrackHeight = std::max(1.0f, Size.y - TrackInset * 2.0f);
    float BarHeight = 0.0f;
    bool BarHovered = false;
    bool BarActive = false;
    if (ShowScrollbar == true)
    {
        const int HistoryRows = CurrentSession.Term->ScrollbackSize();
        const int ScreenRows = CurrentSession.Term->Rows();
        BarHeight = std::max(32.0f * UiScale_, TrackHeight * ScreenRows / (float)(HistoryRows + ScreenRows));
        const float Travel = std::max(1.0f, TrackHeight - BarHeight);

        ImGui::SetCursorScreenPos(ImVec2(TrackLeft, TrackTop));
        ImGui::InvisibleButton("##scrollbar", ImVec2(TrackWidth, TrackHeight));
        BarHovered = ImGui::IsItemHovered();
        BarActive = ImGui::IsItemActive();
        if (BarHovered == true || BarActive == true)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        }
        if (BarActive == true)
        {
            const float Fraction = std::clamp((Io.MousePos.y - TrackTop - BarHeight * 0.5f) / Travel, 0.0f, 1.0f);
            CurrentSession.ScrollOffset = HistoryRows - static_cast<int>(std::round(Fraction * HistoryRows));
        }
        ImGui::SetCursorScreenPos(PanelPos);
    }

    // Submitted before the terminal button for the same reason as the scrollbar: otherwise the terminal swallows the click.
    const bool ShowResume = CurrentSession.CurrentStatus == Status::Exited && CurrentSession.Attempted == true;
    const float ResumeWidth = 170 * UiScale_;
    const ImVec2 ResumeMin(PanelPos.x + (Size.x - ResumeWidth) * 0.5f, PanelPos.y + Size.y * 0.5f + 4);
    const ImVec2 ResumeSize(ResumeWidth, ImGui::GetFrameHeight());
    bool ResumeClicked = false;
    bool ResumeHovered = false;
    if (ShowResume == true)
    {
        ImGui::SetCursorScreenPos(ResumeMin);
        ImGui::PushID(("resume" + CurrentSession.Rec.Id).c_str());
        ResumeClicked = ImGui::InvisibleButton("##resume", ResumeSize);
        ResumeHovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(PanelPos);
    }

    ImGui::InvisibleButton("##term", Size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool Hovered = ImGui::IsItemHovered();
    if (CurrentSession.FindOpen == true && (IsActive == false || ImGui::IsItemClicked(ImGuiMouseButton_Left) == true || CurrentSession.Term == nullptr))
    {
        CurrentSession.FindOpen = false;
        CurrentSession.FindHasMatch = false;
        CurrentSession.FindNoMatch = false;
    }

    if (CurrentSession.Term == nullptr)
    {
        DrawList->AddText(ImVec2(PanelPos.x + Padding, PanelPos.y + Padding), TextDisabledColorU32_, "Session not started.");
    }
    else
    {
        Terminal& CurrentTerminal = *CurrentSession.Term;
        const int ScrollbackSize = CurrentTerminal.ScrollbackSize();
        // keep the view anchored while output scrolls into history
        long long const PushedDelta = CurrentTerminal.PushedTotal() - CurrentSession.LastPushed;
        CurrentSession.LastPushed = CurrentTerminal.PushedTotal();
        if (CurrentSession.ScrollOffset > 0 && PushedDelta > 0)
        {
            CurrentSession.ScrollOffset += (int)PushedDelta;
        }
        CurrentSession.ScrollOffset = std::clamp(CurrentSession.ScrollOffset, 0, ScrollbackSize);
        const int ViewTop = ScrollbackSize - CurrentSession.ScrollOffset;
        const bool Reporting = CurrentTerminal.MouseMode() > 0 && Io.KeyShift == false;

        const std::function<void(ImVec2, int&, int&)> CellAt = [&](ImVec2 MousePos, int& OutRow, int& OutCol)
        {
            OutCol = std::clamp((int)std::floor((MousePos.x - (PanelPos.x + Padding)) / CellSize_.x), 0, CurrentTerminal.Cols() - 1);
            OutRow = std::clamp((int)std::floor((MousePos.y - PanelPos.y) / CellSize_.y), 0, CurrentTerminal.Rows() - 1);
        };

        // ---- mouse ----
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            int Row = 0, Col = 0;
            CellAt(Io.MousePos, Row, Col);
            if (Reporting)
            {
                CurrentSession.Typing(Now_, StatusCallback(),
                               [&](Terminal& Term)
                               {
                                   Term.SendMouseMove(Row, Col, VTERM_MOD_NONE);
                                   Term.SendMouseButton(1, true, VTERM_MOD_NONE);
                               });
            }
            else
            {
                CurrentSession.Selecting = true;
                CurrentSession.HasSelection = false;
                CurrentSession.InputSelected = false;
                CurrentSession.SelectionStartRow = CurrentSession.SelectionEndRow = CurrentTerminal.StableRow(ViewTop + Row);
                CurrentSession.SelectionStartCol = CurrentSession.SelectionEndCol = Col;
            }
        }
        if (CurrentSession.Selecting && (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
        {
            int Row, Col;
            CellAt(Io.MousePos, Row, Col);
            CurrentSession.SelectionEndRow = CurrentTerminal.StableRow(ViewTop + Row);
            CurrentSession.SelectionEndCol = Col;
            CurrentSession.HasSelection = (CurrentSession.SelectionStartRow == CurrentSession.SelectionEndRow && CurrentSession.SelectionStartCol == CurrentSession.SelectionEndCol) == false;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (CurrentSession.Selecting)
            {
                CurrentSession.Selecting = false;
            }
            else if (Reporting && Hovered)
            {
                int Row = 0, Col = 0;
                CellAt(Io.MousePos, Row, Col);
                CurrentSession.Typing(Now_, StatusCallback(),
                               [&](Terminal& Term)
                               {
                                   Term.SendMouseMove(Row, Col, VTERM_MOD_NONE);
                                   Term.SendMouseButton(1, false, VTERM_MOD_NONE);
                               });
            }
        }
        if (Hovered && Reporting == false && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            if (CurrentSession.HasSelection)
            {
                CopySelection(CurrentSession);
            }
            else
            {
                Paste(CurrentSession);
            }
        }
        if (Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
        {
            Paste(CurrentSession);
        }
        if (Hovered && Io.MouseWheel != 0.0f && Io.KeyCtrl == false)
        {
            if (Reporting)
            {
                int Row = 0, Col = 0;
                CellAt(Io.MousePos, Row, Col);
                int Steps = std::max(1, (int)std::abs(Io.MouseWheel * 3));
                CurrentSession.Typing(Now_, StatusCallback(),
                               [&](Terminal& Term)
                               {
                                   Term.SendMouseMove(Row, Col, VTERM_MOD_NONE);
                                   for (int Step = 0; Step < Steps; Step++)
                                   {
                                       Term.SendMouseButton(Io.MouseWheel > 0 ? 4 : 5, true, VTERM_MOD_NONE);
                                   }
                               });
            }
            else if (CurrentTerminal.AltScreen() == false)
            {
                CurrentSession.ScrollOffset = std::clamp(CurrentSession.ScrollOffset + (int)std::round(Io.MouseWheel * 3), 0, ScrollbackSize);
            }
        }
        if (Hovered)
        {
            ImGui::SetMouseCursor(Reporting ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_TextInput);
        }

        // ---- selection range (absolute rows) ----
        int SelRowA = 0, SelColA = 0, SelRowB = -1, SelColB = 0;
        if (CurrentSession.HasSelection)
        {
            SelRowA = CurrentTerminal.AbsRow(CurrentSession.SelectionStartRow);
            SelRowB = CurrentTerminal.AbsRow(CurrentSession.SelectionEndRow);
            SelColA = CurrentSession.SelectionStartCol;
            SelColB = CurrentSession.SelectionEndCol;
            if (SelRowA > SelRowB || (SelRowA == SelRowB && SelColA > SelColB))
            {
                std::swap(SelRowA, SelRowB);
                std::swap(SelColA, SelColB);
            }
        }

        // ---- cells ----
        const float FontSize = MonoFont_->FontSize;
        const float OriginX = PanelPos.x + Padding;
        const float TextOffsetY = (CellSize_.y - FontSize) * 0.5f;
        int CursorRow = 0, CursorCol = 0;
        bool CursorVisible = false;
        CurrentTerminal.GetCursor(CursorRow, CursorCol, CursorVisible);
        std::vector<Cell> RowCells((size_t)CurrentTerminal.Cols());
        const std::vector<uint32_t> FindQuery = CurrentSession.FindOpen == true ? Terminal::FoldQuery(CurrentSession.FindBuffer) : std::vector<uint32_t>{};
        const int FindLength = static_cast<int>(FindQuery.size());
        const int CurrentMatchRow = CurrentSession.FindHasMatch == true ? CurrentTerminal.AbsRow(CurrentSession.FindStableRow) : -1;
        std::vector<int> MatchStarts;
        std::vector<char> Highlight(static_cast<size_t>(CurrentTerminal.Cols()));

        DrawList->PushClipRect(PanelPos, ImVec2(PanelPos.x + Size.x, PanelPos.y + Size.y), true);
        for (int Row = 0; Row < CurrentTerminal.Rows(); Row++)
        {
            const int AbsRow = ViewTop + Row;
            const float RowY = PanelPos.y + Row * CellSize_.y;
            if (RowY > PanelPos.y + Size.y)
            {
                break;
            }
            for (int Col = 0; Col < CurrentTerminal.Cols(); Col++)
            {
                RowCells[(size_t)Col] = CurrentTerminal.GetCell(AbsRow, Col);
            }

            if (FindLength > 0)
            {
                std::fill(Highlight.begin(), Highlight.end(), static_cast<char>(0));
                Terminal::MatchColumns(RowCells, FindQuery, MatchStarts);
                for (const int Start : MatchStarts)
                {
                    const char Kind = (AbsRow == CurrentMatchRow && Start == CurrentSession.FindCol) ? 2 : 1;
                    std::fill(Highlight.begin() + Start, Highlight.begin() + Start + FindLength, Kind);
                }
            }

            // backgrounds (+ selection)
            int RunStart = 0;
            ImU32 RunColor = 0;
            bool RunActive = false;
            for (int Col = 0; Col <= CurrentTerminal.Cols();)
            {
                ImU32 CellColor = 0;
                bool CellPainted = false;
                int Span = 1;
                if (Col < CurrentTerminal.Cols())
                {
                    const Cell& CurrentCell = RowCells[(size_t)Col];
                    Span = CurrentCell.Width > 1 ? CurrentCell.Width : 1;
                    if (FindLength > 0 && Highlight[(size_t)Col] != 0)
                    {
                        CellColor = UiHelpers::WithAlpha(AccentColor_, Highlight[(size_t)Col] == 2 ? 230 : 110);
                        CellPainted = true;
                    }
                    else if (CurrentSession.HasSelection == true &&
                             ((AbsRow > SelRowA && AbsRow < SelRowB) || (AbsRow == SelRowA && AbsRow == SelRowB && Col >= SelColA && Col <= SelColB) ||
                              (AbsRow == SelRowA && AbsRow != SelRowB && Col >= SelColA) || (AbsRow == SelRowB && AbsRow != SelRowA && Col <= SelColB)))
                    {
                        CellColor = SelectionColorU32_;
                        CellPainted = true;
                    }
                    else if (CurrentCell.DefBg == false)
                    {
                        CellColor = UiHelpers::RgbCol(CurrentCell.Bg);
                        CellPainted = true;
                    }
                }
                if (RunActive == true && (CellPainted == false || CellColor != RunColor))
                {
                    DrawList->AddRectFilled(ImVec2(OriginX + RunStart * CellSize_.x, RowY), ImVec2(OriginX + Col * CellSize_.x, RowY + CellSize_.y), RunColor);
                    RunActive = false;
                }
                if (CellPainted == true && RunActive == false)
                {
                    RunStart = Col;
                    RunColor = CellColor;
                    RunActive = true;
                }
                Col += Span;
            }
            if (RunActive == true)
            {
                DrawList->AddRectFilled(ImVec2(OriginX + RunStart * CellSize_.x, RowY),
                                        ImVec2(OriginX + CurrentTerminal.Cols() * CellSize_.x, RowY + CellSize_.y), RunColor);
            }

            // cursor block
            if (CursorVisible && CurrentSession.ScrollOffset == 0 && Row == CursorRow && CurrentSession.IsRunning())
            {
                ImVec2 CursorMin(OriginX + CursorCol * CellSize_.x, RowY), CursorMax(OriginX + (CursorCol + 1) * CellSize_.x, RowY + CellSize_.y);
                if (IsActive && Focused_)
                {
                    DrawList->AddRectFilled(CursorMin, CursorMax, CursorFillColorU32_);
                }
                else
                {
                    DrawList->AddRect(CursorMin, CursorMax, CursorOutlineColorU32_);
                }
            }

            // text - each glyph is drawn on its own, pinned to its exact grid column. A bold font's glyphs
            // are usually a little wider than the regular font's, so batching same-colored characters into
            // one AddText() call (trusting the font's own cumulative advance) let a bold word overflow its
            // allotted cells and bleed into whatever was drawn next on the row.
            for (int Col = 0; Col < CurrentTerminal.Cols(); Col++)
            {
                const Cell& CurrentCell = RowCells[(size_t)Col];
                if (CurrentCell.Ch == Cell::CONTINUATION)
                {
                    continue;
                }
                const bool IsBlank = CurrentCell.Ch == 0 || CurrentCell.Ch == ' ';
                if (CurrentCell.Underline || CurrentCell.Strike)
                {
                    float LineX0 = OriginX + Col * CellSize_.x, LineX1 = LineX0 + CellSize_.x * (CurrentCell.Width > 1 ? CurrentCell.Width : 1);
                    if (CurrentCell.Underline)
                    {
                        DrawList->AddLine(ImVec2(LineX0, RowY + CellSize_.y - 2), ImVec2(LineX1, RowY + CellSize_.y - 2), UiHelpers::RgbCol(CurrentCell.Fg), 1.0f);
                    }
                    if (CurrentCell.Strike)
                    {
                        DrawList->AddLine(ImVec2(LineX0, RowY + CellSize_.y * 0.5f), ImVec2(LineX1, RowY + CellSize_.y * 0.5f), UiHelpers::RgbCol(CurrentCell.Fg), 1.0f);
                    }
                }
                if (IsBlank)
                {
                    continue;
                }
                const float GlyphX = OriginX + Col * CellSize_.x;
                ImFont* const CurrentFont = (CurrentCell.Bold == true && MonoBoldFont_ != nullptr && CurrentCell.Ch <= 0xFF) ? MonoBoldFont_ : MonoFont_;
                if (CurrentCell.Ch < 0x80)
                {
                    const char Ascii = (char)CurrentCell.Ch;
                    DrawList->AddText(CurrentFont, FontSize, ImVec2(GlyphX, RowY + TextOffsetY), UiHelpers::RgbCol(CurrentCell.Fg), &Ascii, &Ascii + 1);
                    continue;
                }
                if (DrawBoxChar(DrawList, GlyphX, RowY, CellSize_.x * (CurrentCell.Width > 1 ? CurrentCell.Width : 1), CellSize_.y, CurrentCell.Ch, UiHelpers::RgbCol(CurrentCell.Fg)))
                {
                    continue;
                }
                if (CurrentCell.Ch > 0xFFFF)
                {
                    continue;  // outside the atlas (emoji)
                }
                std::string const Glyph = Util::Utf8(CurrentCell.Ch);
                DrawList->AddText(CurrentFont, FontSize, ImVec2(GlyphX, RowY + TextOffsetY), UiHelpers::RgbCol(CurrentCell.Fg), Glyph.c_str(), Glyph.c_str() + Glyph.size());
            }
        }

        // scrollback indicator
        if (ShowScrollbar == true)
        {
            const float Travel = std::max(1.0f, TrackHeight - BarHeight);
            const int ShownViewTop = ScrollbackSize - CurrentSession.ScrollOffset;
            const float BarY0 = TrackTop + (float)ShownViewTop / (float)ScrollbackSize * Travel;
            const int BarAlpha = (BarActive == true) ? 190 : (BarHovered == true ? 140 : (CurrentSession.ScrollOffset > 0 ? 100 : 65));
            const float BarWidth = (BarHovered == true || BarActive == true) ? 10.0f * UiScale_ : 7.0f * UiScale_;
            const float BarRight = TrackLeft + TrackWidth - 2.0f * UiScale_;
            if (BarHovered == true || BarActive == true)
            {
                DrawList->AddRectFilled(ImVec2(TrackLeft, TrackTop), ImVec2(TrackLeft + TrackWidth, TrackTop + TrackHeight),
                                        UiHelpers::WithAlpha(IM_COL32(0xC9, 0xCF, 0xDB, 255), 20), 4);
            }
            DrawList->AddRectFilled(ImVec2(BarRight - BarWidth, BarY0), ImVec2(BarRight, BarY0 + BarHeight), UiHelpers::WithAlpha(IM_COL32(0xC9, 0xCF, 0xDB, 255), BarAlpha), 4);
        }
        if (CurrentSession.ScrollOffset > 0 && ScrollbackSize > 0)
        {
            const char* const Hint = "scrolled back - type to return";
            ImVec2 const HintSize = ImGui::CalcTextSize(Hint);
            DrawList->AddRectFilled(ImVec2(PanelPos.x + Size.x - HintSize.x - 22, PanelPos.y + Size.y - HintSize.y - 12),
                                    ImVec2(PanelPos.x + Size.x - 10, PanelPos.y + Size.y - 6), IM_COL32(0, 0, 0, 170), 4);
            DrawList->AddText(ImVec2(PanelPos.x + Size.x - HintSize.x - 16, PanelPos.y + Size.y - HintSize.y - 9),
                              IM_COL32(0xC9, 0xCF, 0xDB, 255), Hint);
        }
        DrawList->PopClipRect();
        if (CurrentSession.FindOpen == true)
        {
            DrawFindBar(CurrentSession, PanelPos, Size);
        }
    }

    // ---- "not running" overlay ----
    if (ShowResume == true)
    {
        DrawList->AddRectFilled(PanelPos, ImVec2(PanelPos.x + Size.x, PanelPos.y + Size.y), IM_COL32(11, 13, 17, 215));
        const char* const Message = "This session is not running.";
        ImVec2 const TextSize = ImGui::CalcTextSize(Message);
        ImGui::SetCursorScreenPos(ImVec2(PanelPos.x + (Size.x - TextSize.x) * 0.5f, PanelPos.y + Size.y * 0.5f - TextSize.y - 10));
        ImGui::TextDisabled("%s", Message);

        const char* const Label = "Resume session";
        const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
        ImVec4 ButtonColor = AccentColorV4_;
        if (ResumeHovered == true)
        {
            ButtonColor = ImVec4(ButtonColor.x * 1.12f, ButtonColor.y * 1.12f, ButtonColor.z * 1.12f, ButtonColor.w);
        }
        DrawList->AddRectFilled(ResumeMin, ImVec2(ResumeMin.x + ResumeSize.x, ResumeMin.y + ResumeSize.y), ImGui::ColorConvertFloat4ToU32(ButtonColor), ImGui::GetStyle().FrameRounding);
        DrawList->AddText(ImVec2(ResumeMin.x + (ResumeSize.x - LabelSize.x) * 0.5f, ResumeMin.y + (ResumeSize.y - LabelSize.y) * 0.5f),
                          IM_COL32(26, 15, 10, 255), Label);

        if (ResumeClicked == true)
        {
            std::string Err;
            if (CurrentSession.Start(State_.AppSettings.ClaudeCommand, Cols, Rows, Now_, Err) == false)
            {
                ShowToast("Could not launch Claude: " + Err, true);
            }
            MarkDirty();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// modals + toasts
// ---------------------------------------------------------------------------------------------

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

void App::DrawModals()
{
    if (Modal_ == Modal::None)
    {
        return;
    }

    const char* Title = "";
    switch (Modal_)
    {
        case Modal::AddProject: Title = "Add project"; break;
        case Modal::NewSession: Title = "New Claude session"; break;
        case Modal::Rename: Title = "Rename"; break;
        case Modal::Confirm: Title = ConfirmTitle_.c_str(); break;
        case Modal::CloseSession: Title = "Close session"; break;
        case Modal::Settings: Title = "Settings"; break;
        case Modal::Usage: Title = "Usage"; break;
        case Modal::GitHub: Title = "GitHub"; break;
        case Modal::Search: Title = "Search"; break;
        case Modal::Review: Title = "GitHub review"; break;
        default: break;
    }
    std::string const PopupId = std::string(Title) + "###modal";
    if (ModalJustOpened_)
    {
        ImGui::OpenPopup(PopupId.c_str());
        ModalJustOpened_ = false;
        ModalError_.clear();
        if (Modal_ == Modal::NewSession)
        {
            NameBuffer_[0] = 0;
            WantWorktree_ = false;
            WantContinue_ = false;
            ProjectComboIndex_ = 0;
            for (size_t Index = 0; Index < State_.Projects.size(); Index++)
            {
                if (State_.Projects[Index].Id == ModalTarget_)
                {
                    ProjectComboIndex_ = (int)Index;
                }
            }
            GitCache_ = GitCache{};
        }
        if (Modal_ == Modal::Usage)
        {
            StartUsageFetch();
        }
    }

    const float Scale = UiScale_;
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    const bool Resizable = Modal_ == Modal::Settings || Modal_ == Modal::Review;
    if (Resizable == true)
    {
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        const float ModalWidth = Modal_ == Modal::Review ? 1000.0f : 560.0f;
        ImGui::SetNextWindowSize(ImVec2(std::min(ModalWidth * Scale, Viewport->WorkSize.x * 0.95f), std::min(720 * Scale, Viewport->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(460 * Scale, 320 * Scale), ImVec2(Viewport->WorkSize.x, Viewport->WorkSize.y));
    }
    else
    {
        ImGui::SetNextWindowPos(Viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2((Modal_ == Modal::Search ? 620 : 460) * Scale, 0));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * Scale, 14 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * Scale, 9 * Scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * Scale);
    bool Open = true;
    const ImGuiWindowFlags ModalFlags = ImGuiWindowFlags_NoSavedSettings | (Resizable == true ? 0 : ImGuiWindowFlags_NoResize);
    if (ImGui::BeginPopupModal(PopupId.c_str(), &Open, ModalFlags))
    {
        const std::function<void()> CloseModal = [&]
        {
            ImGui::CloseCurrentPopup();
            Modal_ = Modal::None;
        };
        const bool EscapeBelongsToCapture = KeyCaptureIndex_ >= 0 || ImGui::GetFrameCount() == KeyCaptureEscapeFrame_;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && EscapeBelongsToCapture == false)
        {
            CloseModal();
        }
        const float Full = -1;
        const bool Enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

        switch (Modal_)
        {
            case Modal::AddProject:
            {
                ImGui::TextDisabled("Project folder");
                ImGui::SetNextItemWidth(Platform::CanPickFolder() ? -80 * Scale : Full);
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::InputText("##path", PathBuffer_, sizeof PathBuffer_);
                if (Platform::CanPickFolder())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Browse..."))
                    {
                        std::string const Picked = Platform::PickFolder(Window_);
                        if (Picked.empty() == false)
                        {
                            std::snprintf(PathBuffer_, sizeof PathBuffer_, "%s", Picked.c_str());
                        }
                    }
                }
                ImGui::TextDisabled("You can also drag a folder onto the window.");
                if (ModalError_.empty() == false)
                {
                    ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", ModalError_.c_str());
                }
                if (ImGui::Button("Add project") || Enter)
                {
                    std::string Id;
                    if (AddProjectPath(PathBuffer_, ModalError_, &Id))
                    {
                        State_.Ui.Collapsed[Id] = false;
                        ModalTarget_ = Id;
                        OpenModal(Modal::NewSession);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::NewSession:
            {
                if (State_.Projects.empty())
                {
                    CloseModal();
                    break;
                }
                ProjectComboIndex_ = std::clamp(ProjectComboIndex_, 0, (int)State_.Projects.size() - 1);
                ImGui::TextDisabled("Project");
                ImGui::SetNextItemWidth(Full);
                if (ImGui::BeginCombo("##proj", State_.Projects[(size_t)ProjectComboIndex_].Name.c_str()))
                {
                    for (int Index = 0; Index < (int)State_.Projects.size(); Index++)
                    {
                        if (ImGui::Selectable(State_.Projects[(size_t)Index].Name.c_str(), Index == ProjectComboIndex_))
                        {
                            ProjectComboIndex_ = Index;
                        }
                    }
                    ImGui::EndCombo();
                }
                const Project& CurrentProject = State_.Projects[(size_t)ProjectComboIndex_];
                if (GitCache_.Path != CurrentProject.Path)
                {
                    Git::Info const CurrentInfo = Git::GetInfo(CurrentProject.Path);
                    GitCache_ = {CurrentProject.Path, CurrentInfo.IsGitRepo, CurrentInfo.Branch};
                    if (CurrentInfo.IsGitRepo == false)
                    {
                        WantWorktree_ = false;
                    }
                }
                ImGui::TextDisabled("Session name (optional)");
                ImGui::SetNextItemWidth(Full);
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::InputTextWithHint("##name", "e.g. Fix login bug", NameBuffer_, sizeof NameBuffer_);
                ImGui::BeginDisabled(GitCache_.IsGitRepo == false);
                ImGui::Checkbox("Isolate in a new git worktree", &WantWorktree_);
                ImGui::EndDisabled();
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", GitCache_.IsGitRepo
                                              ? ("Branches from " + (GitCache_.Branch.empty() ? std::string("HEAD") : GitCache_.Branch) +
                                                 " into .claude/worktrees/, so parallel sessions never touch each other's files.")
                                                    .c_str()
                                              : "Not a git repository - worktrees unavailable.");
                ImGui::PopTextWrapPos();
                if (WantWorktree_ == true)
                {
                    WantContinue_ = false;
                }
                ImGui::BeginDisabled(WantWorktree_ == true);
                ImGui::Checkbox("Continue the last conversation in this folder", &WantContinue_);
                ImGui::EndDisabled();
                if (ModalError_.empty() == false)
                {
                    ImGui::TextColored(ImVec4(0.94f, 0.36f, 0.36f, 1), "%s", ModalError_.c_str());
                }
                if (ImGui::Button("Start session") || Enter)
                {
                    std::string Err;
                    Session* const NewSession = CreateSession(CurrentProject.Id, NameBuffer_, WantWorktree_ && GitCache_.IsGitRepo, WantContinue_, Err);
                    if (NewSession != nullptr)
                    {
                        State_.Ui.Collapsed[CurrentProject.Id] = false;
                        OpenSession(NewSession->Rec.Id);
                        CloseModal();
                    }
                    else
                    {
                        ModalError_ = Err;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::Rename:
            {
                ImGui::TextDisabled(RenameIsProject_ ? "Project name" : "Session name");
                ImGui::SetNextItemWidth(Full);
                if (ImGui::IsWindowAppearing())
                {
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::InputText("##rename", NameBuffer_, sizeof NameBuffer_);
                if (ImGui::Button("Save") || Enter)
                {
                    std::string const Name = Util::Trim(NameBuffer_);
                    if (Name.empty() == false)
                    {
                        if (RenameIsProject_)
                        {
                            if (Project* const CurrentProject = FindProject(ModalTarget_))
                            {
                                CurrentProject->Name = Name;
                            }
                        }
                        else if (Session* const CurrentSession = FindSession(ModalTarget_))
                        {
                            CurrentSession->Rec.Name = Name;
                        }
                        MarkDirty();
                    }
                    CloseModal();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::Confirm:
            {
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(ConfirmText_.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.24f, 1));
                if (ImGui::Button("Confirm"))
                {
                    std::function<void()> const YesAction = ConfirmYesAction_;
                    CloseModal();
                    if (YesAction)
                    {
                        YesAction();
                    }
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::CloseSession:
            {
                Session* const CurrentSession = FindSession(ModalTarget_);
                if (CurrentSession == nullptr || CurrentSession->Rec.WorktreeInfo.has_value() == false)
                {
                    CloseModal();
                    break;
                }
                ImGui::PushTextWrapPos(0);
                ImGui::Text("\"%s\" works in its own git worktree on branch %s.", CurrentSession->Rec.Name.c_str(),
                           CurrentSession->Rec.WorktreeInfo->Branch.c_str());
                if (CloseDirty_)
                {
                    ImGui::TextColored(ImVec4(0.9f, 0.64f, 0.24f, 1), "The worktree has uncommitted changes.");
                }
                ImGui::PopTextWrapPos();
                ImGui::RadioButton("Keep the worktree and branch", &CloseChoice_, 0);
                ImGui::BeginDisabled(CloseDirty_);
                ImGui::RadioButton("Remove the worktree (branch kept if it has unmerged commits)", &CloseChoice_, 1);
                ImGui::EndDisabled();
                ImGui::RadioButton("Remove and discard all changes", &CloseChoice_, 2);
                if (CloseDirty_ && CloseChoice_ == 1)
                {
                    CloseChoice_ = 0;
                }
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.24f, 1));
                if (ImGui::Button("Close session"))
                {
                    std::string const Id = CurrentSession->Rec.Id;
                    int const Choice = CloseChoice_;
                    CloseModal();
                    CloseSession(Id, Choice);
                }
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::Settings:
            {
                ImGui::BeginChild("##settingsScroll", ImVec2(0, -(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_None);
                ImGui::TextDisabled("Claude command (add flags, e.g.  claude --model opus)");
                ImGui::SetNextItemWidth(Full);
                ImGui::InputText("##cmd", CommandBuffer_, sizeof CommandBuffer_);
                ImGui::TextDisabled("Terminal font size");
                ImGui::SetNextItemWidth(Full);
                ImGui::SliderInt("##fs", &FontSizeEdit_, 8, 32);
                ImGui::TextDisabled("Session title text size");
                ImGui::SetNextItemWidth(Full);
                ImGui::SliderInt("##pts", &PaneTitleScaleEdit_, 70, 200, "%d%%");
                ImGui::Checkbox("Desktop notifications when a background session needs you", &NotifyEdit_);
                ImGui::Checkbox("Start the 5-hour usage window as soon as it is free (sends a tiny Haiku message)", &AutoStartEdit_);
                ImGui::Checkbox("Use GitHub access (shows login status; needs the gh CLI)", &GitHubEdit_);
                ImGui::TextDisabled("Command changes apply the next time a session starts.");

                ImGui::Separator();
                ImGui::TextUnformatted("Sessions");
                ImGui::BeginChild("##sessionNames", ImVec2(Full, 150 * Scale), ImGuiChildFlags_Borders);
                for (const std::unique_ptr<Session>& SessionPtr : Sessions_)
                {
                    Session const& CurrentSession = *SessionPtr;
                    ImGui::PushID(CurrentSession.Rec.Id.c_str());
                    Project* const CurrentProject = FindProject(CurrentSession.Rec.ProjectId);
                    ImGui::TextDisabled("%s", CurrentProject != nullptr ? CurrentProject->Name.c_str() : "?");
                    ImGui::SameLine(130 * Scale);
                    ImGui::SetNextItemWidth(Full - 130 * Scale);
                    std::array<char, 256>& Buffer = SessionNameEdits_[CurrentSession.Rec.Id];
                    ImGui::InputText("##name", Buffer.data(), Buffer.size());
                    ImGui::PopID();
                }
                if (Sessions_.empty())
                {
                    ImGui::TextDisabled("No sessions yet.");
                }
                ImGui::EndChild();

                ImGui::Separator();
                ImGui::TextUnformatted("Theme");
                ImGui::SameLine();
                if (ImGui::SmallButton("Dark"))
                {
                    ThemeEdit_ = ThemeColors::Dark();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Light"))
                {
                    ThemeEdit_ = ThemeColors::Light();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Midnight"))
                {
                    ThemeEdit_ = ThemeColors::Midnight();
                }
                ImGui::BeginChild("##themeColors", ImVec2(Full, 260 * Scale), ImGuiChildFlags_Borders);
                const std::function<void(const char*, ThemeColor&)> ColorRow = [&](const char* Label, ThemeColor& CurrentColor)
                { ImGui::ColorEdit4(Label, &CurrentColor.Red, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs); };
                ColorRow("Background", ThemeEdit_.Background);
                ColorRow("Surface (sidebar/toolbar)", ThemeEdit_.Surface);
                ColorRow("Pane background", ThemeEdit_.PaneBg);
                ColorRow("Pane border", ThemeEdit_.PaneBorder);
                ColorRow("Accent", ThemeEdit_.Accent);
                ColorRow("Text", ThemeEdit_.Text);
                ColorRow("Text (disabled)", ThemeEdit_.TextDisabled);
                ColorRow("Status: busy", ThemeEdit_.StatusBusy);
                ColorRow("Status: waiting", ThemeEdit_.StatusWaiting);
                ColorRow("Status: done", ThemeEdit_.StatusDone);
                ColorRow("Status: idle", ThemeEdit_.StatusIdle);
                ColorRow("Usage: warning", ThemeEdit_.UsageWarning);
                ColorRow("Usage: full", ThemeEdit_.UsageFull);
                ColorRow("Terminal background", ThemeEdit_.TerminalBg);
                ColorRow("Selection highlight", ThemeEdit_.Selection);
                ColorRow("Cursor", ThemeEdit_.Cursor);
                ColorRow("Title bar background", ThemeEdit_.TitlebarBg);
                ColorRow("Title bar text", ThemeEdit_.TitlebarText);
                ImGui::EndChild();

                DrawKeybindingEditor();
                ImGui::EndChild();

                if (ImGui::Button("Save"))
                {
                    std::string const Command = Util::Trim(CommandBuffer_);
                    State_.AppSettings.ClaudeCommand = Command.empty() ? "claude" : Command;
                    State_.AppSettings.Notifications = NotifyEdit_;
                    State_.AppSettings.AutoStartWindow = AutoStartEdit_;
                    State_.AppSettings.Keys = KeysEdit_;
                    State_.AppSettings.PaneTitleScale = PaneTitleScaleEdit_;
                    if (GitHubEdit_ != State_.AppSettings.GitHubEnabled)
                    {
                        State_.AppSettings.GitHubEnabled = GitHubEdit_;
                        GitHubLastCheckAt_ = -1.0e9;
                    }
                    if (FontSizeEdit_ != State_.AppSettings.FontSize)
                    {
                        State_.AppSettings.FontSize = FontSizeEdit_;
                        FontsDirty_ = true;
                    }
                    State_.AppSettings.Theme = ThemeEdit_;
                    Style::Apply(UiScale_, State_.AppSettings.Theme);
                    Platform::ApplyTitlebarColors(Window_, Style::ToRgbColor(State_.AppSettings.Theme.TitlebarBg),
                                                  Style::ToRgbColor(State_.AppSettings.Theme.TitlebarText));
                    for (const std::pair<const std::string, std::array<char, 256>>& NameEdit : SessionNameEdits_)
                    {
                        Session* const CurrentSession = FindSession(NameEdit.first);
                        if (CurrentSession == nullptr)
                        {
                            continue;
                        }
                        std::string const Name = Util::Trim(NameEdit.second.data());
                        if (Name.empty() == false)
                        {
                            CurrentSession->Rec.Name = Name;
                        }
                    }
                    MarkDirty();
                    CloseModal();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::Usage:
            {
                bool InFlight = false, Done = false;
                UsageInfo CurrentInfo;
                ReadUsageFetch(InFlight, Done, CurrentInfo);
                if (InFlight)
                {
                    ImGui::TextDisabled("Fetching usage...");
                }
                else if (Done == false || CurrentInfo.Ok == false)
                {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(WaitingColorV4_, "%s", CurrentInfo.Error.empty() ? "Could not fetch usage." : CurrentInfo.Error.c_str());
                    ImGui::PopTextWrapPos();
                }
                else
                {
                    if (CurrentInfo.HasSession)
                    {
                        ImGui::Text("Session usage: %.0f%%", CurrentInfo.SessionPct);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UiHelpers::WithAlpha(UsageFillColor(CurrentInfo.SessionPct), 200));
                        ImGui::ProgressBar((float)(CurrentInfo.SessionPct / 100.0), ImVec2(Full, 0));
                        ImGui::PopStyleColor();
                        DrawResetProgress(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                          UiHelpers::UsageResetProgress(CurrentInfo.SessionPct, UiHelpers::SecondsUntil(CurrentInfo.SessionResetsAt), UiHelpers::SESSION_WINDOW_SECONDS));
                        const std::string SessionResetText = UiHelpers::FormatTimeUntil(CurrentInfo.SessionResetsAt);
                        if (SessionResetText.empty() == false)
                        {
                            ImGui::TextDisabled("Resets in %s", SessionResetText.c_str());
                        }
                        ImGui::Spacing();
                    }
                    if (CurrentInfo.HasWeekly)
                    {
                        ImGui::Text("Weekly usage: %.0f%%", CurrentInfo.WeeklyPct);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UiHelpers::WithAlpha(UsageFillColor(CurrentInfo.WeeklyPct), 200));
                        ImGui::ProgressBar((float)(CurrentInfo.WeeklyPct / 100.0), ImVec2(Full, 0));
                        ImGui::PopStyleColor();
                        DrawResetProgress(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                          UiHelpers::UsageResetProgress(CurrentInfo.WeeklyPct, UiHelpers::SecondsUntil(CurrentInfo.WeeklyResetsAt), UiHelpers::WEEKLY_WINDOW_SECONDS));
                        const std::string WeeklyResetText = UiHelpers::FormatTimeUntil(CurrentInfo.WeeklyResetsAt);
                        if (WeeklyResetText.empty() == false)
                        {
                            ImGui::TextDisabled("Resets in %s", WeeklyResetText.c_str());
                        }
                    }
                    if (CurrentInfo.HasSession == false && CurrentInfo.HasWeekly == false)
                    {
                        ImGui::TextDisabled("No usage data returned.");
                    }
                }
                ImGui::Spacing();
                if (ImGui::Button("Refresh"))
                {
                    StartUsageFetch();
                }
                ImGui::SameLine();
                if (ImGui::Button("Close"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::GitHub:
            {
                if (DrawGitHubLogin() == true)
                {
                    GitHubLogin_.Reset();
                    CloseModal();
                }
                break;
            }
            case Modal::Review:
            {
                DrawReview();
                if (ImGui::Button("Close"))
                {
                    CloseModal();
                }
                break;
            }
            case Modal::Search:
            {
                DrawSearch(Enter);
                break;
            }
            default: break;
        }
        ImGui::EndPopup();
    }
    else if (Open == false)
    {
        Modal_ = Modal::None;
    }
    ImGui::PopStyleVar(3);
}

void App::DrawToasts()
{
    Toasts_.erase(std::remove_if(Toasts_.begin(), Toasts_.end(), [&](const Toast& CurrentToast) { return CurrentToast.Until < Now_; }), Toasts_.end());
    if (Toasts_.empty())
    {
        return;
    }
    ImGuiViewport* const Viewport = ImGui::GetMainViewport();
    float ToastY = Viewport->WorkPos.y + Viewport->WorkSize.y - 16 * UiScale_;
    int Index = 0;
    for (auto It = Toasts_.rbegin(); It != Toasts_.rend(); ++It, ++Index)
    {
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::SetNextWindowPos(ImVec2(Viewport->WorkPos.x + Viewport->WorkSize.x - 16 * UiScale_, ToastY), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowSize(ImVec2(380 * UiScale_, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, It->IsError ? WaitingColorV4_ : AccentColorV4_);
        ImGui::Begin(("##toast" + std::to_string(Index)).c_str(), nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs |
                        ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(It->Text.c_str());
        ImGui::PopTextWrapPos();
        ToastY -= ImGui::GetWindowHeight() + 8 * UiScale_;
        ImGui::End();
        ImGui::PopStyleColor();
    }
}
