#pragma once
#include <cstddef>
#include <string>
#include <vector>

// The marked-up snapshots of one review, waiting to be sent to the session together. Mark numbers keep counting across
// snapshots, so each number in the message points at exactly one note.
class BlenderReviewBatch
{
public:
    struct Snapshot
    {
        std::string MarkedPath;
        std::string SourcePath;
        std::string Notes;
        std::string StatePath;
        std::string BlendPath;
        std::string SceneSummary;
        std::string RestoreLine;
        int FirstNumber = 1;
        int Count = 0;
    };

    const std::vector<Snapshot>& Snapshots() const
    {
        return Snapshots_;
    }

    bool Empty() const
    {
        return Snapshots_.empty() == true;
    }

    int NextNumber() const
    {
        return NextNumber_;
    }

    void Add(const Snapshot& Item)
    {
        Snapshots_.push_back(Item);
        NextNumber_ = Item.FirstNumber + Item.Count;
    }

    void Remove(const size_t Index)
    {
        if (Index < Snapshots_.size())
        {
            Snapshots_.erase(Snapshots_.begin() + static_cast<std::ptrdiff_t>(Index));
        }
        if (Snapshots_.empty() == true)
        {
            NextNumber_ = 1;
        }
    }

    void Clear()
    {
        Snapshots_.clear();
        NextNumber_ = 1;
    }

    // Closing says what the receiver should do with the pictures, since the reviewer and the model's owner act differently.
    // History is the "round N / earlier rounds" text (empty when there is none).
    std::string BuildPrompt(const std::string& Subject, const std::string& History, const std::string& Closing) const
    {
        std::string Text = "The user reviewed " + Subject + " in Blender from " + std::to_string(Snapshots_.size()) +
                           (Snapshots_.size() == 1 ? " angle" : " angles") +
                           " and marked what they are not happy with. Each marked picture has numbered highlights (lines, rectangles and circles drawn on "
                           "it) and every number has one note; numbers are unique across all pictures of this round.\n" +
                           History;
        for (size_t Index = 0; Index < Snapshots_.size(); Index++)
        {
            const Snapshot& Item = Snapshots_[Index];
            Text += "\nPicture " + std::to_string(Index + 1) + " (marks " + std::to_string(Item.FirstNumber) + " to " +
                    std::to_string(Item.FirstNumber + Item.Count - 1) + ")\nMarked picture: " + Item.MarkedPath + "\nOriginal picture without marks: " +
                    Item.SourcePath + "\n";
            if (Item.SceneSummary.empty() == false)
            {
                Text += "Scene when this picture was taken (details in " + Item.StatePath + "):\n" + Item.SceneSummary;
            }
            if (Item.BlendPath.empty() == false)
            {
                Text += "Copy of the whole scene (.blend) at that moment: " + Item.BlendPath + "\n";
            }
            if (Item.RestoreLine.empty() == false)
            {
                Text += "To return Blender's 3D view to this exact angle and save a new picture of it, run this in Blender (Python tool): " + Item.RestoreLine + "\n";
            }
            Text += "Marks:\n" + Item.Notes;
        }
        Text += "\n" + Closing;
        return Text;
    }

private:
    std::vector<Snapshot> Snapshots_;
    int NextNumber_ = 1;
};
