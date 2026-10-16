#pragma once
// The text of the ABOUT APP tab of the player and the radio.
// It is deliberately hard coded (not read from config.txt): the config file used to carry it
// as a ClassTextAboutApp={...}; block, and every save added blank lines around it.
#include <string>
#include <vector>
#include "version.h"

namespace muisc {

inline const std::vector<std::string>& about_app_text() {
    // "v4.2.0" padded to the width of the column, so "Licence" keeps its place in any version.
    static const std::vector<std::string> lines = [] {
        std::string ver = std::string("v") + MOUSIKI_VERSION;
        if (ver.size() < 26) ver.append(26 - ver.size(), ' ');
        else ver += ' ';
        return std::vector<std::string>{
            "Mousiki",
            "",
            "A light weight terminal UI music and radio player for lovers of the terminal <3",
            "",
            "Mousiki Plus ",
            "Windows Port / Modification : Steffen Schwerdtfeger     Github   : StSchwerdtfeger",
            "Email                       : fanti.blub@gmail.com",
            "Version                     : " + ver + "Licence  : Apache licence 2.0",
            "",
            "Original v1.0 ",
            "Developer : ender                         Github   : itzender5820",
            "Email     : itz.ender5820@gmail.com",
            "Version   : original and final v1.0       Licence  : Apache licence 2.0",
            "",
            "Several features and modifications were added along the Win portation, the ",
            "general design remained. Additions: radio mode, playlist menu, meta data ",
            "editor incl. fetch meta data function via AcoustID, stereo mode, loudness ",
            "normalization, listening history, big list/queue/folder overlays, equalizer, ",
            "sleep timer, YX mode oscillator, spectrogram, color themes editor, file ",
            "format conversion, new hot keys, help wizard and then some... ",
            "See Readme.md/manual for full list of additions and changes along the ",
            "Win native port. Done with the help of AI tools.",
        };
    }();
    return lines;
}

} // namespace muisc
