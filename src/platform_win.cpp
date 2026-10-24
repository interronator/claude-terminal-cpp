#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <winhttp.h>

#include <algorithm>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "platform.h"
#include "win_strings.h"

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace
{


bool TrayAdded = false;
NOTIFYICONDATAW TrayIconData{};

}  // namespace

void Platform::Notify(GLFWwindow* Window, const std::string& Title, const std::string& Body)
{
    HWND const Hwnd = glfwGetWin32Window(Window);
    if (TrayAdded == false)
    {
        TrayIconData = {};
        TrayIconData.cbSize = sizeof(TrayIconData);
        TrayIconData.hWnd = Hwnd;
        TrayIconData.uID = 1;
        TrayIconData.uFlags = NIF_ICON | NIF_TIP;
        TrayIconData.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(TrayIconData.szTip, L"Claude Terminal");
        TrayAdded = Shell_NotifyIconW(NIM_ADD, &TrayIconData) != FALSE;
    }
    if (TrayAdded == false)
    {
        return;
    }
    NOTIFYICONDATAW Notification = TrayIconData;
    Notification.uFlags = NIF_INFO;
    Notification.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(Notification.szInfoTitle, WinStrings::Widen(Title).c_str(), _TRUNCATE);
    wcsncpy_s(Notification.szInfo, WinStrings::Widen(Body).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &Notification);
}

void Platform::Flash(GLFWwindow* Window, bool On)
{
    FLASHWINFO Info{sizeof(Info)};
    Info.hwnd = glfwGetWin32Window(Window);
    Info.dwFlags = On ? (FLASHW_TRAY | FLASHW_TIMERNOFG) : FLASHW_STOP;
    Info.uCount = 5;
    FlashWindowEx(&Info);
}

bool Platform::CanPickFolder()
{
    return true;
}

std::string Platform::PickFolder(GLFWwindow* Window)
{
    std::string Result;
    HRESULT const InitResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* Dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&Dialog))))
    {
        DWORD Options = 0;
        Dialog->GetOptions(&Options);
        Dialog->SetOptions(Options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        Dialog->SetTitle(L"Choose a project folder");
        if (SUCCEEDED(Dialog->Show(glfwGetWin32Window(Window))))
        {
            IShellItem* Item = nullptr;
            if (SUCCEEDED(Dialog->GetResult(&Item)))
            {
                PWSTR Path = nullptr;
                if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)))
                {
                    Result = WinStrings::Narrow(Path);
                    CoTaskMemFree(Path);
                }
                Item->Release();
            }
        }
        Dialog->Release();
    }
    if (SUCCEEDED(InitResult))
    {
        CoUninitialize();
    }
    return Result;
}

void Platform::RunScripts(const std::string& Dir, const std::string& Command)
{
    const std::wstring Params = L"/c \"" + WinStrings::Widen(Command) + L" || pause\"";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", Params.c_str(), WinStrings::Widen(Dir).c_str(), SW_SHOWNORMAL);
}

bool Platform::IsOnPath(const std::string& Program)
{
    const std::wstring Name = WinStrings::Widen(Program);
    for (const wchar_t* const Extension : {L".exe", L".cmd", L".bat", L".com"})
    {
        if (SearchPathW(nullptr, Name.c_str(), Extension, 0, nullptr, nullptr) != 0)
        {
            return true;
        }
    }
    return false;
}

void Platform::Reveal(const std::string& Path)
{
    ShellExecuteW(nullptr, L"open", WinStrings::Widen(Path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Platform::ShowInFolder(const std::string& Path)
{
    std::wstring NativePath = WinStrings::Widen(Path);
    std::replace(NativePath.begin(), NativePath.end(), L'/', L'\\');
    const std::wstring Arguments = L"/select,\"" + NativePath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", Arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

bool Platform::ClipboardHasImage()
{
    return (IsClipboardFormatAvailable(CF_DIB) != FALSE) || (IsClipboardFormatAvailable(CF_BITMAP) != FALSE) || (IsClipboardFormatAvailable(CF_DIBV5) != FALSE);
}

void Platform::ApplyTitlebarColors(GLFWwindow* Window, RgbColor Caption, RgbColor Text)
{
    HWND const Hwnd = glfwGetWin32Window(Window);
    if (Hwnd == nullptr)
    {
        return;
    }
    COLORREF CaptionColor = RGB(Caption.Red, Caption.Green, Caption.Blue);
    COLORREF TextColor = RGB(Text.Red, Text.Green, Text.Blue);
    DwmSetWindowAttribute(Hwnd, DWMWA_CAPTION_COLOR, &CaptionColor, sizeof(CaptionColor));
    DwmSetWindowAttribute(Hwnd, DWMWA_TEXT_COLOR, &TextColor, sizeof(TextColor));
}

Platform::HttpResult Platform::HttpGetJson(const std::string& Url, const std::vector<std::pair<std::string, std::string>>& Headers)
{
    HttpResult Out;
    std::wstring const WideUrl = WinStrings::Widen(Url);
    URL_COMPONENTSW Components{};
    Components.dwStructSize = sizeof(Components);
    wchar_t Host[256]{}, Path[1024]{};
    Components.lpszHostName = Host;
    Components.dwHostNameLength = 256;
    Components.lpszUrlPath = Path;
    Components.dwUrlPathLength = 1024;
    if (WinHttpCrackUrl(WideUrl.c_str(), (DWORD)WideUrl.size(), 0, &Components) == FALSE)
    {
        Out.Error = "Invalid usage URL.";
        return Out;
    }

    HINTERNET const SessionHandle = WinHttpOpen(L"ClaudeTerminal/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0);
    if (SessionHandle == nullptr)
    {
        Out.Error = "Could not initialize WinHTTP.";
        return Out;
    }
    HINTERNET const ConnectHandle = WinHttpConnect(SessionHandle, Host, Components.nPort, 0);
    HINTERNET const RequestHandle = ConnectHandle != nullptr
                                   ? WinHttpOpenRequest(ConnectHandle, L"GET", Path, nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    if (RequestHandle == nullptr)
    {
        Out.Error = "Could not connect to the usage endpoint.";
        if (ConnectHandle != nullptr)
        {
            WinHttpCloseHandle(ConnectHandle);
        }
        WinHttpCloseHandle(SessionHandle);
        return Out;
    }

    std::wstring HeaderLines;
    for (const std::pair<std::string, std::string>& Header : Headers)
    {
        HeaderLines += WinStrings::Widen(Header.first) + L": " + WinStrings::Widen(Header.second) + L"\r\n";
    }
    const bool Sent = (WinHttpSendRequest(RequestHandle, HeaderLines.empty() == true ? nullptr : HeaderLines.c_str(), (DWORD)HeaderLines.size(),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE) &&
                (WinHttpReceiveResponse(RequestHandle, nullptr) != FALSE);
    if (Sent == true)
    {
        DWORD Status = 0, StatusSize = sizeof(Status);
        WinHttpQueryHeaders(RequestHandle, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX,
                            &Status, &StatusSize, WINHTTP_NO_HEADER_INDEX);
        Out.StatusCode = (int)Status;
        DWORD Available = 0;
        while (WinHttpQueryDataAvailable(RequestHandle, &Available) != FALSE && Available > 0)
        {
            std::string Buffer((size_t)Available, '\0');
            DWORD BytesRead = 0;
            if (WinHttpReadData(RequestHandle, Buffer.data(), Available, &BytesRead) == FALSE)
            {
                break;
            }
            Out.Body.append(Buffer.data(), BytesRead);
        }
        Out.Ok = true;
    }
    else
    {
        Out.Error = "Network request failed (WinHTTP error " + std::to_string(GetLastError()) + ").";
    }
    WinHttpCloseHandle(RequestHandle);
    WinHttpCloseHandle(ConnectHandle);
    WinHttpCloseHandle(SessionHandle);
    return Out;
}

void Platform::Shutdown(GLFWwindow*)
{
    if (TrayAdded)
    {
        Shell_NotifyIconW(NIM_DELETE, &TrayIconData);
    }
    TrayAdded = false;
}
