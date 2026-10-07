#include <cstdlib>
#include <unistd.h>

#include "git.h"
#include "platform.h"

namespace platform {
void notify(GLFWwindow*, const std::string& title, const std::string& body) {
  if (access("/usr/bin/notify-send", X_OK) == 0) runProcess({"notify-send", "-a", "Claude Terminal", title, body}, "");
}
void flash(GLFWwindow*, bool) {}
std::string pickFolder(GLFWwindow*) { return ""; }
bool canPickFolder() { return false; }
void reveal(const std::string& path) { runProcess({"xdg-open", path}, ""); }
void shutdown(GLFWwindow*) {}
bool clipboardHasImage() { return false; }
void applyTitlebarColors(GLFWwindow*, RgbColor, RgbColor) {}
HttpResult httpGetJson(const std::string&, const std::vector<std::pair<std::string, std::string>>&) {
  return {false, 0, "", "HTTP requests are not supported on this platform."};
}
}  // namespace platform
