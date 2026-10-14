#include <GLFW/glfw3.h>

#include <algorithm>
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

static App* GApp = nullptr;

int main(int, char**)
{
    glfwSetErrorCallback([](int Code, const char* Message) { std::fprintf(stderr, "GLFW error %d: %s\n", Code, Message); });
    if (glfwInit() == GLFW_FALSE)
    {
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* const Window = glfwCreateWindow(1500, 900, "Claude Terminal", nullptr, nullptr);
    if (Window == nullptr)
    {
        std::fprintf(stderr, "Could not create an OpenGL 3.3 window.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(Window);
    glfwSwapInterval(1);

    // Our callbacks go in first; ImGui chains to them when it installs its own.
    glfwSetCharCallback(Window, [](GLFWwindow*, unsigned int Character)
                        {
                            if (GApp != nullptr)
                            {
                                GApp->PushChar(Character);
                            }
                        });
    glfwSetKeyCallback(Window, [](GLFWwindow*, int Key, int, int Action, int Mods)
                       {
                           if (GApp != nullptr)
                           {
                               GApp->PushKey(Key, Action, Mods);
                           }
                       });
    glfwSetDropCallback(Window, [](GLFWwindow*, int Count, const char** Paths)
                        {
                            if (GApp == nullptr)
                            {
                                return;
                            }
                            std::vector<std::string> PathList;
                            for (int Index = 0; Index < Count; Index++)
                            {
                                PathList.emplace_back(Paths[Index]);
                            }
                            GApp->PushDrop(PathList);
                        });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& Io = ImGui::GetIO();
    Io.IniFilename = nullptr;
    Io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplGlfw_InitForOpenGL(Window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    float ScaleX = 1, ScaleY = 1;
    glfwGetWindowContentScale(Window, &ScaleX, &ScaleY);
    float Scale = std::max(ScaleX, ScaleY);
    if (Scale < 0.5f)
    {
        Scale = 1;
    }

    Store Store(Store::DefaultPath());
    Store.Load();
    Style::Apply(Scale, Store.AppState.AppSettings.Theme);
    Platform::ApplyTitlebarColors(Window, Style::ToRgbColor(Store.AppState.AppSettings.Theme.TitlebarBg),
                                  Style::ToRgbColor(Store.AppState.AppSettings.Theme.TitlebarText));

    App App(Window, Store, Scale);
    GApp = &App;

    while (glfwWindowShouldClose(Window) == GLFW_FALSE)
    {
        glfwWaitEventsTimeout(App.FrameWaitSeconds());
        if (glfwGetWindowAttrib(Window, GLFW_ICONIFIED) != 0)
        {
            App.TickSessions(glfwGetTime());
            continue;
        }

        if (App.RebuildFontsIfNeeded())
        {
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        App.Frame(glfwGetTime());
        ImGui::Render();

        int Width, Height;
        glfwGetFramebufferSize(Window, &Width, &Height);
        glViewport(0, 0, Width, Height);
        ImVec4 const Background = Style::ToImVec4(Store.AppState.AppSettings.Theme.Background);
        glClearColor(Background.x, Background.y, Background.z, Background.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(Window);
    }

    GApp = nullptr;
    App.Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(Window);
    glfwTerminate();
    return 0;
}
