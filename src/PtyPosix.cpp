#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "Pty.h"

extern char** environ;

namespace
{

class PosixPty : public Pty
{
public:
    ~PosixPty() override
    {
        Kill();
        if (Reader_.joinable())
        {
            Reader_.join();
        }
        if (Fd_ >= 0)
        {
            ::close(Fd_);
        }
    }

    bool Start(const std::vector<std::string>& argv, const std::string& cwd, int cols, int rows, PtyCallbacks callbacks,
               std::string& err) override
    {
        if (argv.empty())
        {
            err = "empty command";
            return false;
        }
        Callbacks_ = std::move(callbacks);
        std::string command = "exec";
        for (auto& arg : argv)
        {
            command += " " + ShellQuote(arg);
        }
        const char* shellEnv = std::getenv("SHELL");
        std::string shell = (shellEnv != nullptr && *shellEnv != '\0') ? shellEnv : "/bin/bash";

        std::vector<std::string> environmentStrings;
        for (char* const* entry = environ; entry != nullptr && *entry != nullptr; entry++)
        {
            const std::string Entry = *entry;
            if (Entry.rfind("TERM=", 0) == 0 || Entry.rfind("COLORTERM=", 0) == 0)
            {
                continue;
            }
            environmentStrings.push_back(Entry);
        }
        environmentStrings.push_back("TERM=xterm-256color");
        environmentStrings.push_back("COLORTERM=truecolor");
        std::vector<char*> environmentPointers;
        for (std::string& Entry : environmentStrings)
        {
            environmentPointers.push_back(Entry.data());
        }
        environmentPointers.push_back(nullptr);
        const char* const cwdPath = cwd.empty() == true ? nullptr : cwd.c_str();

        struct winsize windowSize {};
        windowSize.ws_col = (unsigned short)cols;
        windowSize.ws_row = (unsigned short)rows;
        Pid_ = forkpty(&Fd_, nullptr, nullptr, &windowSize);
        if (Pid_ < 0)
        {
            err = "forkpty failed";
            return false;
        }
        if (Pid_ == 0)
        {
            // A parent that started us in the background may have left these ignored; children must not inherit that.
            for (int signalNumber : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD})
            {
                signal(signalNumber, SIG_DFL);
            }
            sigset_t none;
            sigemptyset(&none);
            sigprocmask(SIG_SETMASK, &none, nullptr);
            if (cwdPath != nullptr && chdir(cwdPath) != 0)
            {
                _exit(126);
            }
            execle(shell.c_str(), shell.c_str(), "-lc", command.c_str(), (char*)nullptr, environmentPointers.data());
            _exit(127);
        }
        Reader_ = std::thread([this] { ReadLoop(); });
        return true;
    }

    void Write(const char* data, size_t length) override
    {
        while (length > 0 && Fd_ >= 0)
        {
            ssize_t written = ::write(Fd_, data, length);
            if (written < 0)
            {
                if (errno == EINTR || errno == EAGAIN)
                {
                    continue;
                }
                return;
            }
            data += written;
            length -= (size_t)written;
        }
    }

    void Resize(int cols, int rows) override
    {
        struct winsize windowSize {};
        windowSize.ws_col = (unsigned short)cols;
        windowSize.ws_row = (unsigned short)rows;
        if (Fd_ >= 0)
        {
            ioctl(Fd_, TIOCSWINSZ, &windowSize);
        }
    }

    void Kill() override
    {
        Stop_ = true;
        if (Pid_ > 0 && Reaped_ == false)
        {
            ::kill(-Pid_, SIGHUP);
            ::kill(-Pid_, SIGTERM);
            ::kill(-Pid_, SIGKILL);
        }
    }

private:
    static std::string ShellQuote(const std::string& text)
    {
        std::string out = "'";
        for (char character : text)
        {
            if (character == '\'')
            {
                out += "'\\''";
            }
            else
            {
                out += character;
            }
        }
        return out + "'";
    }

    void ReadLoop()
    {
        char buffer[16384];
        for (;;)
        {
            struct pollfd pollInfo {Fd_, POLLIN, 0};
            int pollResult = poll(&pollInfo, 1, 100);
            if (pollResult < 0 && errno != EINTR)
            {
                break;
            }
            if (pollResult > 0)
            {
                ssize_t bytesRead = ::read(Fd_, buffer, sizeof buffer);
                if (bytesRead > 0)
                {
                    if (Stop_ == false && Callbacks_.OnData)
                    {
                        Callbacks_.OnData(buffer, (size_t)bytesRead);
                    }
                    continue;
                }
                if (bytesRead == 0 || (errno != EINTR && errno != EAGAIN))
                {
                    break;
                }
            }
            if (Stop_)
            {
                break;
            }
        }
        int status = 0;
        int exitCode = -1;
        if (waitpid(Pid_, &status, Stop_ ? WNOHANG : 0) > 0 || Stop_)
        {
            if (WIFEXITED(status))
            {
                exitCode = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                exitCode = 128 + WTERMSIG(status);
            }
        }
        Reaped_ = true;
        if (Stop_ == false && Callbacks_.OnExit)
        {
            Callbacks_.OnExit(exitCode);
        }
    }

    PtyCallbacks Callbacks_;
    pid_t Pid_ = -1;
    int Fd_ = -1;
    std::thread Reader_;
    std::atomic<bool> Stop_{false};
    std::atomic<bool> Reaped_{false};
};

}  // namespace

std::unique_ptr<Pty> Pty::Create()
{
    return std::make_unique<PosixPty>();
}
