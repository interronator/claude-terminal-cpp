# Claude Terminal

## Building

The user builds this project themselves. Never build or compile it unless the user explicitly asks you to in that conversation. That includes `build.bat`, `run.bat`, `cmake --build`, or any other compile check. This overrides the general "build and run convention" instruction that sessions are launched with.

After making a change, stop at the code and say it has not been built or tested.

Why: sessions working on this project usually run inside `claude-terminal.exe` itself, and `build.bat`/`run.bat` force-close that program, which kills the session mid-task.
