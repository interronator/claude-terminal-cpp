#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

// The user's highlights on a Blender picture: freehand lines, rectangles and circles, each with a number and a short note.
// Points are stored as fractions (0..1) of the picture so they survive resizing the window.
class BlenderMarkup
{
public:
    enum class Tool
    {
        Pen,
        Rectangle,
        Circle
    };

    struct Mark
    {
        Tool Kind = Tool::Rectangle;
        ImU32 Color = 0;
        std::string ColorName;
        std::vector<ImVec2> Points;
        std::string Note;
        std::string ReferencePath;  // an image the user dropped on the note box: "make it look like this"
    };

    struct Swatch
    {
        const char* Name;
        ImU32 Color;
    };

    static constexpr int SWATCH_COUNT = 6;
    static constexpr int TOOL_COUNT = 3;
    static constexpr size_t NOTE_CAPACITY = 256;
    static constexpr const char* NOTE_POPUP_ID = "##markNote";

    static const Swatch& SwatchAt(const int Index)
    {
        static const Swatch SWATCHES[SWATCH_COUNT] = {
            {"red", IM_COL32(235, 64, 52, 255)},   {"orange", IM_COL32(245, 158, 11, 255)}, {"yellow", IM_COL32(250, 224, 40, 255)},
            {"green", IM_COL32(52, 211, 90, 255)}, {"blue", IM_COL32(59, 130, 246, 255)},   {"magenta", IM_COL32(236, 72, 200, 255)}};
        return SWATCHES[Index];
    }

    static const char* ToolLabel(const Tool Kind)
    {
        switch (Kind)
        {
            case Tool::Pen:
                return "Draw";
            case Tool::Rectangle:
                return "Rectangle";
            case Tool::Circle:
                return "Circle";
        }
        return "";
    }

    static const char* KindWord(const Tool Kind)
    {
        switch (Kind)
        {
            case Tool::Pen:
                return "drawn line";
            case Tool::Rectangle:
                return "rectangle";
            case Tool::Circle:
                return "circle";
        }
        return "";
    }

    const std::vector<Mark>& Marks() const
    {
        return Marks_;
    }

    bool HasMarks() const
    {
        return Marks_.empty() == false;
    }

    int Count() const
    {
        return static_cast<int>(Marks_.size());
    }

    Tool CurrentTool() const
    {
        return CurrentTool_;
    }

    void SetTool(const Tool Kind)
    {
        CurrentTool_ = Kind;
    }

    // The number the first mark gets; later snapshots continue where the previous one stopped.
    void SetFirstNumber(const int Number)
    {
        FirstNumber_ = Number;
    }

    // True while a note box is open, so a file dropped on the window belongs to that mark.
    bool IsEditing() const
    {
        return NoteBoxOpen_ == true && EditingIndex_ >= 0 && EditingIndex_ < static_cast<int>(Marks_.size());
    }

    void AttachReference(const std::string& Path)
    {
        if (IsEditing() == true)
        {
            Marks_[static_cast<size_t>(EditingIndex_)].ReferencePath = Path;
        }
    }

    void Clear()
    {
        Marks_.clear();
        Drawing_ = false;
        Draft_ = Mark{};
        EditingIndex_ = -1;
        OpenNotePopup_ = false;
        NoteBoxOpen_ = false;
    }

    void Undo()
    {
        if (Marks_.empty() == false)
        {
            Marks_.pop_back();
            EditingIndex_ = -1;
        }
    }

    void DrawToolbar(const float Scale)
    {
        for (int Index = 0; Index < TOOL_COUNT; Index++)
        {
            const Tool Kind = static_cast<Tool>(Index);
            if (Index > 0)
            {
                ImGui::SameLine();
            }
            if (ImGui::RadioButton(ToolLabel(Kind), CurrentTool_ == Kind) == true)
            {
                CurrentTool_ = Kind;
            }
        }
        ImGui::SameLine();
        const float SwatchSize = ImGui::GetFrameHeight() * 0.8f;
        for (int Index = 0; Index < SWATCH_COUNT; Index++)
        {
            ImGui::SameLine(0, 4 * Scale);
            ImGui::PushID(Index);
            const ImVec4 Color = ImGui::ColorConvertU32ToFloat4(SwatchAt(Index).Color);
            ImGui::PushStyleColor(ImGuiCol_Button, Color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color);
            if (SelectedSwatch_ == Index)
            {
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 1));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f * Scale);
            }
            if (ImGui::Button("##swatch", ImVec2(SwatchSize, SwatchSize)) == true)
            {
                SelectedSwatch_ = Index;
                CustomColor_ = Color;
            }
            if (SelectedSwatch_ == Index)
            {
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
        }
        ImGui::SameLine(0, 6 * Scale);
        if (ImGui::ColorEdit3("##customColor", &CustomColor_.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel) == true)
        {
            SelectedSwatch_ = -1;
        }
        ImGui::SameLine(0, 10 * Scale);
        if (ImGui::Button("Undo") == true)
        {
            Undo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear marks") == true)
        {
            Clear();
        }
    }

    // Draws the picture's marks over the rectangle and lets the user draw new ones inside it. A finished mark opens a note box.
    void DrawCanvas(const ImVec2 Min, const ImVec2 Max, const float Scale)
    {
        const ImVec2 Size(Max.x - Min.x, Max.y - Min.y);
        const bool NoteOpen = ImGui::IsPopupOpen(NOTE_POPUP_ID);
        ImGui::SetCursorScreenPos(Min);
        ImGui::InvisibleButton("##markupCanvas", Size);
        if (ImGui::IsItemActivated() == true && NoteOpen == false)
        {
            const int BadgeHit = BadgeAt(ImGui::GetIO().MousePos, Min, Size, Scale);
            if (BadgeHit >= 0)
            {
                EditingIndex_ = BadgeHit;
                OpenNotePopup_ = true;
            }
            else
            {
                Drawing_ = true;
                Draft_ = Mark{};
                Draft_.Kind = CurrentTool_;
                Draft_.Color = CurrentColorU32();
                Draft_.ColorName = CurrentColorName();
                Draft_.Points.push_back(Fraction(Min, Size));
            }
        }
        if (Drawing_ == true && ImGui::IsItemActive() == true)
        {
            const ImVec2 Point = Fraction(Min, Size);
            if (Draft_.Kind != Tool::Pen)
            {
                Draft_.Points.resize(1);
                Draft_.Points.push_back(Point);
            }
            else
            {
                const ImVec2 Last = Draft_.Points.back();
                const float DeltaX = (Point.x - Last.x) * Size.x;
                const float DeltaY = (Point.y - Last.y) * Size.y;
                if (DeltaX * DeltaX + DeltaY * DeltaY >= 4.0f)
                {
                    Draft_.Points.push_back(Point);
                }
            }
        }
        if (Drawing_ == true && ImGui::IsItemDeactivated() == true)
        {
            Drawing_ = false;
            if (IsWorthKeeping(Draft_, Size) == true)
            {
                Marks_.push_back(Draft_);
                EditingIndex_ = static_cast<int>(Marks_.size()) - 1;
                OpenNotePopup_ = true;
            }
            Draft_ = Mark{};
        }
        if (ImGui::IsItemHovered() == true)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        ImDrawList* const DrawList = ImGui::GetWindowDrawList();
        DrawList->PushClipRect(Min, Max, true);
        for (size_t Index = 0; Index < Marks_.size(); Index++)
        {
            DrawMark(DrawList, Marks_[Index], FirstNumber_ + static_cast<int>(Index), Min, Size, Scale);
        }
        if (Drawing_ == true)
        {
            DrawMark(DrawList, Draft_, FirstNumber_ + static_cast<int>(Marks_.size()), Min, Size, Scale);
        }
        DrawList->PopClipRect();
        DrawNotePopup(Min, Size, Scale);
    }

    void DrawNotes(const float Scale)
    {
        int RemoveIndex = -1;
        for (size_t Index = 0; Index < Marks_.size(); Index++)
        {
            Mark& Item = Marks_[Index];
            ImGui::PushID(static_cast<int>(Index));
            const ImVec4 Color = ImGui::ColorConvertU32ToFloat4(Item.Color);
            ImGui::TextColored(Color, "%d", FirstNumber_ + static_cast<int>(Index));
            ImGui::SameLine(30 * Scale);
            char Buffer[NOTE_CAPACITY];
            std::snprintf(Buffer, sizeof Buffer, "%s", Item.Note.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::InputTextWithHint("##note", "What is wrong here?", Buffer, sizeof Buffer) == true)
            {
                Item.Note = Buffer;
            }
            ImGui::SameLine();
            if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0)) == true)
            {
                RemoveIndex = static_cast<int>(Index);
            }
            ImGui::PopID();
        }
        if (RemoveIndex >= 0)
        {
            Marks_.erase(Marks_.begin() + RemoveIndex);
            EditingIndex_ = -1;
        }
    }

    // One line per mark, for the message that goes to the session. Extras[Index], when given, is added under that mark's line.
    std::string Describe(const std::vector<std::string>& Extras = std::vector<std::string>()) const
    {
        std::string Text;
        for (size_t Index = 0; Index < Marks_.size(); Index++)
        {
            const Mark& Item = Marks_[Index];
            ImVec2 Low(1.0f, 1.0f);
            ImVec2 High(0.0f, 0.0f);
            for (const ImVec2& Point : Item.Points)
            {
                Low.x = std::min(Low.x, Point.x);
                Low.y = std::min(Low.y, Point.y);
                High.x = std::max(High.x, Point.x);
                High.y = std::max(High.y, Point.y);
            }
            char Line[192];
            std::snprintf(Line, sizeof Line, "%d. (%s %s, from %d%% to %d%% across and %d%% to %d%% down the picture) ", FirstNumber_ + static_cast<int>(Index),
                          Item.ColorName.c_str(), KindWord(Item.Kind), Percent(Low.x), Percent(High.x), Percent(Low.y), Percent(High.y));
            Text += Line;
            Text += Item.Note.empty() == true ? "(no note)" : Item.Note;
            Text += "\n";
            if (Index < Extras.size() && Extras[Index].empty() == false)
            {
                Text += Extras[Index];
            }
        }
        return Text;
    }

private:
    std::vector<Mark> Marks_;
    Mark Draft_;
    bool Drawing_ = false;
    Tool CurrentTool_ = Tool::Rectangle;
    int SelectedSwatch_ = 0;
    ImVec4 CustomColor_ = ImVec4(0.92f, 0.25f, 0.20f, 1.0f);
    int FirstNumber_ = 1;
    int EditingIndex_ = -1;
    bool OpenNotePopup_ = false;
    bool FocusNote_ = false;
    bool NoteBoxOpen_ = false;

    static int Percent(const float Fraction)
    {
        return static_cast<int>(std::lround(Fraction * 100.0f));
    }

    ImU32 CurrentColorU32() const
    {
        return SelectedSwatch_ >= 0 ? SwatchAt(SelectedSwatch_).Color : ImGui::ColorConvertFloat4ToU32(CustomColor_);
    }

    std::string CurrentColorName() const
    {
        if (SelectedSwatch_ >= 0)
        {
            return SwatchAt(SelectedSwatch_).Name;
        }
        char Hex[16];
        std::snprintf(Hex, sizeof Hex, "#%02x%02x%02x", static_cast<int>(CustomColor_.x * 255.0f + 0.5f), static_cast<int>(CustomColor_.y * 255.0f + 0.5f),
                      static_cast<int>(CustomColor_.z * 255.0f + 0.5f));
        return Hex;
    }

    static ImVec2 Fraction(const ImVec2 Min, const ImVec2 Size)
    {
        const ImVec2 Mouse = ImGui::GetIO().MousePos;
        return ImVec2(std::clamp((Mouse.x - Min.x) / std::max(Size.x, 1.0f), 0.0f, 1.0f), std::clamp((Mouse.y - Min.y) / std::max(Size.y, 1.0f), 0.0f, 1.0f));
    }

    static bool IsWorthKeeping(const Mark& Item, const ImVec2 Size)
    {
        if (Item.Points.size() < 2)
        {
            return false;
        }
        if (Item.Kind == Tool::Pen)
        {
            return Item.Points.size() >= 3;
        }
        const float Width = std::fabs(Item.Points[1].x - Item.Points[0].x) * Size.x;
        const float Height = std::fabs(Item.Points[1].y - Item.Points[0].y) * Size.y;
        return Width >= 6.0f && Height >= 6.0f;
    }

    static ImVec2 ToScreen(const ImVec2 Point, const ImVec2 Min, const ImVec2 Size)
    {
        return ImVec2(Min.x + Point.x * Size.x, Min.y + Point.y * Size.y);
    }

    // Where the number badge sits: the start of a line, or the top-left corner of a rectangle or circle, kept inside the picture.
    static ImVec2 AnchorOf(const Mark& Item, const ImVec2 Min, const ImVec2 Size, const float Scale)
    {
        const float Radius = 9.0f * Scale;
        ImVec2 Anchor = ToScreen(Item.Points[0], Min, Size);
        if (Item.Kind != Tool::Pen && Item.Points.size() >= 2)
        {
            const ImVec2 Second = ToScreen(Item.Points[1], Min, Size);
            Anchor = ImVec2(std::min(Anchor.x, Second.x), std::min(Anchor.y, Second.y));
        }
        return ImVec2(std::clamp(Anchor.x, Min.x + Radius, Min.x + Size.x - Radius), std::clamp(Anchor.y, Min.y + Radius, Min.y + Size.y - Radius));
    }

    int BadgeAt(const ImVec2 Position, const ImVec2 Min, const ImVec2 Size, const float Scale) const
    {
        const float Radius = 9.0f * Scale;
        for (int Index = static_cast<int>(Marks_.size()) - 1; Index >= 0; Index--)
        {
            const Mark& Item = Marks_[static_cast<size_t>(Index)];
            if (Item.Points.size() < 2)
            {
                continue;
            }
            const ImVec2 Anchor = AnchorOf(Item, Min, Size, Scale);
            const float DeltaX = Position.x - Anchor.x;
            const float DeltaY = Position.y - Anchor.y;
            if (DeltaX * DeltaX + DeltaY * DeltaY <= Radius * Radius)
            {
                return Index;
            }
        }
        return -1;
    }

    void DrawNotePopup(const ImVec2 Min, const ImVec2 Size, const float Scale)
    {
        if (OpenNotePopup_ == true)
        {
            OpenNotePopup_ = false;
            FocusNote_ = true;
            ImGui::OpenPopup(NOTE_POPUP_ID);
        }
        NoteBoxOpen_ = false;
        if (EditingIndex_ < 0 || EditingIndex_ >= static_cast<int>(Marks_.size()))
        {
            return;
        }
        Mark& Item = Marks_[static_cast<size_t>(EditingIndex_)];
        const ImVec2 Anchor = AnchorOf(Item, Min, Size, Scale);
        ImGui::SetNextWindowPos(ImVec2(Anchor.x + 16.0f * Scale, Anchor.y - 10.0f * Scale), ImGuiCond_Appearing);
        if (ImGui::BeginPopup(NOTE_POPUP_ID, ImGuiWindowFlags_AlwaysAutoResize) == false)
        {
            EditingIndex_ = -1;
            return;
        }
        NoteBoxOpen_ = true;
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Item.Color), "%d", FirstNumber_ + EditingIndex_);
        ImGui::SameLine();
        ImGui::TextDisabled("What is wrong here?");
        if (FocusNote_ == true)
        {
            ImGui::SetKeyboardFocusHere();
            FocusNote_ = false;
        }
        char Buffer[NOTE_CAPACITY];
        std::snprintf(Buffer, sizeof Buffer, "%s", Item.Note.c_str());
        const bool Submitted = ImGui::InputTextMultiline("##noteText", Buffer, sizeof Buffer, ImVec2(280.0f * Scale, 70.0f * Scale),
                                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        Item.Note = Buffer;
        if (Item.ReferencePath.empty() == true)
        {
            ImGui::TextDisabled("Drop an image file here to attach a reference");
        }
        else
        {
            const size_t Slash = Item.ReferencePath.find_last_of("/\\");
            ImGui::Text("Reference: %s", Item.ReferencePath.substr(Slash == std::string::npos ? 0 : Slash + 1).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("remove") == true)
            {
                Item.ReferencePath.clear();
            }
        }
        const bool Confirmed = ImGui::Button("OK") == true;
        ImGui::SameLine();
        const bool Removed = ImGui::Button("Remove mark") == true;
        if (Removed == true)
        {
            Marks_.erase(Marks_.begin() + EditingIndex_);
        }
        if (Submitted == true || Confirmed == true || Removed == true)
        {
            EditingIndex_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    static void DrawMark(ImDrawList* const DrawList, const Mark& Item, const int Number, const ImVec2 Min, const ImVec2 Size, const float Scale)
    {
        if (Item.Points.size() < 2)
        {
            return;
        }
        const float Thickness = 3.0f * Scale;
        if (Item.Kind == Tool::Pen)
        {
            std::vector<ImVec2> Screen;
            Screen.reserve(Item.Points.size());
            for (const ImVec2& Point : Item.Points)
            {
                Screen.push_back(ToScreen(Point, Min, Size));
            }
            DrawList->AddPolyline(Screen.data(), static_cast<int>(Screen.size()), Item.Color, ImDrawFlags_None, Thickness);
        }
        else
        {
            const ImVec2 First = ToScreen(Item.Points[0], Min, Size);
            const ImVec2 Second = ToScreen(Item.Points[1], Min, Size);
            const ImVec2 BoxMin(std::min(First.x, Second.x), std::min(First.y, Second.y));
            const ImVec2 BoxMax(std::max(First.x, Second.x), std::max(First.y, Second.y));
            if (Item.Kind == Tool::Rectangle)
            {
                DrawList->AddRect(BoxMin, BoxMax, Item.Color, 0.0f, 0, Thickness);
            }
            else
            {
                const ImVec2 Center((BoxMin.x + BoxMax.x) * 0.5f, (BoxMin.y + BoxMax.y) * 0.5f);
                DrawList->AddEllipse(Center, ImVec2((BoxMax.x - BoxMin.x) * 0.5f, (BoxMax.y - BoxMin.y) * 0.5f), Item.Color, 0.0f, 0, Thickness);
            }
        }
        const float Radius = 9.0f * Scale;
        const ImVec2 Badge = AnchorOf(Item, Min, Size, Scale);
        DrawList->AddCircleFilled(Badge, Radius, Item.Color);
        char Label[8];
        std::snprintf(Label, sizeof Label, "%d", Number);
        const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
        DrawList->AddText(ImVec2(Badge.x - LabelSize.x * 0.5f, Badge.y - LabelSize.y * 0.5f), IM_COL32(0, 0, 0, 255), Label);
    }
};
