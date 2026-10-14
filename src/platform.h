#pragma once
#include <string>
#include <utility>
#include <vector>

struct GLFWwindow;

class Platform
{
public:
    struct RgbColor
    {
        unsigned char Red = 0, Green = 0, Blue = 0;
    };

    struct HttpResult
    {
        bool Ok = false;
        int StatusCode = 0;
        std::string Body;
        std::string Error;
    };

    static void Notify(GLFWwindow* CurrentWindow, const std::string& Title, const std::string& Body);  // OS notification
    static void Flash(GLFWwindow* CurrentWindow, bool On);                                               // taskbar attention
    static std::string PickFolder(GLFWwindow* CurrentWindow);                                            // "" if cancelled/unsupported
    static void RunScripts(const std::string& Dir, const std::string& Command);  // run in a console window
    static void Reveal(const std::string& Path);                                                  // open in file manager
    static bool CanPickFolder();
    static bool IsOnPath(const std::string& Program);  // true if a program with this name resolves on PATH
    static void Shutdown(GLFWwindow* CurrentWindow);

    // True if the OS clipboard currently holds image data (e.g. a Snipping Tool / Win+Shift+S capture)
    // rather than text.
    static bool ClipboardHasImage();
    // Tints the native window titlebar (Windows 11 22000+ via DWM). No-op elsewhere / on older Windows.
    static void ApplyTitlebarColors(GLFWwindow* CurrentWindow, RgbColor Caption, RgbColor Text);

    // Blocking HTTPS GET. Call from a background thread only. Unsupported on platforms without an impl.
    static HttpResult HttpGetJson(const std::string& Url, const std::vector<std::pair<std::string, std::string>>& Headers);
};
