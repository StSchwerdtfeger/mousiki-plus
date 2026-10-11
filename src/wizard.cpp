#include "wizard.h"
#include <sstream>

// ---------------------------------------------------------------------------
// The wizard's text. See wizard.h for the markup ("@H ", "@D ", "@K ", "@A "
// and the <<accent>> spans) and for the kContentWidth limit every line here
// is written to.
//
// The content follows Mousiki_User_Manual/Mousiki_User_Guide.md: when a key or
// a default changes there, it changes here too. Keys are written as their
// DEFAULT bindings -- the wizard teaches the shipped layout, exactly like the
// cheat sheet (?), and Settings -> REFERENCE is where a rebound key is seen.
// ---------------------------------------------------------------------------

namespace wizard {

namespace {

// Greedy word-wrap to `width` columns. The welcome paragraph is plain ASCII
// prose (no markup, no multi-byte characters), so a byte-length count is a
// visible-width count too -- no need for the UTF-8-aware display_width()
// the rest of the renderer uses for hand-drawn lines.
std::vector<std::string> wrap_paragraph(const std::string& text, int width) {
    std::vector<std::string> out;
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (line.empty()) {
            line = word;
        } else if (line.size() + 1 + word.size() <= static_cast<size_t>(width)) {
            line += " " + word;
        } else {
            out.push_back(line);
            line = word;
        }
    }
    if (!line.empty()) out.push_back(line);
    return out;
}

// The welcome page sits to the right of the 30-column disk, so its paragraph
// is wrapped to kWelcomeTextWidth instead of kContentWidth; the renderer
// centres the disk vertically against this whole block (see
// build_wizard_screen() / build_wizard()). Only the first line (the "@H "
// heading) and the last line (the "@K " legend) are coloured -- everything
// in between is ordinary text, same as the rest of the wizard's markup.
std::vector<std::string> make_welcome(const std::string& version) {
    std::vector<std::string> lines;
    // Real guillemets, not the ASCII "<<"/">>" digraph -- those two characters
    // are this file's own accent-span markup (see wizard.h), so writing them
    // literally here would make the renderer swallow them as a span marker.
    lines.push_back("@H Welcome to the Mousiki Plus \xc2\xab HELP WIZARD \xc2\xbb !!!");
    lines.push_back("");

    const std::string para =
        "Mousiki Plus is a terminal UI music and radio player built for lovers of "
        "the terminal. Apart from playing local files, queuing them and creating "
        "playlists, the music player contains the feature to listen and download "
        "online files (YouTube, SoundCloud, Bandcamp), adjust and fetch (AcoustID) "
        "meta data, a synced lyric function (including timing adjustment and a "
        "Karaoke mode!), visualizations of your music (e.g. spectrogram, XY-mode "
        "oscilloscope), an equalizer / sound normalization, sleep timer and a smart "
        "listening history. The radio player mode includes a lot of the features of "
        "the regular music player and lets you listen to a vast variety of online "
        "radio channels that can be added via an online search mode or manually and "
        "organized by creating stations lists. It includes a recording function and "
        "a listening history that contains meta data, so you can search and download "
        "songs you listened to on the radio. Channels can be assigned a preset keys "
        "to tune in to channels fast.";
    for (auto& l : wrap_paragraph(para, kWelcomeTextWidth)) lines.push_back(std::move(l));

    lines.push_back("");
    lines.push_back("Original Mousiki (Linux/macOS/Android):");
    lines.push_back("ender -- github.com/itzender5820");
    lines.push_back("Version 1.0 -- Apache 2.0 license");
    lines.push_back("");
    lines.push_back("Mousiki Plus (Win/Linux/macOS):");
    lines.push_back("StSchwerdtfeger -- github.com/StSchwerdtfeger");
    // `version` comes from MOUSIKI_VERSION (see wizard.h) -- never hardcoded,
    // so this line is correct after any release without touching this file.
    lines.push_back("Version " + version + " -- Apache 2.0 license");
    lines.push_back("");
    lines.push_back("@K [->] Table of content   [SHIFT+w] Close / Open Help Wizard; [?] Cheat Sheet");
    return lines;
}

// --- shared topics ---------------------------------------------------------
// Visualisations, sound and the sleep timer are the same program code in both
// modes, so both topic lists use these.

Topic topic_visuals() {
    Topic t;
    t.title = "Visualisations";
    t.blurb = "sphere, oscilloscope, spectrogram";
    t.pages.push_back({{
        "@H Three visuals, one key",
        "",
        "The player draws them in the <<lyrics area>>, the radio in the",
        "<<scope block>> right of the station info. The same key cycles",
        "through them in both modes:",
        "",
        "@D   ┌─────────┐   ┌────────┐   ┌─────────────┐   ┌────────────┐",
        "@D   │ lyrics  │ > │ sphere │ > │ oscilloscope│ > │ spectrogram│",
        "@D   └─────────┘   └────────┘   └─────────────┘   └────────────┘",
        "@A        ^ player only              and back to the start",
        "",
        "@K   [.]  cycle the lyrics area / scope block",
        "",
        "A view switched off under <<Settings -> ON/OFF>> (Use Lyrics,",
        "Use Oscilloscope, Use Spectrogram, Use Sphere) is skipped by [.].",
        "The order is set there too (Viz Order 1..4). The choice is",
        "remembered as Lyrics/Viz (radio: Osci/sphere).",
        "",
        "The <<spectrogram is the default>>. All three analyse the decoded",
        "signal, so volume, equalizer and normalization never change",
        "the picture.",
    }});
    t.pages.push_back({{
        "@H The XY oscilloscope",
        "",
        "Like a real scope in X-Y mode: the <<left channel moves the beam>>",
        "<<sideways, the right channel up and down>>. Wide stereo fills",
        "the area, a phase-shifted tone draws an ellipse. A nearly mono",
        "track fades over to a phase portrait instead of a flat line.",
        "",
        "@K   [SHIFT+o]  the tuning overlay, live while the music plays",
        "",
        "Rows worth knowing:",
        "",
        "@D   Osci style        braille (any terminal) / image (pixels)",
        "@D   Osci music mode   for oscilloscope music: the unprocessed",
        "@D                     signal and a fixed scale",
        "@D   Frame rate        30 / 45 / 60 / 90 / 120 / 165 fps",
        "@D   Decay             the afterglow; higher = longer trails",
        "@D   Tail / Glow       bright head, fading tail / line glow",
        "@D   Z-Axis            beam brightness follows speed or level",
        "@D   Color             the VIZ gradient or 7 palettes",
        "",
        "@A   Each style keeps its OWN values -- switching braille <-> image",
        "@A   never loses your tuning. [R] resets only the style in use.",
        "",
        "Starting points for braille: crisp scope = Decay 0.65, Dot",
        "threshold 0.35, Tail 0.60. Dreamy glow = 0.90, 0.20, 0.25.",
    }});
    t.pages.push_back({{
        "@H The spectrogram",
        "",
        "Made the way <<Audacity>> makes its spectrogram view, with",
        "Audacity's defaults: a 2048-sample Hann window, zero padding 2,",
        "the Mel scale from 0 to 20 000 Hz, gain 20 / range 80 dB and the",
        "Roseus colour map. Time runs left to right, frequency upwards,",
        "the colour is the strength. Stereo is drawn as two channels.",
        "",
        "@K   [SHIFT+i]  options      [SHIFT+u]  full screen",
        "",
        "@D   Scale        linear / logarithmic / mel / bark / erb / period",
        "@D   Motion       sweep (a page at a time) / scroll (newest right)",
        "@D   Window size  256 .. 8192 -- bigger = sharper frequency lines,",
        "@D                smaller = crisper clicks and drum hits",
        "@D   Time span    5 .. 60 s across the width",
        "@D   Colors       roseus / classic / grayscale / your VIZ gradient",
        "",
        "[R] in the overlay puts everything back to Audacity's defaults.",
        "A change is applied to the last minute of audio at once, so you",
        "see the difference immediately.",
        "",
        "Full screen adds <<frequency labels>> on the left like Audacity's",
        "ruler and a <<time ruler>> on top.",
    }});
    t.pages.push_back({{
        "@H Their own windows (drawn by the graphics card)",
        "",
        "Both visuals can leave the terminal and open a real window,",
        "redrawn at your <<monitor's refresh rate>> (60, 120, 165 Hz ...):",
        "",
        "@K   [)]  scope window         German SHIFT+9, English SHIFT+0",
        "@K   [(]  spectrogram window   German SHIFT+8, English SHIFT+9",
        "",
        "@D   ┌────────────────────────────┐",
        "@D   │                            │   every sample drawn once,",
        "@D   │        the beam            │   phosphor afterglow, bloom",
        "@D   │                            │   around the beam, and",
        "@D   │                            │   smooth curves between the",
        "@D   └────────────────────────────┘   samples",
        "",
        "@K   In the window:  [F] or [F11] fullscreen   [T] always on top",
        "@K                   [ESC] or [Q] close",
        "",
        "Each window is a <<separate process>> that gets the audio through a",
        "pipe, so a graphics driver problem can only close the window --",
        "never stall the music or the terminal.",
        "",
        "@A   They need SDL2. The Windows packages include SDL2.dll; on",
        "@A   Linux/macOS install it (apt: libsdl2-2.0-0, brew: sdl2).",
        "@A   Position, size and fullscreen are remembered, and the",
        "@A   windows stay open when you switch player <-> radio.",
    }});
    return t;
}

Topic topic_sound() {
    Topic t;
    t.title = "Sound";
    t.blurb = "equalizer, loudness, stereo/mono";
    t.pages.push_back({{
        "@H The 10-band equalizer",
        "",
        "@K   [SHIFT+e]  open it -- the sound changes while you listen",
        "",
        "@D    +12 dB ┐",
        "@D           │  ▁  ▃  ▅  █  ▅  ▃  ▁  ▁  ▃  ▅",
        "@D     0 dB  ┼──────────────────────────────────",
        "@D           │",
        "@D    -12 dB ┘  31 62 125 250 500 1k 2k 4k 8k 16k  Hz",
        "@A                        ^ the selected band is highlighted",
        "",
        "@K   [<-] [->]   pick a band          [UP] [DOWN]  +/- 1 dB",
        "@K   [,] [.]     previous / next preset   [0]  band to 0 dB",
        "@K   [SPACE]     equalizer on / off    [R]  back to Flat",
        "@K   [S]         save the curve as your own preset (up to 24)",
        "@K   [DEL]       delete the selected custom preset (twice)",
        "",
        "Twelve presets are built in (Flat, Bass Boost, Treble Boost,",
        "Vocal, Rock, Pop, Jazz, Classical, Electronic, Hip-Hop,",
        "Acoustic, Loudness), then come your own.",
        "",
        "@A   <<Preamp>>: boosting bands would clip, so the level is lowered",
        "@A   by the height of your loudest point (e.g. -7.1 dB for Bass",
        "@A   Boost). A strong boost is therefore slightly quieter -- turn",
        "@A   the volume up. The EQ is bypassed completely when it is off.",
        "",
        "The radio keeps its own curve and presets in radio_config.txt.",
    }});
    t.pages.push_back({{
        "@H Loudness normalization",
        "",
        "Plays every track at the same perceived loudness, so a quiet",
        "album does not disappear after a loud one.",
        "",
        "@K   [v]        on / off, without opening anything",
        "@K   [SHIFT+v]  the overlay: on/off, target level, max boost",
        "",
        "@D   Normalize      off / on                        (default on)",
        "@D   Target level   -40 .. 0 LUFS                   (default -16)",
        "@D   Max boost      0 .. 24 dB                      (default 9)",
        "",
        "-16 LUFS leaves headroom, -14 matches YouTube and Spotify. The",
        "grey <<live line>> under the rows shows the playing track's measured",
        "loudness and the gain actually applied, so you can see at once",
        "when Max boost is holding a quiet track back.",
        "",
        "@A   In the radio a stream has no end, so the loudness is measured",
        "@A   <<since you tuned in>> and the gain glides in over a second.",
        "",
        "@H Stereo or mono",
        "",
        "<<Settings -> ON/OFF -> Stereo Sound>>. Off folds left and right",
        "together (and halves the memory a loaded track needs). Turning",
        "it off is immediate; turning it on applies from the next track.",
        "In the radio the dial's footer then reads MONO.",
    }});
    t.pages.push_back({{
        "@H Sample rate: nothing is converted on the way",
        "",
        "Every file plays at <<its own sample rate>> (44.1, 48, 88.2,",
        "96 kHz ...): it is decoded at that rate and the audio device is",
        "opened with it. Above 96 kHz the track is brought down to",
        "96 kHz. Where a conversion cannot be avoided it uses a long,",
        "sharp filter (a 64-tap aresample), not a cheap linear one.",
        "",
        "@H Long tracks need little memory",
        "",
        "@D   short track    ──>  decoded completely into memory",
        "@D   long mix       ──>  a 2-minute window around the position",
        "@A                       90 s ahead, the last 30 s kept, so a",
        "@A                       short step back is instant",
        "",
        "A 200 MB file takes about <<50 MB>> instead of gigabytes. The audio",
        "is exactly the same -- same rate, same 32-bit samples.",
        "",
        "@H The order things happen in",
        "",
        "@D   decoded audio > equalizer > normalization > limiter > volume",
        "@A                 ^",
        "@A                 the scopes can tap the signal here, before all",
        "@A                 of it: Osci music mode in the [SHIFT+o] overlay",
    }});
    return t;
}

Topic topic_sleep() {
    Topic t;
    t.title = "Sleep mode";
    t.blurb = "fall asleep to music, with a fade-out";
    t.pages.push_back({{
        "@H The sleep timer",
        "",
        "@K   [SHIFT+z]  open the overlay (playback keeps running)",
        "",
        "@D   ┌─────────────────────────────┐",
        "@D   │  15 minutes                 │",
        "@D   │  30 minutes                 │",
        "@D   │  60 minutes                 │",
        "@D   │  90 minutes                 │",
        "@D   │  120 minutes                │",
        "@D   │  Stop after current song    │ <<-- player only>>",
        "@D   │  Fade out            [on]   │ <<-- a toggle, stays open>>",
        "@D   │  Off                        │",
        "@D   └─────────────────────────────┘",
        "",
        "@K   [UP] [DOWN]  pick      [ENTER]  set it and close",
        "@K   [ESC] or [SHIFT+z]     close without changing anything",
        "",
        "While a timer is armed the remaining time is shown in the",
        "title of the search bar: <<SEARCH LOCAL  [SLEEP 24:10]>>.",
        "",
        "@A   <<Fade out>> (on by default) glides the volume down over the",
        "@A   last 10 % of the time -- at least 30 s, at most 10 minutes --",
        "@A   so the music fades away instead of stopping abruptly.",
        "",
        "When a minute timer runs out the player <<pauses>> (the position is",
        "kept, [p] resumes) and the radio <<stops>> the stream. The timer",
        "never changes the play mode, and it is gone after a restart.",
    }});
    return t;
}

// --- music player topics ---------------------------------------------------

Topic topic_main_ui() {
    Topic t;
    t.title = "Main UI";
    t.blurb = "the screen, the queue and the big overlays";
    t.pages.push_back({{
        "@H The main screen",
        "",
        "@D ┌──────────────────────────────────────────────────────────────┐",
        "@D │  ▓▓▓▓   ARTIST  TITLE  ALBUM        lyrics, sphere, scope    │",
        "@D │  ▓▓▓▓   (metadata panel)            or spectrogram           │",
        "@D ├──────────────────────────────────────────────────────────────┤",
        "@D │  ████████████░░░░░░░░░░░  1:23 / 3:45     waveform           │",
        "@D │  volume  ##########------                                    │",
        "@D ├──────────────────────────────────────────────────────────────┤",
        "@D │  SEARCH LOCAL                                      [ S ]     │",
        "@D ├───────────────────────────────┬──────────────────────────────┤",
        "@D │  LOCAL AUDIO FILES            │  QUEUE (locked)              │",
        "@D │  > a track                    │  1. plays next               │",
        "@D │    another track              │  2. then this one            │",
        "@D └───────────────────────────────┴──────────────────────────────┘",
        "@A      └─ your music folders         └─ plays BEFORE the list",
        "@A                                       continues",
        "",
        "@A   The box right of the search bar is the <<play mode letter>>:",
        "@A   L list, R repeat, S shuffle, O stop, Q queue then stop.",
        "@A   [m] steps through them.",
        "",
        "@K   [TAB]  move the focus between the list and the queue --",
        "@K          it decides where [UP]/[DOWN], [a], [d] and [4]/[5] act",
        "@K   [ENTER] play      [p] play/pause     [n] / [b] next / prev",
        "@K   [<-] [->] seek 5 s    [+] / [-] volume    [x] mute",
    }});
    t.pages.push_back({{
        "@H Where your music comes from",
        "",
        "Mousiki plays what it finds in the folders you point it at:",
        "",
        "@K   [s]  Settings  ->  <<PATHS>>  ->  LOCAL PATH",
        "",
        "@D   LOCAL PATH",
        "@D     Local Path 1 : /home/you/Music",
        "@D     Local Path 2 : /mnt/archive/albums",
        "@D     + new path                     <<-- ENTER adds a folder>>",
        "",
        "Move onto a row, press [ENTER], type the folder, [ENTER] again.",
        "Committing a change <<rescans the library at once>>. Paths accept",
        "~ and %USERPROFILE% and both slash directions on Windows.",
        "An empty line plus [ENTER] removes that path.",
        "",
        "@A   Remember to leave Settings with [s] -- that is what writes",
        "@A   config.txt. [ESC] or [q] puts everything back as it was.",
        "",
        "@K   [SHIFT+r]  rescan the library later (after copying files in)",
        "",
        "@H Finding your way around the list",
        "",
        "@K   [f] filter to the folder of the highlighted track  [c] clear",
        "@K   [SHIFT+t]  sort: folder order > title A-Z > artist A-Z",
        "@K   [SHIFT+n]  rows show the title tag instead of the file name",
        "@K   [r] redraw the screen      [t] the console / log view",
    }});
    t.pages.push_back({{
        "@H Building the queue",
        "",
        "The queue plays <<before>> the normal list continues.",
        "",
        "@K   With the LIST focused:",
        "@K     [a]  put the highlighted track at the FRONT (plays next)",
        "@K     [e]  add it to the END",
        "@K   With the QUEUE focused ([TAB]):",
        "@K     [d]  remove          [4] / [5]  move up / down",
        "@K     [$] / [%]  move to the very top / bottom",
        "@K     [a]  the bulk-add panel: paste a YouTube playlist link",
        "",
        "@K   [!]  lock / unlock      [SHIFT+x]  clear it (asks first)",
        "@K   [CTRL+SHIFT+z]  undo the last clear (one level)",
        "@K   [CTRL+SHIFT+u]  save the queue as a playlist",
        "",
        "@A   The queue is <<locked by default>>: a played track moves to the",
        "@A   end instead of leaving, so the queue loops rather than",
        "@A   draining. The title then reads QUEUE (locked). Press [!] to",
        "@A   unlock it and let played tracks disappear.",
    }});
    t.pages.push_back({{
        "@H The two big overlays",
        "",
        "@K   [SHIFT+l]  the LIST with many more rows",
        "@K   [SHIFT+k]  the QUEUE with many more rows",
        "@K   [SHIFT+UP] / [SHIFT+DOWN]  jump a whole page",
        "@K   [ESC] or the same key again  close",
        "",
        "Both share the cursor with the small pane, and everything else",
        "keeps working from inside them. Only one can be open at a time.",
    }});
    return t;
}

Topic topic_search() {
    Topic t;
    t.title = "Search and Download";
    t.blurb = "local, YouTube, SoundCloud, Bandcamp";
    t.pages.push_back({{
        "@H One search box, six kinds of search",
        "",
        "@K   [/]  open it (German SHIFT+7). The prefix picks the source:",
        "",
        "@D   (nothing)   your local library      live, as you type",
        "@D   s:  query   YouTube                 on ENTER",
        "@D   sc: query   SoundCloud              on ENTER",
        "@D   b:  query   Bandcamp                on ENTER",
        "@D   p:  query   your saved playlists    live",
        "@D   f:  query   your folders            live",
        "",
        "@A   The local search is <<fuzzy>>: \"X Files\" also finds \"X-Files\".",
        "@A   It matches the file name, the artist, the title tag and the",
        "@A   album, and ranks purely by how well things match.",
        "",
        "@K   [UP] / [DOWN] while typing, then [ENTER]",
        "@K        plays the highlighted result <<right away>>",
        "@K   [ENTER] without moving  keeps the search as the filter",
        "@K   [ESC]   cancel and restore the previous view",
        "",
        "On a <<playlist>> row (p:) [ENTER] queues all of its tracks. On a",
        "<<folder>> row (f:) it opens that folder in the list, like [f].",
        "",
        "Online searches only run on [ENTER], never per keystroke.",
        "SoundCloud's 30-second previews and unplayable tracks are left",
        "out of the results, which is why it takes a few seconds longer.",
    }});
    t.pages.push_back({{
        "@H Saving what you are streaming",
        "",
        "@K   [y]  save the streamed track that is playing right now",
        "",
        "@D   YouTube    ──>  Opus",
        "@D   SoundCloud ──>  MP3 / Opus / AAC, as the site sends it",
        "@D   Bandcamp   ──>  the free 128 kbit/s MP3 stream",
        "@A              no second conversion -- the format is kept",
        "",
        "The file is named <<Title - Artist.ext>>, the cached copy is",
        "removed, and the library refreshes so it appears as a local",
        "track straight away.",
        "",
        "@H Where downloads go",
        "",
        "@K   [s]  Settings  ->  <<PATHS>>  ->  DOWNLOAD PATH",
        "",
        "Until you set one, Mousiki uses its cache folder",
        "(~/.cache/mousiki). The download folder is <<scanned automatically>>,",
        "so you never have to repeat it as a local path. The radio puts",
        "its recordings in the same folder by default.",
        "",
        "@A   Status messages you will see:",
        "@A     saved to <path>        it worked",
        "@A     already saved: <name>  that name is already there",
        "@A     not a cached stream    this is a local file already",
    }});
    return t;
}

Topic topic_meta() {
    Topic t;
    t.title = "Meta Data";
    t.blurb = "tags, renaming and the AcoustID lookup";
    t.pages.push_back({{
        "@H The meta editor",
        "",
        "@K   [SHIFT+m]  open it",
        "",
        "@D ┌─────────────────────────────┬──────────────────────────────┐",
        "@D │ SEARCH                      │  [EDIT]   FETCH LIST         │",
        "@D ├─────────────────────────────┼──────────────────────────────┤",
        "@D │ > track_01.mp3              │  FILE   : track_01.mp3       │",
        "@D │   track_02.mp3              │  ARTIST : ...                │",
        "@D │   track_03.mp3              │  TITLE  : ...                │",
        "@D │                             │  ALBUM  : ...                │",
        "@D │                             │  YEAR   : ...                │",
        "@D └─────────────────────────────┴──────────────────────────────┘",
        "@A    └─ the library                 └─ the five fields of the",
        "@A                                      highlighted file",
        "",
        "@H Nothing touches your files until you say so",
        "",
        "@A   Every change goes into a <<pending edit session>>, autosaved",
        "@A   after each keystroke. Closing the editor, quitting, even a",
        "@A   crash keeps it. Touched fields and their library rows are",
        "@A   drawn in the header colour, so you can see what will be",
        "@A   written. <<Only [CTRL+s] writes to the files>>, and it asks.",
        "",
        "@K   [TAB] search > library > fields   [UP]/[DOWN] move",
        "@K   [ENTER] start editing / finish a field",
        "@K   [CTRL+s] apply everything    [CTRL+SHIFT+x] discard it all",
        "@K   [<-] [->] switch the tab (or move the caret while typing)",
    }});
    t.pages.push_back({{
        "@H Finding the files that need work",
        "",
        "@K   With the library pane focused:",
        "@K     [x]        files missing ANY metadata",
        "@K     [SHIFT+t]  files missing a title",
        "@K     [SHIFT+a]  files missing an artist",
        "@K     [SHIFT+y]  files missing a year",
        "@K     [SHIFT+l]  files missing an album",
        "@K     [r]        edited files on top",
        "@K     [SHIFT+p]  play the hovering track",
        "@K   Press the same key again to clear the filter.",
        "",
        "@A   Between them these cover the usual tidy-up: find what is",
        "@A   missing, fix it, and see at a glance what you touched.",
    }});
    t.pages.push_back({{
        "@H Letting AcoustID fill the gaps",
        "",
        "AcoustID recognises a track by an <<audio fingerprint>>, so it",
        "works even when the file has no usable tags at all.",
        "",
        "@D   one title      [SHIFT+b]   ──> fills in the missing tags",
        "@D                  [SHIFT+n]   ──> fetches all, overwrites",
        "@D                              ([SHIFT+b] also on the main list)",
        "@D   many titles    [a]         ──> onto the FETCH LIST",
        "@D                  [SHIFT+f]   ──> the whole filtered pane",
        "@D                  [ENTER] on that tab fetches the full list",
        "@D                  [SHIFT+c] there cancels a running fetch",
        "@A   [DEL] takes a title off the fetch list again -- it never",
        "@A   deletes the file and never touches your pending edits.",
        "",
        "@A   On the FETCH LIST tab, tick what is fetched: <<missing only>>",
        "@A   (default, empty tags only), <<all>>, or any mix of artist,",
        "@A   title, album, year. [TAB] [<-] [->] [SPACE]; [ENTER] fetches.",
        "@A   Album and year come from MusicBrainz; if there are several",
        "@A   candidates [ENTER] on the field lets you choose.",
        "@A   A disclaimer comes first. Answer [y], [n] or [ESC].",
        "@A   Results land in the <<edit session>>, never straight in the",
        "@A   files, and a low-confidence match is reported as \"no match\"",
        "@A   rather than written as a wrong tag.",
        "",
        "Applying writes the tags <<without re-encoding>> the audio; a file",
        "name edit becomes a plain rename.",
    }});
    return t;
}

Topic topic_playlist() {
    Topic t;
    t.title = "Playlists";
    t.blurb = "build them, save them, export them";
    t.pages.push_back({{
        "@H The playlist editor",
        "",
        "@K   [SHIFT+p]  open it -- two tabs, [<-] / [->] switch them",
        "",
        "@D ┌──────────────────────────────────────────────────────────────┐",
        "@D │ Name: my evening mix        [CREATE/EDIT]  SAVED PLAYLISTS   │",
        "@D ├───────────────────────────────┬──────────────────────────────┤",
        "@D │ SEARCH / LIBRARY              │  THE PLAYLIST                │",
        "@D │ > a track                     │  1. first track              │",
        "@D │   another track               │  2. second track             │",
        "@D └───────────────────────────────┴──────────────────────────────┘",
        "@A    └─ [ENTER] adds the                └─ [4]/[5] reorder,",
        "@A       highlighted track                  [D] removes",
        "",
        "@K   [TAB]     name field > library picker > track list",
        "@K   [CTRL+s]  save -- from every pane, also while typing",
        "@K   [ESC]     leave a text box; again: leave the editor",
        "",
        "Typing in the library pane filters it live. On the second tab,",
        "<<SAVED PLAYLISTS>>, [ENTER] loads one back into the editor and",
        "[DEL] deletes it after a Yes/No question.",
        "",
        "@A   Playlists hold <<local files only>>. [CTRL+SHIFT+u] on the main",
        "@A   screen turns the current queue into a playlist and leaves",
        "@A   streamed items out.",
        "",
        "Use them from the main screen with <<p:>> in the search box --",
        "[ENTER] on a playlist row queues all of its tracks.",
    }});
    t.pages.push_back({{
        "@H Where playlists live",
        "",
        "@K   [s]  Settings  ->  <<PATHS>>  ->  PLAYLIST PATH",
        "",
        "One folder holds them all. Without one, the first local path",
        "plus /playlists is used. Changing the path <<copies>> the existing",
        "playlists to the new folder -- nothing is moved or deleted.",
        "",
        "@H Exporting to M3U8 / M3U",
        "",
        "On the SAVED PLAYLISTS tab, select one and press [e]:",
        "",
        "@D   ┌──────────────────────────────────────────────┐",
        "@D   │  Folder : /home/you/Music/exports            │",
        "@D   │  Format : M3U8                               │",
        "@D   │  File   : my evening mix.m3u8                │",
        "@D   └──────────────────────────────────────────────┘",
        "@A        └─ starts at PATHS -> PLAYLIST EXPORT PATH, and can",
        "@A           be changed here just for this one export",
        "",
        "@K   [TAB] or [UP]/[DOWN]  Folder <-> Format",
        "@K   [<-] [->] or [SPACE]  M3U8 <-> M3U",
        "@K   [ENTER] or [CTRL+s]   export      [ESC]  cancel",
        "",
        "The file is an <<extended M3U>>: #EXTM3U, the playlist name, and",
        "per track an #EXTINF line followed by its absolute path. Both",
        "formats are written as UTF-8. Tracks whose file is gone are",
        "left out, and the status line says how many were exported.",
    }});
    return t;
}

Topic topic_lyrics() {
    Topic t;
    t.title = "Lyrics";
    t.blurb = "synced lyrics, retry and the timing overlay";
    t.pages.push_back({{
        "@H Synced lyrics",
        "",
        "When the lyrics engine is on, Mousiki fetches the lyrics of the",
        "playing track and follows along line by line -- word by word",
        "where the source has word timings.",
        "",
        "@D   ┌──────────────────────────────────────────┐",
        "@D   │   a line that was already sung           │  <<dimmed>>",
        "@D   │                                          │",
        "@D   │   the line being sung right now          │  <<active>>",
        "@D   │                                          │",
        "@D   │   the line that comes next               │  <<dimmed>>",
        "@D   └──────────────────────────────────────────┘",
        "",
        "@K   [.]  cycle the area: lyrics > sphere > scope > spectrogram",
        "@K   [k]  the karaoke overlay (its own topic in this wizard)",
        "",
        "Colours come from <<Settings -> COLORS -> LYRICS>> (inactive line,",
        "active line, active word), the alignment and the reveal style",
        "from <<Settings -> ANIMA/VIZ>> (Lyrics Alignment, Lyrics",
        "Animation: full, word by word, line by line, letter by letter,",
        "active line only, active word only).",
        "",
        "@A   <<Settings -> ON/OFF -> Use Lyrics = false>> switches lyrics off",
        "@A   entirely: nothing is ever fetched and [.] skips them.",
    }});
    t.pages.push_back({{
        "@H When the match is wrong: retry ([l])",
        "",
        "The automatic lookup goes by what the file says. When that is",
        "wrong or incomplete, ask again by hand:",
        "",
        "@K   [l]  open the retry form (lyrics engine must be on)",
        "",
        "@D   ┌──────────────────────────────────────────────┐",
        "@D   │  TITLE  : ...                                │",
        "@D   │  ARTIST : ...                                │",
        "@D   │  [ ] slowed      [ ] ultra-slowed            │",
        "@D   │  [ ] sped up     [ ] reverb                  │",
        "@D   │  [ ] remix       [ ] other                   │",
        "@D   └──────────────────────────────────────────────┘",
        "@A        └─ the ticks help with edits whose timing differs from",
        "@A           the original. Slowed, ultra-slowed and sped up",
        "@A           exclude each other.",
        "",
        "@K   [TAB] or [DOWN]  next field     [UP]  previous field",
        "@K   [SPACE]  tick or untick         [ENTER]  fetch with these",
        "@K   [ESC]    cancel",
        "",
        "Fetching again <<replaces the lyrics file>> for that track -- and",
        "with it any timing offset you had saved (see the next page).",
    }});
    t.pages.push_back({{
        "@H Lyrics running early or late: the timing overlay",
        "",
        "@K   [ALT+L]  open it -- on the main screen AND in karaoke mode",
        "",
        "@D   ┌──────────────────────────────────────────────┐",
        "@D   │  LYRICS TIMING            offset  +0.3 s     │",
        "@D   │  ─────────────────────────────────────────   │",
        "@D   │    the line before                           │",
        "@D   │  > the line being sung                       │",
        "@D   │    the line after                            │",
        "@D   └──────────────────────────────────────────────┘",
        "@A        └─ these three move live while you adjust, so you can",
        "@A           judge the offset by ear against the music",
        "",
        "@K   [->] / [<-]   lyrics later / earlier by 0.1 s",
        "@K   [UP] / [DOWN] lyrics later / earlier by 0.5 s",
        "@K   [R]           back to 0        range: +/- 120 s",
        "@K   [ENTER] or [S]  save it into the track's lyrics file",
        "@K   [ESC] or [ALT+L]  cancel -- the old offset comes back",
        "",
        "@A   A <<positive>> offset makes the lyrics appear later, so use it",
        "@A   when they are too early. Saving writes a standard",
        "@A   [offset:...] tag into the .lrc file, which other players",
        "@A   understand too; the timestamps themselves are not changed.",
        "",
        "It opens only while <<synced>> lyrics are loaded. Opened from the",
        "karaoke overlay it returns there when you close it.",
    }});
    t.pages.push_back({{
        "@H Reading the lyrics, and translating them",
        "",
        "@K   [CTRL+L]  open READ LYRICS (playing track, else the hovered one)",
        "",
        "@D   ┌─ READ LYRICS ◀ ────────────────────────────────────────┐",
        "@D   │ ◀ ● Fetched 2026-10-11 (lrclib) | Earlier 2026-01-01 ▶ │",
        "@D   └─ 3 lyrics files ───────────────────────────────────────┘",
        "@D   ┌─ TRANSLATION ──────────────────────────────────────────┐",
        "@D   │ ● English (Google Translate) | French (MyMemory)       │",
        "@D   └─ 2 translations ───────────────────────────────────────┘",
        "@D   ┌─ Artist - Title ───────────────────────────────────────┐",
        "@D   │ Guten Morgen, liebe Sonne                              │",
        "@D   │ (Good morning, dear sun)                               │",
        "@D   └────────────────────────────────────────────────────────┘",
        "",
        "@K   [TAB]      switch between the two top panes",
        "@K   [<-] / [->] choose the file / the translation",
        "@K   [UP] [DOWN] [SPACE] [b] [HOME] [END]  scroll",
        "@K   [t]  <<translate overlay>>: service (Google / MyMemory),",
        "@K        translate into, lyrics are in -- [ENTER] fetches",
        "@K   [SHIFT+t]  show / next / hide translations (never fetches)",
        "@K   [d] / [DEL]  delete a translation (TRANSLATION pane)",
        "",
        "@A   A translation takes a while and goes on in the background;",
        "@A   asking for another one meanwhile only gives a warning.",
    }});
    return t;
}

Topic topic_karaoke() {
    Topic t;
    t.title = "Karaoke";
    t.blurb = "the lyrics over the whole screen, in 5 sizes";
    t.pages.push_back({{
        "@H The karaoke overlay",
        "",
        "@K   [k]  open it   --   [ESC] or [k] close",
        "",
        "@D ┌─ artist - title ─────────────────────────────────────────────┐",
        "@D │  ▓▓▓▓                                             ▓▓▓▓       │",
        "@D │  ▓▓▓▓       a line already sung                   ▓▓▓▓       │",
        "@D │  ▓▓▓▓                                             ▓▓▓▓       │",
        "@D │  ▓▓▓▓    THE LINE BEING SUNG NOW                  ▓▓▓▓       │",
        "@D │  ▓▓▓▓                                             ▓▓▓▓       │",
        "@D │  ▓▓▓▓       the line that comes next              ▓▓▓▓       │",
        "@D └─────────────────────────── [ 1:23 / 3:45 ] ──────────────────┘",
        "@A     └─ the karaoke picture, on both sides, drawn in your DISK",
        "@A        colours with a wave running across it",
        "",
        "The words already sung are drawn in the active-word colour and",
        "the word being sung is underlined. Lines are centred, with an",
        "empty row between them.",
        "",
        "@A   The overlay <<always>> uses the lyrics engine -- also when it is",
        "@A   off on the main screen. It fetches the lyrics itself then,",
        "@A   and again after a track change, without changing that",
        "@A   setting, so your main screen keeps its visual.",
    }});
    t.pages.push_back({{
        "@H Making the lyrics bigger",
        "",
        "@K   [*] / [_]   size 1 to 5",
        "@K               German:  SHIFT and +  /  SHIFT and -",
        "@K               English: SHIFT+8      /  SHIFT and -",
        "",
        "@D   size 1   normal terminal text",
        "@D   size 2   3 x 2 cells per letter   ┐  Mousiki's own pixel",
        "@D   size 3   6 x 4 cells per letter   │  font, drawn in",
        "@D   size 4   9 x 6 cells per letter   │  braille dots",
        "@D   size 5   12 x 8 cells per letter  ┘",
        "",
        "Only the lyrics change size; the pictures stay as they are.",
        "Long lines wrap between words. The bottom border briefly shows",
        "<<[ lyrics size n / 5 ]>>, and the size is saved when you close.",
        "",
        "@A   The font covers the Latin letters, digits, punctuation, the",
        "@A   German umlauts and ss, and the common accented letters. A",
        "@A   line with characters it does not have (Japanese, emoji ...)",
        "@A   is shown in normal text instead.",
    }});
    t.pages.push_back({{
        "@H Karaoke: the keys that keep working",
        "",
        "@K   [p] play/pause   [n] / [b] next / previous   [#] shuffle",
        "@K   [<-] [->] seek   [+] / [-] volume   [x] mute   [m] mode",
        "@K   [v] normalization",
        "@K   [ALT+L]  the <<lyrics timing overlay>>, right here over the big",
        "@K            lyrics -- the easiest place to judge an offset",
        "",
        "On a narrow terminal the pictures are left out and the lyrics",
        "take the whole width.",
    }});
    return t;
}

Topic topic_settings() {
    Topic t;
    t.title = "Settings";
    t.blurb = "all six tabs, and how saving works";
    t.pages.push_back({{
        "@H Settings: six tabs",
        "",
        "@K   [s]  open them (always on the first tab)",
        "",
        "@D   [COLORS]  ON/OFF  ANIMA/VIZ  PATHS  REFERENCE  ABOUT APP",
        "@A    ^ [TAB] steps through them, wrapping back to COLORS",
        "",
        "@K   [UP] / [DOWN]  previous / next row",
        "@K   [<-] / [->]    step through a row's values, applied at once",
        "@K   [ENTER]        edit the value as free text",
        "@K   [s]            <<save to config.txt and close>>",
        "@K   [ESC] or [q]   <<discard everything>> and close",
        "",
        "@A   Every change is <<live>>: you see and hear it while you are",
        "@A   still in Settings. But only [s] writes it down. Leaving with",
        "@A   [ESC] puts every row back the way it was when you opened",
        "@A   Settings -- paths included.",
        "",
        "While there are unsaved changes, a green note under the footer",
        "says so.",
        "",
        "@H Text editing, in every field here",
        "",
        "@K   [<-] [->] caret    [SHIFT+<-] [SHIFT+->] mark",
        "@K   [HOME] [END] jump  [DEL] [BACKSPACE] delete",
        "@K   [CTRL+c] [CTRL+x] [CTRL+v]  copy, cut, paste",
        "@K   [ENTER] confirm    [ESC] cancel the edit",
        "",
        "[TAB] and the arrows never leave a field being edited.",
    }});
    t.pages.push_back({{
        "@H Tab 1: COLORS",
        "",
        "<<COLORS>> -- 16 rows, each with one or two cells, a swatch and a",
        "live preview on the right. Values are numbers from the",
        "256-colour terminal palette (1-255); 0 or empty means \"no",
        "colour\", so the terminal's own default is used.",
        "",
        "@D   BORDER_COLOR  TOP / BOTTOM     the frame lines",
        "@D   DISK          TOP / BOTTOM     the spinning disk",
        "@D   METADATA      KEY / VAL        labels and values",
        "@D   VIZ           LEFT / RIGHT     visualizer, sphere, gradients",
        "@D   PROGRESS_BAR  PLAYED / PENDING the progress bar",
        "@D   LIST / QUEUE  FG / BG          per row state: inactive,",
        "@D                                  playing, cursor",
        "@D   LYRICS        FG / BG          inactive line, active line,",
        "@D                                  active word",
        "@D   HEADER / LEGEND / TAB_NAMES    headings, key hints, tabs",
        "",
        "@K   [<-] / [->] pick the cell, then [ENTER] to type a number",
    }});
    t.pages.push_back({{
        "@H Tab 2: ON/OFF",
        "",
        "Two categories, every row changed with [<-] / [->]:",
        "",
        "<<MODES>>",
        "@D   Playback        list / loop / shuffle / stop / queue",
        "@D                   (same as the [m] key; `loop` is *repeat*)",
        "@D   Start Mode      player / radio: which of the two Mousiki",
        "@D                   opens with. Stored as StartMode= in",
        "@D                   config.txt and shared with the radio.",
        "",
        "<<ON/OFF>>",
        "@D   Eliment Disk, Dummy Buttons, Queue Display, WaveForm,",
        "@D   Lyrics Engine, Visualizer, Stereo Sound, Normalize Volume,",
        "@D   Show meta data only, Replace Emoji",
        "@D   Use Lyrics / Oscilloscope / Spectrogram / Sphere",
        "@A                   false leaves that view out of the [.]",
        "@A                   cycle entirely",
    }});
    t.pages.push_back({{
        "@H Tab 3: ANIMA/VIZ",
        "",
        "Scrolls with the cursor when the terminal is short. Every row",
        "cycles with [<-] / [->]:",
        "",
        "<<LYRICS>>",
        "@D   Lyrics Alignment   left / center / right",
        "@D   Lyrics Animation   full / word by word / line by line /",
        "@D                      letter by letter / active line only /",
        "@D                      active word only",
        "<<DISK>>",
        "@D   Disk Speed         0.01 .. 1.00",
        "<<WAVE>>  (the spectrum bars under the meta data)",
        "@D   Wave Fluidity / Degradation / Viscosity    1-10",
        "@D   Waveform Style     raw / smooth          (same as [w])",
        "<<MODES>>",
        "@D   Lyrics/Viz         sphere / osci / spectro (the view while",
        "@D                      the lyrics are off)",
        "@D   No-Lyrics Viz      sphere / osci / spectro: shown while the",
        "@D                      lyrics view has no lyrics; its Use switch",
        "@D                      is turned on when you pick it",
        "<<VIZ/LYRICS ORDER>>",
        "@D   Viz Order 1..4     the order of the views in the [.] cycle",
        "",
        "@A   A note at the bottom of the tab: the oscilloscope and the",
        "@A   spectrogram are tuned with [SHIFT+o] and [SHIFT+i], [SHIFT+u]",
        "@A   is the big spectrogram overlay, [)] and [(] open the extra",
        "@A   windows of the oscilloscope and the spectrogram.",
    }});
    t.pages.push_back({{
        "@H Tab 4: PATHS",
        "",
        "<<PATHS>> -- five sections. [ENTER] edits a row; the tab scrolls",
        "with the cursor, so a short terminal is fine.",
        "",
        "@D   LOCAL PATH             the folders scanned for music",
        "@D                          (several, plus \"+ new path\")",
        "@D   DOWNLOAD PATH          where [y] saves streams",
        "@D   PLAYLIST PATH          one folder for all playlists",
        "@D   PLAYLIST EXPORT PATH   where [e] writes M3U8 / M3U",
        "@D   HISTORY PATH           holds history.json",
        "",
        "@A   Changing LOCAL PATH <<rescans at once>>. Changing the playlist",
        "@A   or history folder <<copies>> what is there -- nothing is moved",
        "@A   or deleted. Changed folders only stick when you save with [s].",
    }});
    t.pages.push_back({{
        "@H Tab 5: REFERENCE      Tab 6: ABOUT APP",
        "",
        "<<REFERENCE>> is the longest tab and has two parts.",
        "",
        "@D   Hotkeys        every rebindable command, grouped like the",
        "@D                  cheat sheet: PLAYBACK, NAVIGATION & VIEW,",
        "@D                  LYRICS & KARAOKE, VISUALIZATIONS, SEARCH,",
        "@D                  QUEUE, PLAYLISTS, META EDITOR, HISTORY,",
        "@D                  EQUALIZER, DOWNLOADS, LIBRARY, SYSTEM",
        "@D   FONT MAP       the A = A, a table from config.txt",
        "@D                  (read-only here)",
        "",
        "@K   [ENTER] on a hotkey row, type the new key, [ENTER] again",
        "",
        "A key is written as the character itself (n, T, #, +; a capital",
        "is SHIFT + the letter) or as a name: ENTER, TAB, SPACE,",
        "BACKSPACE, ARROW_KEY_UP / DOWN / LEFT / RIGHT. A combination",
        "([ALT+L], [CTRL+L], [CTRL+SHIFT+u], [CTRL+SHIFT+z]) is entered by",
        "pressing it. An empty field unbinds the command.",
        "",
        "@A   A key already used by another command is <<refused>>, with a",
        "@A   message naming it -- so you can never lose a command by",
        "@A   accident. [*] and [ESC] are reserved. Keys not listed here",
        "@A   (the editors' and overlays' own keys, [ENTER], the arrows,",
        "@A   [CTRL+s] ...) are fixed; the cheat sheet [?] shows those.",
        "",
        "<<ABOUT APP>> -- the credits and versions, scrolled with [UP] /",
        "[DOWN]. The text lives in config.txt and can be edited there",
        "while the app is closed; the version line is brought up to the",
        "running build automatically.",
    }});
    return t;
}

Topic topic_history() {
    Topic t;
    t.title = "History";
    t.blurb = "what you played, and 16 smart lists";
    t.pages.push_back({{
        "@H The listening history",
        "",
        "@K   [SHIFT+h]  open it -- [ESC], [q] or [SHIFT+h] close",
        "@K   [1] - [4] or [<-] / [->]  jump to a tab",
        "",
        "@D   [1 HISTORY]       the last 100 plays: when, what, how long,",
        "@D                     and PLAY / done / skip",
        "@D   [2 TOP TRACKS]    one row per title with its play count",
        "@D   [3 TOP PLAYLISTS] the top 5 saved playlists of the week,",
        "@D                     month, quarter, year and overall",
        "@D   [4 HABITS]        sessions, time per day, tracks per",
        "@D                     session, skips, replays, completion",
        "",
        "@K   [UP] / [DOWN]  move (on TOP PLAYLISTS and HABITS they scroll)",
        "@K   [ENTER]        on HISTORY or TOP TRACKS: <<play it now>>",
        "@K                  (the overlay stays open)",
        "@K   [r]            on TOP TRACKS: most played <-> least played",
        "",
        "Nothing you do in here changes your music.",
        "",
        "@A   A play counts for a playlist when it was queued from a saved",
        "@A   playlist (Enter or [a] on a playlist row). Plays from before",
        "@A   TOP PLAYLISTS existed have no playlist and are not counted.",
        "",
        "@A   The history keeps the newest 1000 plays one by one and folds",
        "@A   older ones into lifetime totals per title -- including the",
        "@A   day and the time of day of every play, so the period and",
        "@A   time-of-day lists on the next page stay exact over years.",
        "",
        "It lives in history.json, in <<Settings -> PATHS -> HISTORY PATH>>.",
    }});
    t.pages.push_back({{
        "@H ADD SMART HISTORY TO QUEUE",
        "",
        "Under the TOP TRACKS list sits a pane with <<sixteen ready-made>>",
        "<<lists>> in four columns:",
        "",
        "@D   TOP TRACKS   TOP OF THE...   TIME OF DAY   REDISCOVER",
        "@D   ──────────   ─────────────   ───────────   ──────────",
        "@D   Top 10       the week        Morning       newly added",
        "@D   Top 25       the month       Day           least played",
        "@D   Top 50       the quarter     Evening       least: month",
        "@D   Top 100      the year        Night         least: year",
        "",
        "@K   [2] (or [->])  go to TOP TRACKS",
        "@K   [TAB]          move INTO the pane (a mark appears behind",
        "@K                  its title)",
        "@K   arrows         pick a list -- while the pane has the focus,",
        "@K                  [<-] / [->] move between COLUMNS and do not",
        "@K                  switch tabs",
        "@K   [ENTER]        add that list to the end of the queue",
        "@K   [ESC] or [TAB] back to the track list",
        "",
        "@A   <<Morning>> 7-11, <<Day>> 11-18, <<Evening>> 18-22, <<Night>> 22-7.",
        "@A   <<Least played>> counts never-played tracks as 0 and shuffles",
        "@A   ties every time, which makes it a good way to rediscover",
        "@A   forgotten music.",
        "",
        "Files that moved or were deleted are skipped, and the status",
        "line says how many tracks were queued.",
    }});
    return t;
}

Topic topic_formats() {
    Topic t;
    t.title = "What Mousiki plays";
    t.blurb = "audio, tracker modules and chiptunes";
    t.pages.push_back({{
        "@H The usual formats, and some unusual ones",
        "",
        "MP3, FLAC, WAV, OGG, Opus, M4A and the other common formats",
        "play out of the box. So do retro formats, which show up in the",
        "list, the search and the queue like any other file:",
        "",
        "@D   Tracker modules (Amiga / PC demoscene)",
        "@D     .mod .xm .it .s3m .mptm .stm .669 .mtm .med .okt .far",
        "@D     .ult .ams .dbm .digi .dmf .dsm .gdm .imf .j2b .mdl .mt2",
        "@D     .psm .ptm .umx .plm",
        "@A     needs FFmpeg built with libopenmpt (setup.sh / setup.ps1",
        "@A     install or upgrade it for you)",
        "",
        "@D   Game console music",
        "@D     .nsf .nsfe (NES)   .spc (SNES)   .gbs (Game Boy)",
        "@D     .vgm .vgz (Sega)   .ay .hes .kss .sap .gym",
        "@A     needs FFmpeg built with libgme (same: setup installs it)",
        "",
        "@D   Commodore 64 SID tunes",
        "@D     .sid .psid .rsid",
        "@A     needs the sidplayfp program on the PATH (setup installs",
        "@A     it and adds it to the PATH where it can)",
        "",
        "The TYPE column says <<tracker module>>, <<game music>> or",
        "<<C64 SID (n tunes)>>, and the module's own title is used as its",
        "metadata. If a required tool is missing, the status line says",
        "which one and the track is skipped.",
        "",
        "@A   A SID tune has no end, so it is rendered once for",
        "@A   SidPlayLength seconds (default 180) and played from there.",
    }});
    return t;
}

// --- radio topics ----------------------------------------------------------

Topic topic_radio_main() {
    Topic t;
    t.title = "Radio Main UI";
    t.blurb = "the dial, the panes and the big overlay";
    t.pages.push_back({{
        "@H The radio screen",
        "",
        "@D ┌──────────────────────────────────────────────────────────────┐",
        "@D │  ░▒▓ ON AIR ▓▒░   STATION  TITLE  ARTIST      oscilloscope,  │",
        "@D │                   genre  bitrate  country     sphere or      │",
        "@D │                   ▁▃▅█▅▃▁ spectrum           spectrogram     │",
        "@D ├──────────────────────────────────────────────────────────────┤",
        "@D │  FREQUENCY BAND (MHz)                                        │",
        "@D │  88 ·  92 ·  96 ▼ 100 ·  104 ·  108                          │",
        "@D │  ▮▮▮▮▯ STEREO   LIVE 12:34                                   │",
        "@D ├──────────────────────────────────────────────────────────────┤",
        "@D │  SEARCH STATIONS                                   [ L ]     │",
        "@D ├───────────────────────────────┬──────────────────────────────┤",
        "@D │  STATIONS                     │  PRESETS (Morning)           │",
        "@D │  > a station                  │  1 one   2 two   3 ...       │",
        "@D └───────────────────────────────┴──────────────────────────────┘",
        "@A        └─ your station list          └─ 16 slots on the keys",
        "@A                                         1..0 and e r t d f g",
        "@A    ▼ = the needle on the tuned station; the dots are the",
        "@A        other stations of your list",
        "",
        "@K   [UP] / [DOWN] move    [ENTER] tune the hovered station",
        "@K   [n] / [b] next / previous channel    [#] a random one",
        "@K   [p] mute/unmute   [x] stop   [R] reconnect   [+] / [-] volume",
        "@K   [m] surf mode S shuffle <-> L list (it only affects [b])",
    }});
    t.pages.push_back({{
        "@H What the dial's footer tells you",
        "",
        "@D   ▮▮▮▮▯ STEREO   LIVE 12:34",
        "@A    │     │        └─ how long you have been tuned in",
        "@A    │     └─ STEREO or MONO",
        "@A    └─ signal strength",
        "",
        "@D   ‖ PAUSED -0:45     you pressed SPACE and are behind live",
        "@D   ◑ TIMESHIFT -2:30  you are listening 2:30 behind live",
        "@D   REC 01:12          a recording is running",
        "",
        "@H The big STATIONS overlay",
        "",
        "@K   [SHIFT+l]  the STATIONS pane with the whole screen, plus a",
        "@K              PRESET NAME column   --   [ESC] closes",
        "",
        "@K   [a]        add a station by its <<stream URL>>",
        "@K              [TAB] switches URL <-> NAME; an empty name",
        "@K              becomes the host name. The station is appended",
        "@K              at the end, so preset slots stay valid.",
        "@K   [SHIFT+c]  give the hovered station a <<preset name>>: a",
        "@K              shorter second name shown in the PRESETS pane",
        "",
        "It shares the search box and the state of the main screen, and",
        "[ENTER], [n], [b], [#], [p], [x], [+] and [-] work as usual.",
        "",
        "@A   Letters are <<case sensitive>> in the radio: [r] recalls a",
        "@A   preset slot, [R] reconnects the station.",
    }});
    return t;
}

Topic topic_radio_presets() {
    Topic t;
    t.title = "Presets Overlay";
    t.blurb = "16 slots per preset, as many presets as you like";
    t.pages.push_back({{
        "@H Presets: 16 stations on 16 keys",
        "",
        "A preset is a named set of <<16 station slots>> on these keys:",
        "",
        "@D   1  2  3  4  5  6  7  8  9  0  e  r  t  d  f  g",
        "@A   └─ press one on the main screen to recall that slot",
        "",
        "@K   [SHIFT+<-] / [SHIFT+->]  previous / next preset",
        "",
        "The active preset's name is in the pane title: PRESETS (Morning).",
        "",
        "@H The PRESETS menu",
        "",
        "@K   [SHIFT+k]  open it",
        "",
        "@D   ┌──────────────────────────────────────────────────────────┐",
        "@D   │  SEARCH STATION / SEARCH PRESET                          │",
        "@D   ├──────────────────────────────────────────────────────────┤",
        "@D   │  SELECT PRESET    Default   Morning   Jazz   Night       │",
        "@D   ├───────────────────────────┬──────────────────────────────┤",
        "@D   │  STATIONS                 │  PRESETS (the 16 slots)      │",
        "@D   └───────────────────────────┴──────────────────────────────┘",
        "@A        └─ a small mark behind a pane's title shows which one",
        "@A           has the keyboard",
        "",
        "Names are 1 to 24 characters and unique. Every change is",
        "written to presets.txt at once.",
    }});
    t.pages.push_back({{
        "@H Keys in the PRESETS menu",
        "",
        "@K   [TAB]      SEARCH > SELECT PRESET > STATIONS",
        "@K   [ENTER]    STATIONS: tune it.  SELECT PRESET: open that",
        "@K              preset and tune its first filled slot",
        "@K   [1]..[0], [e r t d f g]   in STATIONS: put the hovered",
        "@K              station in that slot (the same key again clears",
        "@K              it; a station sits in one slot only, so it moves)",
        "@K   [DEL]      remove the hovered station from the preset",
        "@K   [SHIFT+n]  new preset     [SHIFT+c]  rename the hovered one",
        "@K   [SHIFT+d]  delete it -- press twice; the last one stays",
        "@K   [/]        back to the search box    [ESC]  close",
        "",
        "@A   A station sits in <<one slot only>>: putting it in another slot",
        "@A   moves it rather than duplicating it.",
    }});
    return t;
}

Topic topic_radio_search() {
    Topic t;
    t.title = "Search Radio Stations";
    t.blurb = "your list, saved lists, and Radio Browser";
    t.pages.push_back({{
        "@H Searching your own stations",
        "",
        "@K   [/]  open the search box under the frequency band",
        "",
        "The STATIONS pane filters <<live>> as you type; the station under",
        "the cursor stays highlighted, [ENTER] tunes it and [ESC] clears",
        "the search.",
        "",
        "@D   (a name, genre or country)   filters your stations",
        "@D   s:                           back to searching stations",
        "@D   p:  query                    your saved STATION LISTS",
        "@A                                [ENTER] fills the pane with",
        "@A                                that list and tunes its first",
        "@A                                station",
        "",
        "While a list is shown the pane title reads",
        "<<STATIONS (LIST: name - ESC: all)>> and its rows are numbered by",
        "position. [n], [b] and [#] then surf <<only inside that list>>.",
        "[ESC] clears a search still in the box; the next [ESC] returns",
        "to all stations.",
        "",
        "@A   The search is fuzzy and typo tolerant, like the player's.",
        "@A   The characters - _ . / count as spaces. With a query the",
        "@A   best match comes first; [SHIFT+t] (sort A-Z) overrides that.",
    }});
    t.pages.push_back({{
        "@H Finding new stations: RADIO BROWSER",
        "",
        "@K   [SHIFT+s]  search the public directory radio-browser.info",
        "",
        "@D   ┌──────────────────────────────────────────────────────────┐",
        "@D   │ NAME        TAGS        COUNTRY                          │",
        "@D   │ STATE       LANGUAGE    BITRATE                          │",
        "@D   ├──────────────────────────────────────────────────────────┤",
        "@D   │ RESULTS                                                  │",
        "@D   │ · a station that is already in your list                 │",
        "@D   │   another station                                        │",
        "@D   ├──────────────────────────────────────────────────────────┤",
        "@D   │ STATION INFO  (every detail of the hovered result)       │",
        "@D   └──────────────────────────────────────────────────────────┘",
        "@A     └─ a dot in front means you already have that station",
        "",
        "@D   TAGS      comma separated, EVERY tag has to match",
        "@D   COUNTRY   a name (Germany) or a two-letter code (DE)",
        "@D   BITRATE   128 = at least 128, 64-192 = range, -192 = at most",
        "",
        "@K   [TAB]    NAME > TAGS > COUNTRY > STATE > LANGUAGE >",
        "@K            BITRATE > RESULTS > NAME",
        "@K   [ENTER]  in a pane: search. In RESULTS: tune it.",
        "@K   [a]      in RESULTS: <<add it to your station list>>",
        "@K   [ESC]    clear a pane; on an empty pane or RESULTS: close",
        "",
        "Every search hides broken streams, sorts by clicks and returns",
        "at most 200 results. It needs <<curl>>.",
    }});
    return t;
}

Topic topic_radio_lists() {
    Topic t;
    t.title = "Station Lists";
    t.blurb = "named collections, built like playlists";
    t.pages.push_back({{
        "@H The STATION LISTS menu",
        "",
        "Named, ordered collections of stations -- a \"jazz\" list, a",
        "\"morning\" list -- built exactly like the player's playlist",
        "editor.",
        "",
        "@K   [SHIFT+p]  open it. Two tabs, [<-] / [->] switch them",
        "@K              ([ALT+<-] / [ALT+->] from every pane, also while",
        "@K              typing).",
        "",
        "@D ┌──────────────────────────────────────────────────────────────┐",
        "@D │ Name: morning               [CREATE/EDIT]  SAVED LISTS       │",
        "@D ├──────────────────────────────────────────────────────────────┤",
        "@D │ SEARCH ALL STATIONS                                          │",
        "@D ├───────────────────────────────┬──────────────────────────────┤",
        "@D │ STATIONS                      │ LIST CONTENTS                │",
        "@D │ > a station ·                 │ 1. first station             │",
        "@D │   another one                 │ 2. second station            │",
        "@D └───────────────────────────────┴──────────────────────────────┘",
        "@A        └─ a dot means it is already       └─ what you are",
        "@A           in the list being built            building",
        "",
        "Lists are stored in stationlists.txt. A station that has been",
        "removed from your station list is dropped when a list is read.",
    }});
    t.pages.push_back({{
        "@H Keys in the STATION LISTS menu",
        "",
        "@K   [TAB]      name > search > STATIONS > LIST CONTENTS",
        "@K   [ENTER]    in STATIONS: add it to the end of the list.",
        "@K              In LIST CONTENTS: tune it, to audition the list",
        "@K   [4] / [5]  move the hovered station up / down",
        "@K   [DEL] or [d]  remove it",
        "@K   [SHIFT+t]  sort STATIONS: list order <-> name A-Z",
        "@K   [CTRL+s]   save under the typed name and stay in the menu",
        "@K   [ESC]      leave a text box; again: close (it asks about",
        "@K              unsaved changes)",
        "",
        "On tab 2, [ENTER] loads a list back into tab 1 and [DEL]",
        "deletes it after a Yes/No question.",
        "",
        "@A   Open a list on the main screen with <<p:>> in the search box.",
        "@A   [n], [b] and [#] then surf only inside that list.",
    }});
    return t;
}

Topic topic_radio_timeshift() {
    Topic t;
    t.title = "Timeshift and Recording";
    t.blurb = "pause live radio, and record from the past";
    t.pages.push_back({{
        "@H Timeshift: live radio you can pause",
        "",
        "From the moment you tune a station, the radio keeps the last",
        "minutes of it in a rolling buffer <<on disk>>, so live radio can be",
        "treated like a recording.",
        "",
        "@D   tuned in                       now (live)",
        "@D      |------------------------------|",
        "@D      |   the buffer: 5 .. 60 min    |",
        "@D                  ^",
        "@A                  you can listen anywhere in here",
        "",
        "@K   [SPACE]  pause -- the station keeps arriving in the buffer;",
        "@K            [SPACE] again resumes where you stopped",
        "@K   [[] / []]  back / forward 30 seconds",
        "@K            (German keyboard: ALT GR+8 / ALT GR+9)",
        "@K   [{]      back 5 minutes      [}]  back to live",
        "@K            (German: ALT GR+7 / ALT GR+0)",
        "",
        "Every jump is shown for 4 seconds under the scope block, e.g.",
        "<<-0:30 | 2:30 behind live>>.",
        "",
        "@A   You can never go back before the moment you tuned in --",
        "@A   internet radio only delivers from \"now\". Catching up with",
        "@A   []] hands back to the live stream without a gap. Tuning",
        "@A   another station, [x] or quitting clears the buffer.",
    }});
    t.pages.push_back({{
        "@H The timeshift buffer itself",
        "",
        "The buffer size is <<Settings -> ON/OFF -> Timeshift buffer>>",
        "(5, 15, 30, 45 or 60 minutes; default 30).",
        "",
        "@D   radio_timeshift.pcm   in ~/.cache/mousiki/",
        "@D   about 11.5 MB per minute  ──>  ~345 MB for 30 minutes",
        "",
        "@A   It is written by the stream thread and never touched by the",
        "@A   screen loop, so the interface stays exactly as fast as it is",
        "@A   without it. The file is deleted when the radio closes.",
        "",
        "If a pause lasts longer than the buffer, the oldest part is",
        "overwritten and playback continues from the oldest audio that",
        "is still there.",
    }});
    t.pages.push_back({{
        "@H Recording -- including the minutes before you pressed",
        "",
        "@K   [y]  open the RECORD overlay. It freezes the moment you",
        "@K        pressed, so no time is lost while you choose.",
        "",
        "@D   ┌──────────────────────────────────────────────┐",
        "@D   │  From now on (when this opened)              │",
        "@D   │  The last 1 minute                           │",
        "@D   │  The last 5 minutes                          │",
        "@D   │  The last 10 minutes                         │",
        "@D   │  The last 15 minutes                         │",
        "@D   │  The last 30 minutes                         │",
        "@D   │  Everything buffered                         │",
        "@D   └──────────────────────────────────────────────┘",
        "@A        └─ a row says \"only m:ss\" when the buffer does not",
        "@A           reach that far back, and starts at the oldest audio",
        "",
        "@K   [UP] / [DOWN] choose   [ENTER] or [y] start   [ESC] cancel",
        "@K   [y] again while it runs  stop",
        "",
        "While it runs the dial's footer shows <<REC mm:ss>>.",
        "",
        "@A   It is written from the buffer as a WAV, which ffmpeg then",
        "@A   converts in the background to <<YYYY-MM-DD_HH-MM-SS.mp3>>,",
        "@A   named by the time the first recorded second was <<on air>>,",
        "@A   and taken before volume, mute and the mono fold.",
        "",
        "Recordings go to the radio's download folder (Settings ->",
        "PATHS), which by default is the player's one.",
    }});
    return t;
}

Topic topic_radio_history() {
    Topic t;
    t.title = "History";
    t.blurb = "channels, habits, and finding a song on YouTube";
    t.pages.push_back({{
        "@H The radio's own history",
        "",
        "@K   [h]  open it -- [ESC], [q] or [h] close",
        "@K   [1] [2] [3], [<-] / [->] or [TAB]  switch the tab",
        "@K   [UP] / [DOWN] move    [HOME] / [END] jump to the ends",
        "",
        "@D   [1 HISTORY]       WHEN / CHANNEL / ARTIST / TITLE / HEARD",
        "@D   [2 TOP CHANNELS]  channels by time listened ([r] flips it)",
        "@D   [3 HABITS]        sessions, time per day, channels, and",
        "@D                     listening by hour and by weekday",
        "",
        "A new HISTORY line starts whenever the channel, the artist or",
        "the title changes. Lines heard for less than 3 seconds are",
        "dropped. A session ends after a gap of 30 minutes.",
        "",
        "@A   The newest 10 000 lines are kept one by one; older ones are",
        "@A   folded into lifetime totals per channel, day, hour and",
        "@A   weekday, so TOP CHANNELS and HABITS stay lifetime figures.",
    }});
    t.pages.push_back({{
        "@H Heard a song on the radio? Download it.",
        "",
        "The radio writes down the <<artist and title>> the stream sends.",
        "On the HISTORY tab that becomes a search:",
        "",
        "@K   [y]  on the hovered line -- a small overlay opens and",
        "@K        searches YouTube for \"artist title\"",
        "",
        "@D   ┌──────────────────────────────────────────────┐",
        "@D   │  FIND ON YOUTUBE: artist title               │",
        "@D   ├──────────────────────────────────────────────┤",
        "@D   │  > a result                           [ok]   │",
        "@D   │    another result                     [..]   │",
        "@D   │    a third one                        [!!]   │",
        "@D   └──────────────────────────────────────────────┘",
        "@A        └─ [ok] done   [..] downloading   [!!] failed",
        "",
        "@K   [ENTER]  download the hovered result into the download",
        "@K            folder (yt-dlp, opus)",
        "@K   [TAB]    edit the search query",
        "@K   [ESC]    close the overlay",
        "",
        "@A   This is the player's own online search, reached from the",
        "@A   radio -- so a song you only heard once on air ends up in",
        "@A   your local library, where the player will find it after a",
        "@A   rescan ([SHIFT+r] in the player).",
    }});
    return t;
}

Topic topic_radio_settings() {
    Topic t;
    t.title = "Radio Settings";
    t.blurb = "its own file, and what only the radio has";
    t.pages.push_back({{
        "@H Settings, the radio's own",
        "",
        "@K   [s]  open them. Same frame, tab strip and editing keys as",
        "@K        the player's: [TAB] next tab, [UP]/[DOWN] rows,",
        "@K        [<-]/[->] or [ENTER] change a value,",
        "@K        <<[s] saves and closes, [ESC] / [q] discard>>.",
        "",
        "@A   They are stored in <<radio_config.txt>>, separate from the",
        "@A   player's config.txt. Colours are <<not>> carried over between",
        "@A   the two modes -- the radio has its own palette.",
        "",
        "@H COLORS: the rows only the radio has",
        "",
        "@D   ON_AIR         UPPER_LEFT / BOTTOM_RIGHT",
        "@A                  the ON AIR sign's diagonal gradient, and",
        "@A                  with it the needle, the lamp and LIVE",
        "@D   OSCI           LEFT / RIGHT     the oscilloscope",
        "@D   FREQUENCY_BAR  LINE / MHz       the dial's line and ticks /",
        "@A                                   the numbers and the dots",
        "@D   VOLUME         CURRENT / POSSIBLE   the volume bar",
        "@D   PRESETS        INACTIVE FG/BG, KEY FG",
        "@A                  empty slots / the shortcut letters",
        "",
        "BORDER_COLOR, METADATA, VIZ, LIST, HEADER, LEGEND and TAB_NAMES",
        "work exactly as in the player.",
    }});
    t.pages.push_back({{
        "@H ON/OFF and ANIMA/VIZ",
        "",
        "<<ON/OFF>> -- two categories, as in the player:",
        "",
        "<<MODES>>",
        "@D   Playback          shuffle / list  (same as [m])",
        "@D   Start Mode        player / radio: which of the two Mousiki",
        "@D                     opens with (the player's StartMode=)",
        "<<ON/OFF>>",
        "@D   On air ascii      the ON AIR sign and its colour wave",
        "@D   Pulse wave        the faint circle burst behind the sign",
        "@D   Dummy buttons     the MUTE boxes next to the dial",
        "@D   Visualizer        the spectrum under the station info",
        "@D   Stereo sound      off folds left and right into mono",
        "@D   Normalize volume  loudness normalization, as [v]",
        "@D   Tuning noise      static with crackle while tuning",
        "@A                     (off by default; it is never recorded)",
        "@D   Timeshift buffer  5 / 15 / 30 / 45 / 60 minutes",
        "@D   Use oscilloscope / Use spectrogram",
    }});
    t.pages.push_back({{
        "@H ANIMA/VIZ and PATHS",
        "",
        "<<ANIMA/VIZ>> -- <<ON AIR SIGN>> (Gradient wave speed: the colour",
        "wave; Pulse wave speed), <<WAVE>> (Wave Fluidity, Degradation,",
        "Viscosity, as in the player) and <<MODES>> (Osci/sphere:",
        "osci / sphere / spectro / off, and Osci style: braille / image).",
        "A note at the bottom names the keys: [SHIFT+o] / [SHIFT+i] tune",
        "the oscilloscope / spectrogram, [SHIFT+u] the big spectrogram,",
        "[)] and [(] the extra windows.",
        "",
        "<<PATHS>> -- every path is a <<base>> folder; the radio makes its own",
        "sub-folders below it:",
        "",
        "@D   STATION LISTS PATH  <path>/stations/, <path>/station_lists/",
        "@D   PRESETS PATH        <path>/presets/presets.txt",
        "@D   DOWNLOAD PATH       <path>/radio_downloads/",
        "@D   Same folder as music player   on = use the player's one",
        "@D   HISTORY PATH        <path>/radio_history/",
        "",
        "@A   Changed folders are <<copied, never moved>>, and nothing is",
        "@A   deleted; playback stops and everything reloads.",
    }});
    t.pages.push_back({{
        "@H REFERENCE: rebinding the radio's keys",
        "",
        "The keys that are not hard coded, grouped like the player's:",
        "PLAYBACK, MENUS, TIMESHIFT and SYSTEM.",
        "",
        "@K   [ENTER]  on a row, then type ONE character (<<case matters>>)",
        "@K            or SPACE / TAB / BACKSPACE",
        "@K   [DEL]    restore that key's default",
        "@K   [CTRL+SHIFT+u]  undo the last five key changes",
        "",
        "The first row, <<Reset all keys to default>>, sits under the",
        "cheat-sheet note at the top.",
        "",
        "@A   A key already in use is <<refused>> -- and the fixed keys count",
        "@A   too: the 16 preset slot keys (1..0, e r t d f g) and the",
        "@A   vi-style [j] / [k]. The cheat sheet [?] always lists the",
        "@A   DEFAULT keys, so it stays a reliable reference.",
        "",
        "Keys are stored as HKey<Action>= lines in radio_config.txt.",
    }});
    t.pages.push_back({{
        "@H The radio's files",
        "",
        "@D   radio_config.txt   settings, colours, EQ, oscilloscope",
        "@D   stations.txt       your station list",
        "@D   presets.txt        the presets and the active one",
        "@D   stationlists.txt   the saved station lists",
        "@D   preset_names.txt   the short names for the PRESETS pane",
        "@D   radio_history/     history_radio.txt, archive_radio.txt",
        "",
        "They live in the config folder (~/.config/mousiki/, on Windows",
        "%USERPROFILE%\\.config\\mousiki\\).",
    }});
    return t;
}

// Built once on first use and handed out by reference afterwards: the pages
// are static text, so there is no reason to rebuild them on every keypress.
const std::vector<Topic>& player_topics() {
    static const std::vector<Topic> v = [] {
        std::vector<Topic> t;
        t.push_back(topic_main_ui());
        t.push_back(topic_search());
        t.push_back(topic_meta());
        t.push_back(topic_playlist());
        t.push_back(topic_visuals());
        t.push_back(topic_lyrics());
        t.push_back(topic_karaoke());
        t.push_back(topic_sound());
        t.push_back(topic_sleep());
        t.push_back(topic_history());
        t.push_back(topic_settings());
        t.push_back(topic_formats());
        return t;
    }();
    return v;
}

const std::vector<Topic>& radio_topics() {
    static const std::vector<Topic> v = [] {
        std::vector<Topic> t;
        t.push_back(topic_radio_main());
        t.push_back(topic_radio_presets());
        t.push_back(topic_radio_search());
        t.push_back(topic_radio_lists());
        t.push_back(topic_radio_timeshift());
        t.push_back(topic_radio_history());
        t.push_back(topic_visuals());
        t.push_back(topic_sound());
        t.push_back(topic_sleep());
        t.push_back(topic_radio_settings());
        return t;
    }();
    return v;
}

} // namespace

std::vector<std::string> welcome_lines(const std::string& version) {
    return make_welcome(version);
}

const std::vector<Topic>& topics(Flavor f) {
    return f == Flavor::Player ? player_topics() : radio_topics();
}

int page_count(Flavor f) {
    int n = 0;
    for (const auto& t : topics(f)) n += static_cast<int>(t.pages.size());
    return n;
}

} // namespace wizard
