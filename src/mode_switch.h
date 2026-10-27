#pragma once
// Music player <-> radio mode. Both modes live in ONE program, switched with SHIFT and + (the '*' character, a fixed
// key in both modes): a mode ends by returning kExitSwitchMode from its run function and main() starts the other one.
// The radio is destroyed completely on every switch (audio device, threads, caches, terminal state). The player is
// only SUSPENDED while the radio runs (playback paused, its App object kept alive by main()), so coming back to it is
// instant; it really ends when the program quits.
constexpr int kExitSwitchMode = 77;

// There are three modes: the music player (0), the radio (1) and the audio book player (2) -- the player and the audio book
// player are two instances of the same engine (class App). SHIFT and + ('*') goes to the next mode that is switched on
// (Settings -> ON/OFF -> MODES: Mode Order), SHIFT and - ('_') to the previous one. A mode that returns kExitSwitchMode
// says which way in this variable (+1 forward, -1 back); main() picks the next mode with it.
inline int g_mode_switch_dir = 1;
// The keys of the switch, read from config.txt when a mode starts (HKeyModeNext / HKeyModePrev, Settings -> REFERENCE; 0 = none).
inline int g_mode_key_next = '*';
inline int g_mode_key_prev = '_';

// The radio mode (src_radio/radio_main.cpp). Returns 0 on a normal quit, kExitSwitchMode for the mode switch ('*').
int radio_main(int argc, char** argv);
