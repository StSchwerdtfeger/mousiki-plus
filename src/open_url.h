#pragma once
// Opens a web address in the default browser (Windows: ShellExecute, macOS: open, elsewhere: xdg-open). Only plain https://
// addresses are accepted (no spaces or shell characters). Returns false when the address was refused or nothing could be started;
// it does not wait for the browser.
#include <string>

namespace muisc {
bool open_url(const std::string& url);
bool url_is_safe(const std::string& url);   // the check open_url() makes (exposed for the tests)
}
