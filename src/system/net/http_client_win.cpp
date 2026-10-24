/**
 * @file        system/net/http_client_win.cpp
 * @brief       Blocking HTTP(S) requests over WinHTTP (see http_client.h).
 */

#include "http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace rex::net::online {
namespace {

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
  return out;
}

// Wine's WinHTTP doesn't know WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (it is
// taken as "no proxy", so a proxy set for Wine is skipped): the default proxy
// settings there (Wine's own, or http_proxy).
DWORD ProxyAccessType() {
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
  }();
  return wine ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
}

}  // namespace

HttpResponse HttpRequest(const std::string& method, const std::string& url,
                         const std::string& body, int timeout_ms) {
  HttpResponse response;
  const std::wstring wide_url = Widen(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 2047;
  wchar_t extra[2048] = {};
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = 2047;
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
    return response;
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  HINTERNET session = WinHttpOpen(L"burstlimit-online", ProxyAccessType(),
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    session = WinHttpOpen(L"burstlimit-online", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session) {
    return response;
  }
  WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
  HINTERNET request = nullptr;
  if (connection) {
    const std::wstring object = std::wstring(path) + extra;
    request = WinHttpOpenRequest(connection, Widen(method).c_str(), object.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
  }
  if (request) {
    const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                          : L"Content-Type: application/json\r\n";
    const DWORD header_length = body.empty() ? 0 : DWORD(-1L);
    BOOL sent = WinHttpSendRequest(request, headers, header_length,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                : const_cast<char*>(body.data()),
                                   DWORD(body.size()), DWORD(body.size()), 0);
    if (sent && WinHttpReceiveResponse(request, nullptr)) {
      DWORD status = 0;
      DWORD status_size = sizeof(status);
      if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                              WINHTTP_NO_HEADER_INDEX)) {
        response.status = int(status);
      }
      for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) {
          break;
        }
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
          break;
        }
        response.body.append(chunk.data(), read);
        if (response.body.size() > (4u << 20)) {
          break;
        }
      }
    }
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return response;
}

}  // namespace rex::net::online
