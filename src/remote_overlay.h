#pragma once
// The REMOTE APP overlay (Settings -> ON/OFF -> MODES -> Remote App, player and radio): switch the remote app on / off,
// see the address and a QR code to scan with the phone, change the port and the PIN. One implementation for both modes;
// each mode keeps the values in its own settings (player: Settings, radio: RadioSettings) and passes them in.
#include <functional>
#include <string>
#include <vector>

namespace muisc::remote_ui {

struct Values {               // what the overlay edits
    bool enabled = false;
    int port = 4747;
    std::string pin;
};

struct State {
    bool open = false;
    int row = 0;              // 0 on/off, 1 port, 2 PIN
    bool editing = false;     // typing into the port or the PIN
    std::string buffer;
    std::string status;
};

struct Style {                // colour escapes (already built) and the box glyphs of the mode
    std::string border, border_bottom, header, legend, text, value, change, cursor;
    std::string ul = "╭", ur = "╮", ll = "╰", lr = "╯", h = "─", v = "│";
    // BorderGradient=smooth: the frame colour at t = 0 (top line) .. 1 (bottom line); empty = border / border_bottom
    std::function<std::string(float)> border_at;
};

// The panel as lines of equal visible width (`width` returns it); fits into max_w x max_h when possible (the QR code is
// left out when there is no room for it).
std::vector<std::string> build(const State& st, const Values& val, const Style& s, int max_w, int max_h, int& width);

// One key. Returns true when `val` changed (the caller applies it: the web server, the settings). `close` is set when
// the overlay should close (ESC outside a field). `arrow`: the key was an arrow ('A'..'D').
bool key(State& st, Values& val, int k, bool arrow, bool& close);

// The address the QR code carries: http://<lan address>:<port>/#pin=<pin> (the PIN goes along, so scanning logs in).
std::string scan_url(const Values& val);

} // namespace muisc::remote_ui
