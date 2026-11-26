// Windows device names. CON, PRN, AUX, NUL, COM1-9 and LPT1-9 are not file names there -- with any extension: "con.txt" opens the console, and
// a playlist or an audio file of that name cannot be written or found again. One rule for every place that makes a file name from a title.
#pragma once
#include <string>

namespace muisc {

// `name` = a file name, with or without an extension ("Con", "nul.mp3", "COM3.txt"): is it a device name on Windows?
inline bool is_windows_device_name(const std::string& name) {
    const size_t dot = name.find('.');
    std::string stem = name.substr(0, dot);
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();   // ("con " too: Windows ignores the trailing blanks)
    for (char& c : stem) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return true;
    return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '1' && stem[3] <= '9';
}

// The name made safe: a device name gets an underscore at the end of its stem ("nul.mp3" -> "nul_.mp3").
inline std::string avoid_windows_device_name(std::string name) {
    if (!is_windows_device_name(name)) return name;
    const size_t dot = name.find('.');
    name.insert(dot == std::string::npos ? name.size() : dot, "_");
    return name;
}

} // namespace muisc
