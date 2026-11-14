#pragma once
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "ModelCatalog.h"

// The model and permission mode new Claude sessions open with. An empty value leaves Claude's own default.
class SessionDefaults
{
public:
    struct Choice
    {
        std::string Value;
        std::string Label;
    };

    // The newest model of each family goes in Latest (plus Opus Plan); everything else goes in Older. Models arrive newest first.
    static void ModelChoices(const std::vector<ModelCatalog::Model>& Models, std::vector<Choice>& Latest, std::vector<Choice>& Older)
    {
        Latest = {{"", "Claude's default"}};
        Older.clear();
        std::vector<const ModelCatalog::Model*> Newest;
        std::set<std::string> SeenFamilies;
        for (const ModelCatalog::Model& Current : Models)
        {
            if (SeenFamilies.insert(Current.Family).second == true)
            {
                Newest.push_back(&Current);
                continue;
            }
            Older.push_back({Current.Id, Current.Name});
        }
        std::stable_sort(Newest.begin(), Newest.end(),
                         [](const ModelCatalog::Model* const Left, const ModelCatalog::Model* const Right) { return FamilyRank(Left->Family) < FamilyRank(Right->Family); });
        const ModelCatalog::Model* const Opus = FindFamily(Newest, "opus");
        const ModelCatalog::Model* const Sonnet = FindFamily(Newest, "sonnet");
        for (const ModelCatalog::Model* const Current : Newest)
        {
            Latest.push_back({Current->Id, Current->Name});
            if (Current != Opus || Sonnet == nullptr)
            {
                continue;
            }
            Latest.push_back({"opusplan", "Opus Plan (" + Opus->Name + " to plan, " + Sonnet->Name + " to work)"});
        }
    }

    static const std::vector<Choice>& Modes()
    {
        static const std::vector<Choice> MODES = {
            {"", "Default (asks before changes)"}, {"auto", "Auto"}, {"plan", "Plan"}, {"acceptEdits", "Accept edits"}};
        return MODES;
    }

    // Flags already in the user's Claude command win over these defaults.
    static void AppendFlags(std::vector<std::string>& Argv, const std::string& Model, const std::string& Mode)
    {
        AppendFlag(Argv, "--model", Model);
        AppendFlag(Argv, "--permission-mode", Mode);
    }

    static void AppendName(std::vector<std::string>& Argv, const std::string& Name)
    {
        AppendFlag(Argv, "--name", Name);
    }

    // Older choices sit in a submenu at the bottom of the list, so the main list only shows the latest versions.
    static void DrawCombo(const char* const Id, std::string& Value, const std::vector<Choice>& Choices, const std::vector<Choice>& Older)
    {
        const Choice* Current = Find(Value, Choices);
        if (Current == nullptr)
        {
            Current = Find(Value, Older);
        }
        if (ImGui::BeginCombo(Id, Current != nullptr ? Current->Label.c_str() : Value.c_str()) == false)
        {
            return;
        }
        DrawChoiceList(Value, Choices, Older);
        ImGui::EndCombo();
    }

    // The items inside an open combo or popup. Returns true when the user picked something different from Value.
    static bool DrawChoiceList(std::string& Value, const std::vector<Choice>& Choices, const std::vector<Choice>& Older)
    {
        bool Changed = false;
        for (const Choice& Candidate : Choices)
        {
            const bool Selected = Value == Candidate.Value;
            if (ImGui::Selectable(Candidate.Label.c_str(), Selected) == true)
            {
                Changed = Selected == false;
                Value = Candidate.Value;
            }
            if (Selected == true)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        if (Older.empty() == true)
        {
            return Changed;
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Older versions") == false)
        {
            return Changed;
        }
        for (const Choice& Candidate : Older)
        {
            if (ImGui::MenuItem(Candidate.Label.c_str(), nullptr, Value == Candidate.Value) == true)
            {
                Changed = Value != Candidate.Value;
                Value = Candidate.Value;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndMenu();
        return Changed;
    }

private:
    static int FamilyRank(const std::string& Family)
    {
        static const std::vector<std::string> ORDER = {"fable", "opus", "sonnet", "haiku"};
        const std::vector<std::string>::const_iterator Found = std::find(ORDER.begin(), ORDER.end(), Family);
        if (Found == ORDER.end())
        {
            return -1;  // a family we don't know yet is probably new, so it goes first (the stable sort keeps newest first)
        }
        return static_cast<int>(Found - ORDER.begin());
    }

    static const ModelCatalog::Model* FindFamily(const std::vector<const ModelCatalog::Model*>& Models, const char* const Family)
    {
        for (const ModelCatalog::Model* const Current : Models)
        {
            if (Current->Family == Family)
            {
                return Current;
            }
        }
        return nullptr;
    }

    static const Choice* Find(const std::string& Value, const std::vector<Choice>& Choices)
    {
        for (const Choice& Candidate : Choices)
        {
            if (Value == Candidate.Value)
            {
                return &Candidate;
            }
        }
        return nullptr;
    }

    static void AppendFlag(std::vector<std::string>& Argv, const char* const Flag, const std::string& Value)
    {
        if (Value.empty() == true)
        {
            return;
        }
        const std::string Prefix = std::string(Flag) + "=";
        for (const std::string& Existing : Argv)
        {
            if (Existing == Flag || Existing.rfind(Prefix, 0) == 0)
            {
                return;
            }
        }
        Argv.push_back(Flag);
        Argv.push_back(Value);
    }
};
