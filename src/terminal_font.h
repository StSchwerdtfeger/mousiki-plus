// Mousiki's own terminal font (Settings -> GENERAL -> TEXT AND TRANSLITERATION -> Font; config.txt TerminalFont=, default
// "Cascadia Mono"). The disk and the pictures are braille characters: with a font that has full braille dots they look like the
// screenshots, with one that has none the terminal borrows them from another font and they turn into scattered dots.
//
// The font belongs to the terminal, not to the program in it, so Mousiki can only set it where the terminal offers a way, and
// only for its own window -- the terminal's own settings are never changed:
//   macOS Terminal.app   the font of Mousiki's own window (AppleScript); the old one comes back when Mousiki quits.
//   iTerm2               a profile "Mousiki" (iTerm2 Dynamic Profiles: your profile plus the font) that Mousiki's tab switches
//                        to, and back to its own profile when Mousiki quits.
//   Windows Terminal     a profile "Mousiki" (a Windows Terminal profile fragment) that the Mousiki launchers open; a tab that
//                        was opened another way keeps its font.
//   Windows console      the font of this console window only, restored when Mousiki quits.
//   anything else        not possible from inside: the font has to be chosen in the terminal itself (apply() says so).
#pragma once
#include <string>
#include <vector>

namespace muisc::terminal_font {

inline constexpr const char* kDefaultFont = "Cascadia Mono";       // the default (bundled with the macOS app)
inline constexpr const char* kTerminalDefault = "Terminal Default"; // = leave the terminal's font alone

enum class Host { AppleTerminal, ITerm2, WindowsTerminal, WinConsole, Other };
Host detect_host();
const char* host_name(Host h);

// The monospace fonts installed on this computer (family names, sorted, without duplicates; Windows: TrueType ones). Can take
// a moment (macOS asks the font manager through osascript, Linux runs fc-list): call it when the list is wanted.
std::vector<std::string> list_monospace_fonts();

// Sets `font` for Mousiki's window where this terminal allows it (kTerminalDefault: puts the terminal's own font back). The
// answer is one line for the status bar: what was done, or why nothing could be done. `launcher` is the command the Windows
// Terminal profile starts ("" = this program).
std::string apply(const std::string& font, const std::string& launcher = "");
// The terminal's own font again (at exit). Safe to call when apply() did nothing.
void restore();
// The answer of the last apply() ("" before the first): shown in the FONT overlay.
std::string last_note();

// ---- helpers (exposed for the tests) -------------------------------------------------------------------------------------
std::vector<std::string> parse_fc_list(const std::string& fc_list_output);                 // "Fam A,Fam A Loc\nFam B\n" -> {"Fam A","Fam B"}
std::string wt_fragment_json(const std::string& font, const std::string& commandline, const std::string& icon);
std::string iterm_profile_json(const std::string& postscript_font, int size, const std::string& parent_profile);
std::string guess_postscript_name(const std::string& family);                              // "Cascadia Mono" -> "CascadiaMono-Regular"

} // namespace muisc::terminal_font
