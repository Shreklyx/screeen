// In-app updates: native-latest.json on the "desktop-latest" release says the newest version; the
// installer is downloaded and run silently (it starts the app again).
#include "update.h"

#include <windows.h>
#include <winhttp.h>

#include <vector>

#include "gpu.h"
#include "json.h"

namespace ss {

const char* kFeed = "https://github.com/Shreklyx/screeen/releases/download/desktop-latest";

namespace {

/** GET a URL (following redirects); the body is passed on in pieces. False on any error. */
bool httpGet(const std::string& url, const std::function<bool(const uint8_t*, size_t, uint64_t total)>& body) {
  std::wstring w = widen(url);
  URL_COMPONENTS u{};
  u.dwStructSize = sizeof u;
  wchar_t host[256] = L"", path[2048] = L"";
  u.lpszHostName = host;
  u.dwHostNameLength = 255;
  u.lpszUrlPath = path;
  u.dwUrlPathLength = 2047;
  if (!WinHttpCrackUrl(w.c_str(), 0, 0, &u)) return false;
  HINTERNET s = WinHttpOpen(L"ScreenShare", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) s = WinHttpOpen(L"ScreenShare", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!s) return false;
  bool ok = false;
  HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
  if (r && WinHttpSendRequest(r, L"Cache-Control: no-cache\r\n", DWORD(-1), nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    wchar_t len[32] = L"";
    DWORD lenSize = sizeof len;
    uint64_t total = WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lenSize, WINHTTP_NO_HEADER_INDEX) ? _wcstoui64(len, nullptr, 10) : 0;
    if (status == 200) {
      std::vector<uint8_t> buf(64 * 1024);
      ok = true;
      for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) {
          ok = false;
          break;
        }
        if (!got) break;
        if (!body(buf.data(), got, total)) {
          ok = false;
          break;
        }
      }
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  WinHttpCloseHandle(s);
  return ok;
}

}  // namespace

bool newerVersion(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && a[i] != '.') x = x * 10 + unsigned(a[i++] - '0');
    while (j < b.size() && b[j] != '.') y = y * 10 + unsigned(b[j++] - '0');
    if (x != y) return x > y;
    i++;
    j++;
  }
  return false;
}

bool checkForUpdate(const std::string& current, UpdateInfo& out) {
  std::string text;
  if (!httpGet(std::string(kFeed) + "/native-latest.json", [&](const uint8_t* d, size_t n, uint64_t) {
        text.append(reinterpret_cast<const char*>(d), n);
        return text.size() < 65536;
      }))
    return false;
  Json j = Json::parse(text);
  out.version = j["version"].str("");
  out.url = std::string(kFeed) + "/" + j["windows"].str("ScreenShare-Setup.exe");
  return !out.version.empty() && newerVersion(out.version, current);
}

bool downloadAndInstall(const UpdateInfo& info, const std::function<void(int)>& progress) {
  wchar_t dir[MAX_PATH];
  GetTempPathW(MAX_PATH, dir);
  std::wstring path = std::wstring(dir) + L"ScreenShare-Setup-" + widen(info.version) + L".exe";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  uint64_t got = 0;
  int lastPct = -1;
  bool ok = httpGet(info.url, [&](const uint8_t* d, size_t n, uint64_t total) {
    DWORD written = 0;
    if (!WriteFile(f, d, DWORD(n), &written, nullptr) || written != n) return false;
    got += n;
    int pct = total ? int(got * 100 / total) : -1;
    if (pct != lastPct && progress) progress(lastPct = pct);
    return true;
  });
  CloseHandle(f);
  if (!ok || got < 100000) return false;
  // silent install; the installer starts the new version when it is done
  std::wstring cmd = L"\"" + path + L"\" /S /run";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace ss
