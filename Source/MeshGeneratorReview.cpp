#include "App.h"

#include <filesystem>

#include "MeshGenerator.h"
#include "PeerNames.h"

namespace fs = std::filesystem;

void App::SendMeshGeneratorToReviewer(Session& Target)
{
    if (GeneratorProgress_.Running == true)
    {
        ShowToast("Wait for the running step to finish before sending.", true);
        return;
    }
    GeneratorReport_ = Generator_.ReadReport();
    GeneratorReportAt_ = Now_;
    const MeshGenerator::Report& Report = GeneratorReport_;
    if (Report.Valid == false || Report.RenderPaths.empty() == true)
    {
        ShowToast("Run Cleanup first: there are no renders to review.", true);
        return;
    }
    const std::string OwnDirectory = BlenderOwnDirectory(Target);
    if (OwnDirectory.empty() == true)
    {
        ShowToast("This session has no screenshot folder to copy the renders into.", true);
        return;
    }
    const std::string Tag = ScreenshotFinder::SessionTag(Target.Rec.Id);
    std::error_code ErrorCode;
    fs::create_directories(OwnDirectory, ErrorCode);

    int Added = 0;
    for (const std::string& Path : Report.RenderPaths)
    {
        const std::string Label = fs::path(Path).stem().string();
        const fs::path Copy = fs::path(OwnDirectory) / (Tag + "_gen_" + Generator_.AssetName() + "_" + Label + ".png");
        fs::copy_file(Path, Copy, fs::copy_options::overwrite_existing, ErrorCode);
        if (ErrorCode.value() != 0)
        {
            continue;
        }
        BlenderReviewBatch::Snapshot Item;
        Item.MarkedPath = Copy.string();
        Item.SourcePath = Copy.string();
        Item.FirstNumber = BlenderBatch_.NextNumber();
        Item.Count = 1;
        Item.Notes = std::to_string(Item.FirstNumber) + ". (whole picture) Generated model, " + Label +
                     " view. Judge the overall quality: shape, proportions, texture, holes or patches, anything that looks wrong.\n";
        BlenderBatch_.Add(Item);
        Added++;
    }
    if (Added == 0)
    {
        ShowToast("Could not copy the renders: " + ErrorCode.message(), true);
        return;
    }
    BlenderBatchOwnerId_ = Target.Rec.Id;

    std::string Note = "\n\nThese pictures are renders of a mesh made by the local generator pipeline, not a live Blender view. Brief: " + Generator_.DescribeBrief() +
                       ". Result: " + std::to_string(Report.Tris) + " triangles";
    if (Report.Flags.empty() == true)
    {
        Note += ", all automatic checks passed.";
    }
    if (Report.Flags.empty() == false)
    {
        Note += ". Automatic checks that failed:";
    }
    for (const std::string& Flag : Report.Flags)
    {
        Note += " [" + Flag + "]";
    }
    Note += " Report: " + Report.ReportPath + ". Folder: " + Report.Folder +
            ". Do not edit the model; judge it and tell the user what to change. End your reply with a block headed 'GENERATOR ADVICE:' whose lines use only "
            "these keys, so the user can paste it into the generator: 'prompt: <new subject text>', 'seed: <number>', 'tris: <number>', 'size: <meters>'.";
    SendBlenderReview(Target, true, Note);
}
