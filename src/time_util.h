// Small date and time helpers shared by the whole program.
#pragma once
#include <chrono>
#include <ctime>

namespace muisc {

// Seconds since the Unix epoch (1970-01-01 UTC), as a signed 64-bit count.
inline long long epoch_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace muisc
