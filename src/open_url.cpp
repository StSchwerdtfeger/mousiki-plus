#include "open_url.h"

#include <cstdint>
#include <thread>

#include "process_util.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace muisc {

bool url_is_safe(const std::string& url) {
    if (url.size() < 12 || url.size() > 300 || url.compare(0, 8, "https://") != 0) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c >= 0x7F) return false;                                   // spaces, control and non-ASCII characters
        if (c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' || c == '<' || c == '>' || c == '|' || c == '&' || c == ';' || c == '^' || c == '%')
            return false;
    }
    return true;
}

bool open_url(const std::string& url) {
    if (!url_is_safe(url)) return false;
#if defined(_WIN32)
    const std::wstring w(url.begin(), url.end());   // plain ASCII (url_is_safe)
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
#if defined(__APPLE__)
    const std::string cmd = "open " + shell_quote(url);
#else
    const std::string cmd = "xdg-open " + shell_quote(url);
#endif
    std::thread([cmd] { run_capture(cmd, /*merge_stderr=*/false); }).detach();   // a browser that starts up may not return at once
    return true;
#endif
}

} // namespace muisc
