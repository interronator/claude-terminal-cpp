#pragma once
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

// What the Blender script wrote about the scene at the moment a review picture was taken (<tag>_snap_<id>.json), turned into
// a few plain lines for the message that goes to the session.
class BlenderSnapshotState
{
public:
    struct ObjectInfo
    {
        std::string Name;
        std::string Type;
        float Dimensions[3] = {0.0f, 0.0f, 0.0f};
        int Faces = -1;
        std::vector<std::string> Modifiers;
    };

    bool Load(const std::string& Path)
    {
        std::ifstream Stream(Path, std::ios::binary);
        if (Stream.good() == false)
        {
            return false;
        }
        const nlohmann::json Root = nlohmann::json::parse(Stream, nullptr, false);
        if (Root.is_object() == false)
        {
            return false;
        }
        BlendFile_ = Root.value("BlendFile", "");
        Image_ = Root.value("Image", "");
        BlendCopy_ = Root.contains("Blend") == true && Root["Blend"].is_string() == true ? Root["Blend"].get<std::string>() : std::string();
        const nlohmann::json::const_iterator Scene = Root.find("Scene");
        if (Scene == Root.end() || Scene->is_object() == false)
        {
            return true;
        }
        HasScene_ = true;
        Units_ = Scene->value("Units", "");
        UnitScale_ = Scene->value("UnitScale", 1.0f);
        ObjectCount_ = Scene->value("ObjectCount", 0);
        Active_ = Scene->contains("Active") == true && (*Scene)["Active"].is_string() == true ? (*Scene)["Active"].get<std::string>() : std::string();
        if (Scene->contains("Selected") == true && (*Scene)["Selected"].is_array() == true)
        {
            for (const nlohmann::json& Name : (*Scene)["Selected"])
            {
                if (Name.is_string() == true)
                {
                    Selected_.push_back(Name.get<std::string>());
                }
            }
        }
        if (Scene->contains("Objects") == true && (*Scene)["Objects"].is_array() == true)
        {
            for (const nlohmann::json& Entry : (*Scene)["Objects"])
            {
                if (Entry.is_object() == false)
                {
                    continue;
                }
                ObjectInfo Info;
                Info.Name = Entry.value("Name", "");
                Info.Type = Entry.value("Type", "");
                Info.Faces = Entry.value("Faces", -1);
                if (Entry.contains("Dimensions") == true && Entry["Dimensions"].is_array() == true && Entry["Dimensions"].size() >= 3)
                {
                    for (size_t Axis = 0; Axis < 3; Axis++)
                    {
                        Info.Dimensions[Axis] = Entry["Dimensions"][Axis].is_number() == true ? Entry["Dimensions"][Axis].get<float>() : 0.0f;
                    }
                }
                if (Entry.contains("Modifiers") == true && Entry["Modifiers"].is_array() == true)
                {
                    for (const nlohmann::json& Modifier : Entry["Modifiers"])
                    {
                        if (Modifier.is_string() == true)
                        {
                            Info.Modifiers.push_back(Modifier.get<std::string>());
                        }
                    }
                }
                Objects_.push_back(Info);
            }
        }
        return true;
    }

    // Two or three short lines: object count and the active/selected objects with sizes, the heaviest meshes, and modifiers in use.
    std::string Summary() const
    {
        if (HasScene_ == false)
        {
            return std::string();
        }
        std::string Text = "Scene: " + std::to_string(ObjectCount_) + " objects";
        if (Active_.empty() == false)
        {
            Text += ", active '" + Active_ + "'" + SizeOf(Active_);
        }
        if (Selected_.empty() == false)
        {
            Text += "; selected: " + Join(Selected_, 5);
        }
        Text += "\n";
        std::vector<const ObjectInfo*> Heavy;
        for (const ObjectInfo& Info : Objects_)
        {
            if (Info.Faces > 0)
            {
                Heavy.push_back(&Info);
            }
        }
        std::sort(Heavy.begin(), Heavy.end(), [](const ObjectInfo* const Left, const ObjectInfo* const Right) { return Left->Faces > Right->Faces; });
        std::string HeavyText;
        for (size_t Index = 0; Index < Heavy.size() && Index < 3; Index++)
        {
            HeavyText += (HeavyText.empty() == true ? "" : ", ") + Heavy[Index]->Name + " " + std::to_string(Heavy[Index]->Faces) + " faces";
        }
        if (HeavyText.empty() == false)
        {
            Text += "Heaviest meshes: " + HeavyText + "\n";
        }
        std::string ModifierText;
        size_t ModifierObjects = 0;
        for (const ObjectInfo& Info : Objects_)
        {
            if (Info.Modifiers.empty() == true || ModifierObjects >= 4)
            {
                continue;
            }
            ModifierObjects++;
            ModifierText += (ModifierText.empty() == true ? "" : "; ") + Info.Name + " (" + Join(Info.Modifiers, 4) + ")";
        }
        if (ModifierText.empty() == false)
        {
            Text += "Modifiers: " + ModifierText + "\n";
        }
        return Text;
    }

    const std::string& BlendFile() const
    {
        return BlendFile_;
    }

    // The picture the script saved for this moment, and the copy of the scene when one was requested (empty otherwise).
    const std::string& Image() const
    {
        return Image_;
    }

    const std::string& BlendCopy() const
    {
        return BlendCopy_;
    }

    // One line to run inside Blender (through the Python tool): returns the 3D view to the reviewed angle and saves a new picture of it.
    static std::string RestoreLine(const std::string& ScriptPath, const std::string& StatePath, const std::string& ShotPath)
    {
        return "exec(open(r'" + ScriptPath + "').read()); RestoreReviewView(r'" + StatePath + "', r'" + ShotPath + "')";
    }

private:
    bool HasScene_ = false;
    std::string BlendFile_;
    std::string Image_;
    std::string BlendCopy_;
    std::string Units_;
    float UnitScale_ = 1.0f;
    int ObjectCount_ = 0;
    std::string Active_;
    std::vector<std::string> Selected_;
    std::vector<ObjectInfo> Objects_;

    static std::string Join(const std::vector<std::string>& Names, const size_t Limit)
    {
        std::string Text;
        for (size_t Index = 0; Index < Names.size() && Index < Limit; Index++)
        {
            Text += (Index == 0 ? "" : ", ") + Names[Index];
        }
        if (Names.size() > Limit)
        {
            Text += " and " + std::to_string(Names.size() - Limit) + " more";
        }
        return Text;
    }

    std::string SizeOf(const std::string& Name) const
    {
        for (const ObjectInfo& Info : Objects_)
        {
            if (Info.Name != Name)
            {
                continue;
            }
            const bool Metric = Units_ == "METRIC";
            const float Factor = Metric == true ? UnitScale_ : 1.0f;
            char Buffer[96];
            std::snprintf(Buffer, sizeof Buffer, " (%.2f x %.2f x %.2f %s)", Info.Dimensions[0] * Factor, Info.Dimensions[1] * Factor, Info.Dimensions[2] * Factor,
                          Metric == true ? "m" : "units");
            return Buffer;
        }
        return std::string();
    }
};
