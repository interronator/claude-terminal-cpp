#include "App.h"

#include <cstdlib>
#include <filesystem>
#include <string>

// General sessions still need a working folder, so they share one under the user's home that shows up as a
// "General" project in the sidebar.
std::string App::GeneralFolderPath() const
{
#ifdef _WIN32
    const char* const Home = std::getenv("USERPROFILE");
#else
    const char* const Home = std::getenv("HOME");
#endif
    if (Home == nullptr)
    {
        return std::string();
    }
    return (std::filesystem::path(Home) / "ClaudeGeneral").lexically_normal().make_preferred().string();
}

void App::StartGeneralSession()
{
    const std::string Folder = GeneralFolderPath();
    if (Folder.empty() == true)
    {
        ShowToast("Could not find your home folder for a general session.", true);
        return;
    }
    std::error_code ErrorCode;
    std::filesystem::create_directories(Folder, ErrorCode);
    std::string Err;
    std::string ProjectId;
    if (AddProjectPath(Folder, Err, &ProjectId) == false)
    {
        ShowToast("Could not start a general session: " + Err, true);
        return;
    }
    Project* const General = FindProject(ProjectId);
    if (General != nullptr && General->Name == "ClaudeGeneral")
    {
        General->Name = "General";
        MarkDirty();
    }
    Session* const NewSession = CreateSession(ProjectId, "", false, false, Err);
    if (NewSession == nullptr)
    {
        ShowToast("Could not start a general session: " + Err, true);
        return;
    }
    State_.Ui.Collapsed[ProjectId] = false;
    OpenSession(NewSession->Rec.Id);
}
