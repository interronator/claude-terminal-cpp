#include <cstdlib>
#include <unistd.h>

#include "git.h"
#include "platform.h"

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

void Platform::Reveal(const std::string& path)
{
    Git::RunProcess({"xdg-open", path}, "");
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
