#include <cstdlib>
#include <filesystem>
#include <unistd.h>

#include "Git.h"
#include "Platform.h"

void Platform::Notify(GLFWwindow*, const std::string& title, const std::string& body)
{
    if (access("/usr/bin/notify-send", X_OK) == 0)
    {
        Git::RunProcess({"notify-send", "-a", "Claude Terminal", title, body}, "");
    }
}

void Platform::Flash(GLFWwindow*, bool) {}

std::string Platform::PickFolder(GLFWwindow*)
{
    return "";
}

bool Platform::CanPickFolder()
{
    return false;
}

void Platform::RunScripts(const std::string&, const std::string&) {}

bool Platform::IsOnPath(const std::string& Program)
{
    const char* const PathVariable = std::getenv("PATH");
    if (PathVariable == nullptr)
    {
        return false;
    }
    const std::string Paths = PathVariable;
    size_t Start = 0;
    while (Start <= Paths.size())
    {
        size_t End = Paths.find(':', Start);
        if (End == std::string::npos)
        {
            End = Paths.size();
        }
        const std::string Candidate = Paths.substr(Start, End - Start) + "/" + Program;
        if (access(Candidate.c_str(), X_OK) == 0)
        {
            return true;
        }
        Start = End + 1;
    }
    return false;
}

void Platform::Reveal(const std::string& path)
{
    Git::RunProcess({"xdg-open", path}, "");
}

void Platform::ShowInFolder(const std::string& Path)
{
    Git::RunProcess({"xdg-open", std::filesystem::path(Path).parent_path().string()}, "");
}

void Platform::Shutdown(GLFWwindow*) {}

bool Platform::ClipboardHasImage()
{
    return false;
}

void Platform::ApplyTitlebarColors(GLFWwindow*, RgbColor, RgbColor) {}

Platform::HttpResult Platform::HttpGetJson(const std::string&, const std::vector<std::pair<std::string, std::string>>&)
{
    return {false, 0, "", "HTTP requests are not supported on this platform."};
}
