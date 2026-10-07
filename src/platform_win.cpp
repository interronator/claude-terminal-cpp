#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <winhttp.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include "platform.h"

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

namespace platform {

namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
bool trayAdded = false;
NOTIFYICONDATAW nid{};
}  // namespace

void notify(GLFWwindow* w, const std::string& title, const std::string& body) {
  HWND hwnd = glfwGetWin32Window(w);
  if (!trayAdded) {
    nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Claude Terminal");
    trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
  }
  if (!trayAdded) return;
  NOTIFYICONDATAW n = nid;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = NIIF_INFO;
  wcsncpy_s(n.szInfoTitle, widen(title).c_str(), _TRUNCATE);
  wcsncpy_s(n.szInfo, widen(body).c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

void flash(GLFWwindow* w, bool on) {
  FLASHWINFO f{sizeof(f)};
  f.hwnd = glfwGetWin32Window(w);
  f.dwFlags = on ? (FLASHW_TRAY | FLASHW_TIMERNOFG) : FLASHW_STOP;
  f.uCount = 5;
  FlashWindowEx(&f);
}

bool canPickFolder() { return true; }

std::string pickFolder(GLFWwindow* w) {
  std::string result;
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dlg = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose a project folder");
    if (SUCCEEDED(dlg->Show(glfwGetWin32Window(w)))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          result = narrow(path);
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dlg->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

void reveal(const std::string& path) { ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

bool clipboardHasImage()
{
  return IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIBV5);
}

void applyTitlebarColors(GLFWwindow* w, RgbColor caption, RgbColor text) {
  HWND hwnd = glfwGetWin32Window(w);
  if (!hwnd) return;
  COLORREF cap = RGB(caption.r, caption.g, caption.b);
  COLORREF txt = RGB(text.r, text.g, text.b);
  DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
  DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &txt, sizeof(txt));
}

HttpResult httpGetJson(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers) {
  HttpResult out;
  std::wstring wurl = widen(url);
  URL_COMPONENTSW uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256]{}, path[1024]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 1024;
  if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
    out.error = "Invalid usage URL.";
    return out;
  }

  HINTERNET hSession = WinHttpOpen(L"ClaudeTerminal/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    out.error = "Could not initialize WinHTTP.";
    return out;
  }
  HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
  HINTERNET hRequest = hConnect ? WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
  if (!hRequest) {
    out.error = "Could not connect to the usage endpoint.";
    if (hConnect) WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return out;
  }

  std::wstring hdrs;
  for (auto& [k, v] : headers) hdrs += widen(k) + L": " + widen(v) + L"\r\n";
  bool sent = WinHttpSendRequest(hRequest, hdrs.empty() ? nullptr : hdrs.c_str(), (DWORD)hdrs.size(),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(hRequest, nullptr);
  if (sent) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    out.statusCode = (int)status;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
      std::string buf((size_t)avail, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
      out.body.append(buf.data(), read);
    }
    out.ok = true;
  } else {
    out.error = "Network request failed (WinHTTP error " + std::to_string(GetLastError()) + ").";
  }
  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);
  return out;
}

void shutdown(GLFWwindow*) {
  if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &nid);
  trayAdded = false;
}

}  // namespace platform
