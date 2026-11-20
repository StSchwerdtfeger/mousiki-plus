// bars_help.h -- one short sentence per spectrum-bars option: what it does, for the preview box of the settings (the music player's
// and the radio's). The key is the option's short name; "" = nothing to say.
#pragma once
#include <string>

namespace muisc {

inline std::string bars_help(const std::string& key) {
    if (key == "onoff") return "The bars under the meta data / station info: on or off.";
    if (key == "style") return "Classic: the block bars, two rows high. Hi-Fi: a finer picture, four rows high, with the options below.";
    if (key == "hifires") return "Fine: every column of the bars is one band. Rough: two columns share one band (calmer, wider bars).";
    if (key == "hifichan") return "Stereo: the left channel above, the right below the middle line. Mono: both channels together.";
    if (key == "hificolor") return "Gradient: the colours of the bars' gradient from left to right. Corresponding: each bar takes the colour of its loudness, as the spectrogram.";
    if (key == "hifiscale") return "How tall the bars may grow: full height down to very low. Lower bars leave more room under the meta data.";
    if (key == "hifiadapt") return "On: the height follows the loudness of the track, so quiet music is as tall as loud music.";
    if (key == "hifisplit") return "Add: a gap between the left and the right channel. Remove: they touch at the middle line.";
    if (key == "hififscale") return "How the pitch is spread over the bars: linear, logarithmic (like the piano), mel, bark or erb (how the ear hears).";
    if (key == "hifigain") return "Raises or lowers every bar by this many dB (a quiet source: more).";
    if (key == "hifirange") return "The dB between the lowest and the highest level the bars show. Wider shows quiet sounds too, narrower is more dramatic.";
    if (key == "behavior") return "Four ready-made ways the bars move: from smooth and slow to quick and sharp. The three values below make your own.";
    if (key == "fluidity") return "How quickly the bars follow the sound: low is smooth and slow, high is quick.";
    if (key == "degradation") return "How quickly the bars fall back when the sound gets quieter: low falls slowly, high drops fast.";
    if (key == "viscosity") return "How much the bars hold on to their level: 0 jumps freely, high makes them thick and sluggish.";
    return std::string();
}

}  // namespace muisc
