// The status message history (second pane of the console overlay).
#include "app.h"

#include <ctime>

namespace muisc {

namespace {
constexpr size_t kMaxStatusHistory = 500;

std::string stamp_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return std::string("[") + buf + "] ";
}
}  // namespace

void App::capture_status() {
    const std::string* const lines[] = {&status_line_, &meta_status_, &playlist_status_, &cf_status_, &history_status_, &rd_status_,
                                        &creator_status_, &eq_status_, &playlist_export_status_, &playlist_import_status_};
    constexpr size_t n = sizeof lines / sizeof lines[0];
    if (status_seen_.size() != n) status_seen_.assign(n, std::string());
    for (size_t i = 0; i < n; ++i) {
        const std::string& cur = *lines[i];
        if (cur == status_seen_[i]) continue;
        status_seen_[i] = cur;
        if (cur.empty()) continue;
        std::string one = cur;
        for (char& c : one) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        status_history_.push_back(stamp_now() + one);
        if (status_history_.size() > kMaxStatusHistory) status_history_.erase(status_history_.begin(), status_history_.begin() + static_cast<std::ptrdiff_t>(status_history_.size() - kMaxStatusHistory));
    }
}

}  // namespace muisc
