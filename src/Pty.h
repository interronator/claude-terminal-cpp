#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PtyCallbacks
{
    std::function<void(const char*, size_t)> OnData;  // called from a reader thread
    std::function<void(int)> OnExit;                  // called once, after the last OnData
};

// A child process attached to a pseudo-terminal (ConPTY on Windows, forkpty elsewhere).
class Pty
{
public:
    virtual ~Pty() = default;
    static std::unique_ptr<Pty> Create();

    // Argv[0] is looked up on PATH. Windows goes through cmd.exe so claude.exe and claude.cmd both work.
    virtual bool Start(const std::vector<std::string>& Argv, const std::string& Cwd, int Cols, int Rows,
                       PtyCallbacks Callbacks, std::string& Err) = 0;
    virtual void Write(const char* Data, size_t Length) = 0;
    virtual void Resize(int Cols, int Rows) = 0;
    virtual void Kill() = 0;  // also kills child processes; the destructor joins all threads
};
