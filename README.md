# Claude Terminal (C++)

A native desktop app for running many Claude Code sessions across many projects. Each session is a real `claude` CLI in its own pseudo-terminal (Windows ConPTY). Written in C++17 with Dear ImGui, GLFW/OpenGL3 and libvterm; produces a single `.exe`.

## Getting started

1. Install the [Claude Code CLI](https://docs.claude.com/en/docs/claude-code) (`npm install -g @anthropic-ai/claude-code`) so `claude` is on your PATH. If it is missing, the app tells you when you start a session.
2. Run the app and start a session. The first time, Claude Code asks you to sign in inside the terminal pane. Follow its prompts; this app never sees your credentials.
3. Optional, for the Git panel: install the [GitHub CLI](https://cli.github.com) (`gh`), then use **Sign in with GitHub** in the app. It shows a one-time code to enter on github.com, and `gh` keeps the login.

No accounts or settings are bundled with the app; everything is per user (see Configuration).

## Build (Windows)

Prerequisites: Git, CMake 3.20+, and Visual Studio 2022 Build Tools (MSVC) or MinGW-w64. Dependencies (GLFW, Dear ImGui, libvterm, nlohmann/json) are fetched automatically by CMake.

Double-click or run `build.bat` (options: `build.bat clean`, `build.bat run`). The result is `dist\claude-terminal.exe`.

Or manually:
```
cmake -S . -B build
cmake --build build --config Release
build\Release\claude-terminal.exe
```

Requires Windows 10 1809+ (ConPTY) and the `claude` CLI on your PATH.

## Using it

- Add a project folder (button, or drag a folder onto the window), then **+** on a project to start a session.
- Open sessions tile automatically; **Tabs** shows one at a time. Hide (–) keeps a session running; × in the sidebar closes it.
- Sessions persist and resume on next launch (`claude --resume <id>`; falls back to a fresh start if resume fails).
- Optional git worktree per session: branch `claude/<name>-xxxx` under `.claude/worktrees/`; on close choose keep or remove.

### Status dots
Orange pulsing = working; red = waiting for approval/input; bright green = finished while you were elsewhere; dim green = idle; hollow = not running. Red/green trigger a notification and taskbar flash when unfocused.

### Shortcuts
Alt+1…9 focus pane; Ctrl+Shift+N new session; Ctrl+Shift+M maximize; Ctrl+Shift+W hide pane; Ctrl+Shift+C / Ctrl+Shift+V copy/paste (Ctrl+C copies if text is selected, else interrupts); Shift+Enter newline; Ctrl+= / Ctrl+- / Ctrl+0 zoom.

## Configuration
- State: `%APPDATA%\ClaudeTerminal\state.json` (override with env `CLAUDE_TERMINAL_DATA`).
- Claude command and font size in Settings (e.g. `claude --model opus`).
- Fonts: Segoe UI / Cascadia Mono / Consolas by default; override with `CT_FONT_UI`, `CT_FONT_MONO`, `CT_FONT_MONO_BOLD`.

## Status detection
Inferred from output: sustained output = busy; after ~2s silence, the bottom of the screen is matched against approval patterns (`Do you want to`, `Esc to cancel`, `❯ 1. Yes`…). Adjust the patterns in `src/session.cpp` if Claude Code's prompts change.

## Layout
- `src/` C++ sources. `app.cpp` is the UI, `session.cpp` status and lifecycle, `terminal.cpp` the libvterm wrapper, `pty_win.cpp` / `pty_posix.cpp` the PTY backends, `git.cpp` worktrees, `store.cpp` JSON state. Most other logic lives in header-only classes (`*.h`).
- `resources/` Windows icon and resource script.
- `scripts/` developer tooling (`check-bool-style.ps1` flags raw `!` negation).
- `build.bat` builds and copies the exe to `dist/`. `build/` and `dist/` are generated and git-ignored.

## Testing status
Developed and tested on Windows with MSVC. The POSIX PTY backend (`pty_posix.cpp`) is less exercised.
