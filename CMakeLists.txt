cmake_minimum_required(VERSION 3.20)
project(ClaudeTerminal VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ---- dependencies (all fetched at configure time) -------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE INTERNAL "")

FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW ON)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW ON)
FetchContent_Declare(libvterm GIT_REPOSITORY https://github.com/neovim/libvterm.git GIT_TAG 934bc2fbf21800ac3458a499df8820ca5fb45fd3)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)
FetchContent_MakeAvailable(glfw imgui libvterm json stb)

# libvterm ships a Makefile, not CMake: build it ourselves.
file(GLOB VTERM_SOURCES ${libvterm_SOURCE_DIR}/src/*.c)
add_library(vterm STATIC ${VTERM_SOURCES})
target_include_directories(vterm PUBLIC ${libvterm_SOURCE_DIR}/include PRIVATE ${libvterm_SOURCE_DIR}/src)
if(NOT MSVC)
  target_compile_options(vterm PRIVATE -w)
  target_compile_definitions(vterm PRIVATE _GNU_SOURCE)
endif()

# Dear ImGui + GLFW/OpenGL3 backends
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

find_package(OpenGL REQUIRED)

# ---- the app ---------------------------------------------------------------
set(APP_SOURCES
  src/main.cpp src/app.cpp src/session.cpp src/terminal.cpp
  src/git.cpp src/store.cpp)
if(WIN32)
  list(APPEND APP_SOURCES src/pty_win.cpp src/platform_win.cpp resources/app.rc)
else()
  list(APPEND APP_SOURCES src/pty_posix.cpp src/platform_posix.cpp)
endif()

add_executable(claude-terminal ${APP_SOURCES})
target_compile_definitions(claude-terminal PRIVATE CLAUDE_TERMINAL_VERSION="${PROJECT_VERSION}")
target_include_directories(claude-terminal PRIVATE src ${stb_SOURCE_DIR})
target_link_libraries(claude-terminal PRIVATE imgui vterm nlohmann_json::nlohmann_json OpenGL::GL)

if(WIN32)
  target_compile_definitions(claude-terminal PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  target_link_libraries(claude-terminal PRIVATE user32 shell32 ole32 shlwapi dwmapi winhttp)
  if(MSVC)
    # The standard getenv/sscanf/fopen calls are used on purpose so the code stays portable; silence MSVC's
    # "use the _s version" deprecation noise.
    target_compile_definitions(claude-terminal PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(claude-terminal PRIVATE /utf-8 /W3)
    target_link_options(claude-terminal PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
  else()
    target_link_options(claude-terminal PRIVATE -mwindows -static)
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(claude-terminal PRIVATE Threads::Threads util)
  target_compile_options(claude-terminal PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
