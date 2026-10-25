# Claude Terminal

## Building

The user builds this project themselves. Never build or compile it unless the user explicitly asks you to in that conversation. That includes `build.bat`, `run.bat`, `cmake --build`, or any other compile check. This overrides the general "build and run convention" instruction that sessions are launched with.

After making a change, stop at the code and say it has not been built or tested.

Why: sessions working on this project usually run inside `claude-terminal.exe` itself, and `build.bat`/`run.bat` force-close that program, which kills the session mid-task.

## File size

Source files are PascalCase and named after the class or feature they hold (see the global file size and naming rule). The `App` class's methods are split by feature across `Source/Sidebar.cpp`, `Toolbar.cpp`, `Pane.cpp`, `Modals.cpp`, `ProjectLogWindow.cpp` and so on; `Source/App.cpp` keeps only construction, fonts, the session model and the frame loop. Put a new feature in its own file (its own class in a header, or its `App::` methods in a new `<Feature>.cpp`) instead of growing an existing file, and add new .cpp files to `APP_SOURCES` in `CMakeLists.txt`. Keep source files under about 1,500 lines; split one that grows past that.

Why: several sessions edit this repo at once, and one huge file means edit collisions and makes the code hard for people to read.
