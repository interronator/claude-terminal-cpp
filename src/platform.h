#pragma once
#include <string>
#include <utility>
#include <vector>

struct GLFWwindow;

namespace platform {
void notify(GLFWwindow* w, const std::string& title, const std::string& body);  // OS notification
void flash(GLFWwindow* w, bool on);                                               // taskbar attention
std::string pickFolder(GLFWwindow* w);                                            // "" if cancelled/unsupported
void reveal(const std::string& path);                                             // open in file manager
bool canPickFolder();
void shutdown(GLFWwindow* w);

struct RgbColor {
  unsigned char r = 0, g = 0, b = 0;
};

// True if the OS clipboard currently holds image data (e.g. a Snipping Tool / Win+Shift+S capture)
// rather than text.
bool clipboardHasImage();
// Tints the native window titlebar (Windows 11 22000+ via DWM). No-op elsewhere / on older Windows.
void applyTitlebarColors(GLFWwindow* w, RgbColor caption, RgbColor text);

struct HttpResult {
  bool ok = false;
  int statusCode = 0;
  std::string body;
  std::string error;
};
// Blocking HTTPS GET. Call from a background thread only. Unsupported on platforms without an impl.
HttpResult httpGetJson(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers);
}  // namespace platform
