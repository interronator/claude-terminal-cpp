#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"
#include "platform.h"
#include "store.h"
#include "style.h"

static App* g_app = nullptr;

int main(int, char**) {
  glfwSetErrorCallback([](int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); });
  if (!glfwInit()) return 1;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
  GLFWwindow* win = glfwCreateWindow(1500, 900, "Claude Terminal", nullptr, nullptr);
  if (!win) {
    std::fprintf(stderr, "Could not create an OpenGL 3.3 window.\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);

  // Our callbacks go in first; ImGui chains to them when it installs its own.
  glfwSetCharCallback(win, [](GLFWwindow*, unsigned int c) {
    if (g_app) g_app->pushChar(c);
  });
  glfwSetKeyCallback(win, [](GLFWwindow*, int key, int, int action, int mods) {
    if (g_app) g_app->pushKey(key, action, mods);
  });
  glfwSetDropCallback(win, [](GLFWwindow*, int n, const char** paths) {
    if (!g_app) return;
    std::vector<std::string> v;
    for (int i = 0; i < n; i++) v.emplace_back(paths[i]);
    g_app->pushDrop(v);
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");

  float sx = 1, sy = 1;
  glfwGetWindowContentScale(win, &sx, &sy);
  float scale = std::max(sx, sy);
  if (scale < 0.5f) scale = 1;

  Store store(Store::defaultPath());
  store.load();
  applyStyle(scale, store.state.settings.theme);
  platform::applyTitlebarColors(win, toRgbColor(store.state.settings.theme.titlebarBg),
                                toRgbColor(store.state.settings.theme.titlebarText));

  App app(win, store, scale);
  g_app = &app;

  while (!glfwWindowShouldClose(win)) {
    glfwWaitEventsTimeout(app.wantsFastFrames() ? 0.016 : 0.1);
    if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) continue;

    if (app.rebuildFontsIfNeeded()) {
      ImGui_ImplOpenGL3_DestroyFontsTexture();
      ImGui_ImplOpenGL3_CreateFontsTexture();
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    app.frame(glfwGetTime());
    ImGui::Render();

    int w, h;
    glfwGetFramebufferSize(win, &w, &h);
    glViewport(0, 0, w, h);
    ImVec4 bg = toImVec4(store.state.settings.theme.background);
    glClearColor(bg.x, bg.y, bg.z, bg.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(win);
  }

  g_app = nullptr;
  app.shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
