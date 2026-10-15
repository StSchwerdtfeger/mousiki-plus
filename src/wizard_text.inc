// Generated from Mousiki_Help_Wizard.txt by tools/wizard_to_inc.py -- edit the .txt, not this file.
static const char* const kWizardTextChunks[] = {
R"WIZ(
[[New Page]]
WELCOME TO THE MOUSIKI PLUS HELP WIZARD !!! [[Header]]

Mousiki Plus is a terminal UI music and radio player built for lovers
of the terminal. <3

OPEN/CLOSE HELP WIZARD: [[Header]]
To open/close use [SHIFT+w] and [ESC] for just closing it.

RECOMMENDATIONS FOR FIRST TIME USERS: [[Header]]
The first thing you might want to do is setting your custom folder paths
first! In the main music player press [s] to go to SETTINGS[[Header]],
press [TAB] to move to the [[PATHS]] tab and change the local paths to
add your music folders. Subdirectories of paths will be recognized
automatically. More details on paths can be found in the Help Wizard
page on MUSIC FILE PATHS[[Header]].

HELP WIZARD TABLE OF CONTENT: [[Header]	]
Use [->] to move on to the table of content or press [t] from anywhere
you currently are in the Wizard. Apart from the Help Wizard, you can
also use [?] to open the CHEAT SHEET[[Header]] for a plain overview of commands 
without detailed explanations. As a third option you can also use the 
Mousiki Plus user manual to find your way around (includes images and 
is even more detailed than the Help Wizard or cheat sheet).

ABOUT: [[Header]]
Mousiki Plus (Win/Linux/macOS):
StSchwerdtfeger — github.com/StSchwerdtfeger
Version {VERSION} — Apache 2.0 license

Original Mousiki (Linux/macOS/Android):
ender — github.com/itzender5820
Version 1.0 — Apache 2.0 license


[[New Page]]
WELCOME TO THE MOUSIKI PLUS HELP WIZARD !!! [[Header]]

Mousiki Plus is a terminal UI music and radio player built for lovers
of the terminal. <3

OPEN/CLOSE HELP WIZARD: [[Header]]
To open/close use [SHIFT+w] and [ESC] for just closing it.

HELP WIZARD TABLE OF CONTENT: [[Legend]]
Use [->] to move on to the table of content or press [t] from anywhere
you currently are in the Wizard. Apart from the Help Wizard, you can
also use [?] to open the CHEAT SHEET[[Header]] for a plain overview of commands 
without detailed explanations. As a third option you can alsouse the 
Mousiki Plus user manual to find your way around (includes images and 
is even more detailed than the Help Wizard)

ABOUT: [[Header]]
Mousiki Plus (Win/Linux/macOS):
StSchwerdtfeger — github.com/StSchwerdtfeger
Version {VERSION} — Apache 2.0 license

Original Mousiki (Linux/macOS/Android):
ender — github.com/itzender5820
Version 1.0 — Apache 2.0 license


[[New Page]]
TABLE OF CONTENT: [[Header]]

I Overview of the Capabilities of Mousiki Plus [[Header]]

II MUSIC PLAYER MODE[[Header]]

  1 Main UI [[Header]]
   1.1 The Main Music Player UI and its Screen Panes
   1.2 Music File Paths
    1.2.1 Supported Formats

  2 Search, Filter and Download[[Header]]
   2.1 Search and Filtering Capabilities
   2.2 Download (Music Player)

  3 Curating Your Files[[Header]]
   3.1 Playback Modes
   3.2 Building a Queue
   3.3 Meta Data Editor (Edit / Fetch)
    3.3.1 Edit Tab
    3.3.2 Fetch Meta Data (AcoustID/MusicBrainz)
   3.4 Playlist Editor/Creator (incl. Import/Export)
   3.5 File Conversion

  4 Lyrics and Karaoke[[Header]]
   4.1 Synced Lyrics
   4.2 Lyrics Retry, Timing and Reading
   4.3 Karaoke Mode

  5 Visualizations[[Header]]
   5.1 The Visualization Field
   5.2 Oscilloscope
   5.3 Spectrogram and Extra Windows

  6 Sound and Sleep Timer[[Header]]
   6.1 Equalizer
   6.2 Loudness Normalization and Stereo/Mono
   6.3 Sample Rate and Playback Engine
   6.4 Sleep Timer

  7 History[[Header]]
   7.1 Listening History
   7.2 Smart History Lists

  8 Settings and Color Themes[[Header]]
   8.1 Settings Basics
   8.2 Colors and Color Themes
   8.3 ON/OFF, ANIMA/VIZ and PATHS
   8.4 Hotkeys, Reference and About

III RADIO PLAYER MODE[[Header]]

  9 The Radio Main UI[[Header]]
   9.1 The Radio Screen and its Keys
   9.2 Presets

  10 Finding Stations[[Header]]
   10.1 Searching Stations and the Radio Browser
   10.2 Station Lists

  11 Timeshift, Recording and History[[Header]]
   11.1 Timeshift
   11.2 Recording
   11.3 Radio History

  12 Radio Settings[[Header]]
   12.1 Radio Settings: Colors, ON/OFF and ANIMA/VIZ
   12.2 Radio Settings: Paths, Keys and Files

[[New Page]]

OVERVIEW OF THE CAPABILITIES OF MOUSIKI PLUS: [[Header]]
Apart from playing and queuing local and online files, the Mousiki Plus
music player offers the possibility to create playlists, download online
files (YouTube, SoundCloud, Bandcamp), adjust and fetch meta data from
online sources (AcoustID/MetaBrainz), a synced lyric function (including
timing adjustment, lyrics translation and a Karaoke mode!), includes
various visualizations of your music (e.g. spectrogram, XY/YT-mode
oscilloscope), an equalizer / sound normalization, sleep timer, file
format conversion, a smart listening history, (custom) color themes
and then some...

The radio player mode includes a lot of the features of the regular
music player and lets you listen to a vast variety of online radio
channels that can be added via an online search mode or manually and
organized by creating stations lists. It includes a recording function
and a listening history that contains meta data, so you can search and
download songs you listened to on the radio. Channels can be assigned
preset keys to tune in to channels fast...

Mousiki Plus (Win/Linux/macOS):
StSchwerdtfeger — github.com/StSchwerdtfeger
Version {VERSION} — Apache 2.0 license

Original Mousiki (Linux/macOS/Android):
ender — github.com/itzender5820
Version 1.0 — Apache 2.0 license


[[New Page]]
1.1 THE MAIN MUSIC PLAYER UI AND ITS SCREEN PANES [[Header]]

MODE CHANGE:[[Header]] Use [SHIFT and +] (German keyboard) or [SHIFT+8] (English keyboard) to 
switch between music and radio player.

The main screen consists of five panes, which are discussed below. The focuse between the
SEARCH, LOCAL AUDIO FILES and QUEUE can be changed via [TAB], where the [◀] is an indicator
for the current focus. Commands that work no matter which pane is focused are [n] for next
song in list and [b] song before current song in a list, [p] play/pause, [+/-] change
volume, [x] mute, [SHIFT+n] to toggle if either the filename or the meta data track name
is shown in the track lists, as well as all commands that open certain overlays (such as
the settings, oscilloscope parameter overlay etc.).

MAIN PANE:[[Header]]
Includes the Mousiki disk, meta data information, visualizations and
lyrics (only music player mode). Several adjustments to this pane (e.g. turning visualizations
on/off) can be made in the settings "ON/OFF" tab ([s] to open and save setting).

PROGRESS BAR: [[Header]]
Shows the progress of an audio file. Use [<-/->] to seek through a track (in steps of 5 s).

VOLUME, PLAY/PAUSE, PLAYEBACK MODES: [[Header_line]]
The volume can be changed via [+/-].
If a track shall be played or paused can be controlled via [p].
A track can be muted by using [x].
Playback modes such as e.g. R for repeat or S for shuffle can be changed via [m]. For more
details see PLAYBACK MODES[[Header]].

SEARCH BAR: [[Header]]
Type in a search query. See SEARCH[[HEADER]] and DOWNLOAD[[Header]] for details on what
can be specifically search for.

LOCAL AUDIO FILES: [[Header]]
When focused or using the search bar [UP]/[DOWN] can be used to hover through the track list.
[ENTER] starts a song and hitting [ENTER] again locks the current search result, so that only
the results are shown in the list. Hit [ESC] to clear a search result and the complete list
of local files will be shown again.

Hitting [a] adds a song on the top of a queue. See chapter on  BUILDING A QUEUE[[Header]] 
for more details.

Using [SHIFT+n] toggles between showing the filename / track name in the lists, [SHIFT+t]
changes the sorting of the local audio files.

The command [f] filters the local list by folder, which means, when a highlighted track is
e.g. in folder with other tracks from the same album, it will then only show the files in
the associated folder. Use [c] or [ESC] to clear folder filter.
[SHIFT+l] opens a BIG LIST OVERLAY[[HEADER]] for a better overview.
[SHIFT+f] opens the BIG FOLDER OVERLAY[[Header]].
)WIZ",
R"WIZ([SHIFT+k] opens the BIG QUEUE OVERLAY[[Header]].

QUEUE PANE: [[Header]]
See BUILDING A QUEUE[[Header]] for details on how to e.g. adjust the queue order, how to
save a queue to a list and more...


[[NEW PAGE]]
1.2 MUSIC FILE PATHS [[Header]]

ADDING/CHANGING PATHS: [[Header]]

HOW TO GET TO THE SETTINGS PATH TAB: [[Header]]
[s]  Settings [[Legend]] ->>  PATHS [[Header]] ->>  LOCAL PATH [[Header]] to set several paths.
For more details on all the SETTINGS TABS see chapter SETTINGS AND COLOR THEMES[[Header]]. 

Move onto a row, press [ENTER], type in folder path, hit [ENTER] again to change/add a path. 
An empty line plus [ENTER] removes that path. Paths accept "~" and "%USERPROFILE%" and both 
slash directions on Windows.
Committing a change rescans the library at once. Note that the download folder is included in
the list automatically, so you do not have to add it as a local path manually.

RESCAN LIBRARY: [[Header]]
Can also be done at any time via the command [SHIFT+r] when adding/deleting files while
Mousiki is currently running (restarting also triggers a new scan). 

IMPORTANT BEFORE LEAVING THE SETTINGS: [[Header]]
Remember to leave the settings by hitting [s] to save changes (made in the config.txt). 
Exiting via [ESC] or [q] exits the settings and discards changes that were made, but you
will also see a warning message appear when you try to do so...

See the chapter on SETTINGS AND COLOR THEMES[[Header]] for details on all the paths that can be set.

[[NEW PAGE]]
1.2.1 SUPPORTED FORMATS[[HEADER]]

Supported are the usual formats, including some unusual ones.

REGULAR FORMATS: [[Header]]
MP3, FLAC, WAV, OGG, Opus, M4A and other common formats play out of the box.

=> See FFmpeg for a full list of supported formats! [[Legend]]

Apart from that a variety of retro formats, which show up in the list below
are also supported and behave like any other track in search and the queue.
The below needs FFmpeg built with libopenmpt and libgme. When installing
Mousiki Plus it will upgrade FFmpeg, however the upgraded version has to be
on top in PATHs list, in case other or older versions were installed in the
past.

Tracker modules (Amiga / PC demoscene):[[Header]]
  .mod .xm .it .s3m .mptm .stm .669 .mtm .med .okt .far
  .ult .ams .dbm .digi .dmf .dsm .gdm .imf .j2b .mdl .mt2
  .psm .ptm .umx .plm
=> the above need FFmpeg built with libopenmpt[[Legend]]

Game console music:[[Header]]
  .nsf .nsfe (NES) .spc (SNES) .gbs (Game Boy)
  .vgm .vgz (Sega) .ay .hes .kss .sap .gym
=> needs FFmpeg built with libgme[[Legend]]

Commodore 64 SID tunes:[[Header]]
  .sid .psid .rsid
=> needs sidplayfp on the PATH (https://github.com/libsidplayfp/sidplayfp)[[Legend]]

In the meta data field of the main UI the TYPE column says tracker module,
game music or C64 SID (n tunes), and the module's own title is used as its
metadata title. If a required tool is missing, the status line says which
one and the track is skipped.

A SID tune has no end, so it is rendered once for SidPlayLength seconds
with a default of 180s.


[[NEW PAGE]]
2.1 SEARCH AND FILTERING CAPABILITIES [[Header]]

DIFFERENT SEARCH CAPABILITIES:[[Header]]
[/]  open/focus the search bar. The prefix picks the source:

[/]    +  query       your local library      live, as you type
[/s:]  +  query       YouTube                 live on [ENTER]
[/sc:] +  query       SoundCloud              live on [ENTER]
[/b:]  +  query       Bandcamp                live on [ENTER]
[/p:]  +  query       your saved playlists    live
[/f:]  +  query       your folders            live

The local search is fuzzy: "X Files" also finds "X-Files".
It matches the file name, the artist, the title tags and the
album, and ranks purely by how well things match.

GENERAL:[[HEADER]]
[UP] / [DOWN] while typing, then [ENTER] plays the highlighted result
right away
[ENTER] without moving keeps the search as the filter
[ESC] cancel and restore the unfiltered list view

NOTES ON SEARCH BEHAVIOR: [[HEADER]]
On a playlist row (/p:) [ENTER] queues all of its tracks.
On a folder row (/f:) [ENTER] opens that folder in the list, like [f].
Online searches only run on [ENTER], never per keystroke.
SoundCloud's 30-second previews and unplayable tracks are left out of
the results, which is why it takes a few seconds longer to search...



[[NEW PAGE]]
2.2 DOWNLOAD (MUSIC PLAYER) [[Header]]

SAVING WHAT YOU STREAM:[[Header]]
[y] saves the streamed track that is playing right now

SAVE FORMATS: [[Header]]
YouTube    =>  Opus
SoundCloud =>  MP3 / Opus / AAC, as the site sends it
Bandcamp   =>  the free 128 kbit/s MP3 stream

The file is named Title - Artist.ext, the cached copy is
removed, and the library refreshes so it appears as a local
track straight away.

DOWNLOAD LOCATION:[[HEADER]]
Set download PATH at [s] Settings[[Header]]  ->>  PATHS[[LEGEND]]  ->>  DOWNLOAD PATH [[Header]]
Default download path is inside the cache folder (~/.cache/mousiki).
The download folder is scanned automatically, so you never have to repeat it as a
local path. The radio puts its recordings in the same folder by default.

SHARED DOWNLOAD HISTORY OF MOUSIKI AND YT-DLP:[[Header]]
When using yt-dlp it places a downloaded_history.txt file inside the chosen download path. This
can lead to the case that the history file is accidentally moved and when e.g. downloading a playlist
to which you may have added new files, it will not download the new files only, but all of them again.
To avoid this the history is placed inside a folder called "history" which is placed within the
download path that was chosen in the setting. Mousiki avoids duplicates by checking for files with
the same file name and therefore does not rely on the history itself, but is built to add lines
to the history file that is created by yt-dlp using it outside Mousiki so duplicates are avoided.
When using yt-dlp outside of Mousiki it is recommended to point the history file to the same folder
Mousiki is creating inside the chosen download path as seen below (maybe create a shell profile
function for downloading songs and playlists from it). NOTE: When the download folder is changed in
Mousiki, the history folder will be copied (not moved!) to avoid history amnesia on the yt-dlp side.

YT-DLP COMMAND POINTING AT SHARED HISTORY FOLDER: [[Header]]
yt-dlp -x -P "<download folder>" --download-archive "<download folder>/history/downloaded_history.txt" <playlist-url>


[[New Page]]
3.1 PLAYBACK MODES [[Header]]

DIFFERENT PLAYBACK MODES:[[Header]]

There are five different playback modes. In the main UI the small box to the right of the search bar
indicates which playback mode is currently active. Use [m] to step through them.

S[[Header]] ==[[Header]] SHUFFLE MODE[[Header]]
Shuffles through tracks that are currently in the local audio files list (search filters apply!).
Note that you can also actively shuffle to next song in the list via [#].

L[[Header]] ==[[Header]] LIST MODE[[Header]]
Plays the next song in the list (filters apply!).

R[[Header]] ==[[Header]] REPEAT MODE[[Header]]
Repeats the current track again.

O[[Header]] ==[[Header]] STOP MODE[[Header]]
Plays the current song and then stops.

Q[[Header]] ==[[Header]] QUEUE THEN STOP MODE [[Header]]
Plays through the tracks in the queue and then stops.


[[NEW PAGE]]
3.2 BUILDING A QUEUE [[Header]]

Tracks can be added to a queue from the local audio files pane.
The order can be adjusted and queues can be saved as playlist. 

GENERAL: [[Header]]
[SHIFT+k]       open the big queue overlay (full size queue list)
[SHIFT+l]       open the big list  overlay (full size list)
[SHIFT+UP/DOWN] jump a whole page up/down

With the LIST focused: [[Header]]
[a]             add track to the TOP of the queue
[e]             add track to the END of the queue

With QUEUE focused: [[Header]]
[d]             remove from queue
[4/5]           move UP / DOWN
[$/%]           move to TOP / END
[a]             opens bulk-add panel: paste a YouTube playlist link to add

QUEUE related commands, no matter the focus: [[Header]]
)WIZ",
R"WIZ([!]             lock / unlock (default is locked: move to queue end when played)
[SHIFT+x]       clear it (asks first)
[CTRL+SHIFT+z]  undo the last clear (one level)
[CTRL+SHIFT+u]  save the queue as a playlist


[[NEW PAGE]]
3.3.1[[Header]] META DATA EDITOR[[Header]] —[[Header]] EDIT TAB[[HEADER]]

OPEN AND CHANGE TAB:[[HEADER]] Open via [SHIFT+m] and use [<-/->] to change tabs

NOTE:[[HEADER]] Use [SHIFT+r] to rescan, in case you just recently added new tracks to a
local path folder, otherwise they won't appear in the library list...

LAYOUT AND CHANGING FOCUS BEHAVIOR:[[Header]]
The SEARCH PANE [[HEADER]] uses fuzzy search on files and their meta data, if given. The
LIBRARY PANE [[Header]] only shows the file names. When the focus is on the LIBRARY PANE[[HEADER]] 
and hitting [ENTER] on a highlighted track, the focus moves to the EDIT PANE[[Header]]. Same can be 
achieved via [TAB] which changes the pane focus in general.

EDIT META DATA IN THE EDIT PANE:[[Header]]
Move the cursor to the row you want to change and hit [ENTER] to edit a meta data tag field.

SORT LIBRARY AND ADD TRACK TO FETCH META DATA TAB:[[Header]]
Several sort options are available for the LIBRARY PANE:

[a]         Add a track to the fetch list
[SHIFT+f]   Add full list to the fetch list
[r]         Show edited tracks first (highlighted in the same color as headers)
[x]         Show all tracks with NO META DATA
[SHIFT+t]   Show all track with MISSING TITLE tag
[SHIFT+a]   Show all tracks with MISSING ARTIST tag
[SHIFT+y]   Show all tracks with MISSING YEAR tag
[SHIFT+l]   Show all tracks with MISSING ALBUM tag

GENERAL BEHAVIOR OF META DATA EDITOR:[[Header]]
Every change goes into a pending edit session, autosaved after each keystroke.
Closing the editor, quitting, even a crash keeps it. Edited fields and their
library rows are drawn in the header colour, so you can see what will be written.
Also, when fetching takes a while and you exit the meta data menu, fetching does
not stop, will continue in the background.
Only [CTRL+s] writes to the files, and it asks if you want to keep changes. Same
goes for [CTRL+SHIFT+x] for discarding changes (not only the recent but all
currently autosaved changes!).
NOTE: Saving a high number of titles can take a few seconds!!![[Legend]]



[[NEW PAGE]]
3.3.2[[Header]] META DATA EDITOR[[Header]] —[[Header]] FETCH META DATA [[Header]]

USING ACOUSTID: [[Header]]
Meta data can be fetched via AcoustID using a locally created fingerprint via
chromaprint, so meta data can be fetched even for files without any tags
or usable file name. MusicBrainz is also involved — needed for album and year.

DISCLAIMER FOR FETCHING META DATA:[[Header]]
The fetched meta data is not always correct, so definitely check first, which
is why any fetching meta data comes with a disclaimer and needs a [YES/NO]
confirmation.
For tracks without usable title, you can use [SHIFT+p] to play a track, so you
can identify e.g. the artist / title etc. yourself to check if the fetched
meta data is correct or not.

FETCH META DATA FOR A SINGLE TRACK:[[Header]]
Meta data can also be fetched for single highlighted tracks in the LIBRARY PANE[[Header]].
Doing so via [SHIFT+b] only fetches missing tags. This can be done in the main screen list!
Using [SHIFT+n] fetches all tags, but only in the meta data menu. For a more detailed
control over what is changed and not, add tracks to the fetch list and mark what category you
want to be fetched via AcoustID/MusicBrainz (existing tags will be overwritten).

CHOOSE WHAT YOU WANT TO BE FETCHED AND CHANGED: [[Header]]
Categories are "all", "missing only", "title", "artist", "album", "year".
Album and year can lead to a choice that has to be made. The color used for legends
indicates that there is more than one option for album and year. Go to the respective
field and hit [ENTER] to open an overlay with the respective choices. Fetching an album
also fetches the year tag, since it is closely associated.

ADD TO FETCH LIST AND FETCH ALL:[[Header]]
Apply [a] to a highlighted track in the LIBRARY PANE[[Header]] to add a track to
the FETCH LIST PANE[[Header]]. For fetching data of all fetch listed tracks
at once hit [ENTER].
The command [DEL] or [d] deletes a track from the fetch list.
All edited tracks will also be highlighted and filtered by the [r] command in the
LIBRARY PANE [[Header]] of the EDIT TAB [[Header]].
NOTE: Saving a high number of titles can take a few seconds![[Legend]] The process
runs in the background, so you can also leave the menu without automatically
quitting a process. 


[[NEW PAGE]]
3.4 PLAYLIST EDITOR/CREATOR [[Header]]

OPEN AND CHANGE TABS:[[Header]] Open via [SHIFT+p], change tabs via [<-/->]

CREATE/EDIT TAB: [[Header]]
NAME: Choose a name for a playlist.
SEARCH AND ADD: Use the search function to search for titles and add via [ENTER]
REMOVE: Move the focus to the track list, choose and delete a track via [DEL/d]
SAVE/DISCARD: Save via [CTRL+s] and just choose [No] when exiting the playlist
editor to discard your current playlist.

PLAYLISTS TAB: [[Header]]
SEARCH AND ADD TO QUEUE: Search and choose a playlist and hit [ENTER] to add to queue.
EXPORT/IMPORT: Use [e/i] to import or export playlists in the M3U/M3U8 format. Playlists
created with Mousiki are stored in its own format, but they can be exported in M3U/M3U8.
DELETE PLAYLIST: Use [DEL/d] to delete a playlist.

[[New Page]]
3.5 FILE CONVERSION [[Header]]

Turns high resolution files into lighter ones, e.g. 24 bit / 96 kHz FLAC into MP3 for your phone.

OPEN:[[Header]] [SHIFT+c] opens the FILES TO CONVERT list (the library with format, sample rate,
bitrate, size and the estimated size after the conversion). 
NOTE:[[Header]] The original files are never changed/deleted in the conversion process!

CHOOSE WHAT TO CONVERT: [[Header]]
[SPACE]          mark / unmark single highlighted files          
[a]              mark all files shown in the (filtered) track list
[t] / [SHIFT+t]  pre-filter by type (all, lossless, FLAC, WAV, M4A, OGG, OPUS, AAC, MP3)
[r] / [SHIFT+r]  pre-filter by minimum bitrate (any, 320, 500, 1000 kbps)
[h]              hi-res only (more than 16 bit or more than 48 kHz)
[/] or [TAB]     search bar: folder and file name; use f: or p: to pick a folder or a playlist

CHOOSE THE TARGET: [[Header]]
[<-/->]          MP3 320 kbps, MP3 V0 (variable), MP3 192 kbps, AAC 256 kbps (.m4a), Opus 128 kbps

START: [[Header]]
[ENTER] converts the marked files (the highlighted one when nothing is marked!), [c] cancels a
running conversion, [CTRL+p] plays the highlighted file. [ESC] closes the overlay; a running
conversion goes on in the background and reports on the status line.

WHERE THE FILES GO: [[Header]]
Into the CONVERSION EXPORT PATH ([s] Settings[[Header]] ->> PATHS[[Header]]), by default the 
folder conversion-export inside the download folder, in the same sub folders as the originals.
A file that already exists there is skipped. Tags and cover are copied (not to Opus).
NOTE: Needs ffmpeg with libmp3lame and libopus; the installers provide it.[[Legend]]


[[New Page]]
4.1 SYNCED LYRICS [[Header]]

When the lyrics engine is on, Mousiki fetches the lyrics of the playing track and follows along
line by line, and word by word where the source has word timings (depends on the structure of the 
imported file). They are shown in the field right of the meta data (the VIZ/LYRICS FIELD[[Header]] of the main pane): the line that was sung and the following one are dimmed, the line being sung right now is highlighted. However, the coloring / highlighting behavior depends on chosen options in the SETTINGS[[Header]] in the tab COLORS[[Header]]. When no colors are applied, the text will just move on and may eventually be underlined, in
case the fetched file allows for such behavior.

SWITCH THROUGH THE FIELD: [[Header]]
[,/.]   cycles the field lyrics, sphere, oscilloscope, spectrogram back and forth; for more 
        details on those options see the chapter on VISUALIZATIONS
)WIZ",
R"WIZ([k]     opens the KARAOKE MODE[[Header]] (see respective chapter for details)

LOOK OF THE LYRICS (ADJUSTABLE IN THE SETTINGS): [[Header]]
Colors:[[Header]] settings tab COLORS, rows LYRICS (inactive line, active line, active word).
Alignment and reveal style:[[Header]] settings tab ANIMA/VIZ, rows Lyrics Alignment (left / 
center / right) and Lyrics Animation (full, word by word, line by line, letter by letter, 
active line only, active word only).

SWITCH LYRICS OFF: [[Header]]
Use Lyrics Engine = false (settings tab ON/OFF) which switches the lyrics off completely: 
nothing is fetched and [,/.] skips this mode.

NO LYRICS FOUND: [[Header]]
The setting No-Lyrics Viz (tab ANIMA/VIZ) decides what is shown while the lyrics view has no
lyrics: sphere, oscilloscope, spectrogram or nothing. 

[[New Page]]
4.2 LYRICS RETRY, TIMING AND READING [[Header]]

WRONG MATCH? RETRY:[[Header]]
In case the lyrics don't fit, e.g. given a different version of a song witch alternative
verses (not the timing!), lyrics can be refetched under a different query. This can be done
via the command [l], which opens the retry form (the lyrics engine must be on!).
Fields:[[Header]] TITLE and ARTIST, plus ticks that help with edits whose timing differ by 
concept from the original: slowed, ultra-slowed, sped up (these three exclude each other), 
reverb, remix, other. [TAB]/[DOWN] next field, [UP] previous field, [SPACE] tick/untick, 
[ENTER] fetch, [ESC] cancel. Fetching lyrics again does not replace older version, as long 
there had been timing adjustments or the lyrics differ. Lyrics can be deleted in the re-fetch
form or in the read/translate lyrics overlay which can be accessed via [Ctrl+l] (see further
below for details).    

LYRICS RUNNING EARLY OR LATE: THE TIMING OVERLAY[[Header]]
[ALT+L] opens the overlay, on bothe the main screen and in karaoke mode (only while synced 
lyrics are loaded). The lines move live while you adjust, so you can judge the offset by ear.
[->] / [<-]       lyrics later / earlier by 0.1 s
[UP] / [DOWN]     lyrics later / earlier by 0.5 s       [R] back to 0 (range +/- 120 s)
[SHIFT + <-/->]   seek currently playling track back / forth 
[ENTER] or [S]    save into the lyrics file of the track
[ESC] or [ALT+L]  cancel, the old offset comes back
A positive offset makes the lyrics appear later, so use it when they are too early. Saving writes
a standard [offset:...] tag into the .lrc file, which other players understand too.

READING AND TRANSLATING LYRICS:[[Header]]
[CTRL+L] opens READ LYRICS (playing track, else the hovered one). It shows the lyrics fetched 
so far and the translations and the text itseld. [TAB] switches between the two top panes, 
[<-/->] chooses the file or the translation, the scrolling commands are [UP]/[DOWN] for a line,
[SPACE]/[b] for a page and [HOME] / [END] for top/bottom. 
[t] opens the translate overlay (service (Google / MyMemory), translate into, lyrics are in), 
the command [ENTER] fetches.
[SHIFT+t] shows / cycles / hides translations, [d] or [DEL] deletes a translation.
Fetching translations goes on in the background and may take a while; asking for another one 
meanwhile then only gives a warning.


[[New Page]]
4.3 KARAOKE MODE [[Header]]

OPEN AND CLOSE:[[Header]] [k] opens the karaoke overlay, [ESC] or [k] close it.

The lyrics are shown over the whole screen. The coloring behavior again matches the colors
applied in the SETTING COLORS TAB[[Header]]. Lines are centered, with an empty row between them. On both sides a ASCII-Braille image saying "Karaoke" in Japanese is drawn in your disk colors, 
with a wave running across it. On a narrow terminals the images are left out and the lyrics 
take the whole width...

NOTE:[[Header]] The overlay always uses the lyrics engine, also when it is switched off on the main screen.
It fetches the lyrics itself then (and again after a track change) without changing that setting.

MAKE THE LYRICS BIGGER: [[Header]]
[*] / [_]   size 1 to 5      German keyboard: [SHIFT and +] / [SHIFT and -]
                             English keyboard: [SHIFT+8] / [SHIFT and -]
Size 1 is normal terminal text. Sizes 2 to 5 use Mousiki's own pixel font drawn in braille dots
(3 x 2, 6 x 4, 9 x 6 and 12 x 8 cells per letter). Only the lyrics change size, long lines wrap
between words. The bottom border shows the size for a moment, and it is saved when you close.
The basic font covers Latin letters, digits, punctuation, umlauts and common accented letters.
Another font for Japanese, Chinese, Korean, Cyrillic and Greek letters is also available but
may have different sizing options.

KEYS THAT KEEP WORKING IN KARAOKE: [[Header]]
[p] play/pause   [n] / [b] next / previous   [#] shuffle   [<-/->] seek   [+/-] volume
[x] mute   [m] mode   [v] normalization
[ALT+L] the lyrics timing overlay, right here over the big lyrics: the easiest place to judge an offset


[[New Page]]
5.1 THE VISUALIZATION FIELD [[Header]]

Three visualizations, one key. The music player shows them in the field right of the meta data
(where the lyrics are), the radio in the scope block right of the station info.

[,/.]   cycles the field: lyrics (player only), sphere, oscilloscope, spectrogram back and forth

WHAT IS IN THE CYCLE: [[Header]]
A view that is switched off in the settings tab ON/OFF (Use Lyrics Engine, Use Oscilloscope,
Use Spectrogram, Use Sphere) is skipped by [,/.]. The order of the views is set on the tab ANIMA/VIZ
(Viz Order 1 to 4). The view Mousiki starts with is Lyrics/Viz Default on the same tab; [,/.] only
changes the visualizations while the program runs.

WHAT THEY SHOW: [[Header]]
SPHERE:[[Header]] an audio-reactive sphere (uses same FFT band data as te spectrum strip below
the meta data field in the main UI. 
OSCILLOSCOPE:[[Header]] Available are a XY and YT mode oscilloscope.
SPECTROGRAM:[[Header]] time runs left to right, frequency upwards, the color is the strength.
All of them analyze the decoded signal, so volume, equalizer and normalization never change
the picture.

TUNING AND EXTRA VIEWS: [[Header]]
[SHIFT+o/0] oscilloscope tuning/overlay; [SHIFT+i/u] spectrogram options/overlay; 
[SHIFT+(/)] extra window mode for each.

[[New Page]]
5.2 OSCILLOSCOPE [[Header]]

THE XY OSCILLOSCOPE: [[Header]]
Like a real scope in X-Y mode. Wide stereo fills the area, a phase-shifted tone draws an ellipse.
A nearly mono track fades over to a phase portrait instead of a flat line.

TUNING OVERLAY:[[Header]] [SHIFT+o] opens it, the picture changes live while the music plays.
Scope mode       XY, YT, XY + YT (overlap), XY + YT (row), XY + YT (column)
Osci style       braille (works in any terminal) / image (pixels, needs Kitty graphics or Sixel)
Osci music mode  for oscilloscope music: the unprocessed signal and a fixed scale
Beam             CRT: every sample drawn once, light by the time the beam spends on a spot
                 (image default) / classic: the old look with a tail (braille default)
Dwell brightness CRT beam: slow strokes bright, jumps dark; off = every stroke equally bright
Low-rate blank.  music mode, CRT beam: files below 96 kHz get the jumps between shapes faded out
Rough mode       not in music mode: loud beats clip along the edges of the square (edge clipping;
                 braille default on, image default off)
Frame rate       30 / 45 / 60 / 90 / 120 / 165 fps
Image res/detail image style: drawn at full / half / third resolution; detail normal / high =
                 up to 560 / 1024 pixels wide (high: sharper thin lines, more data per frame)
Decay            the afterglow, higher = longer trails
Tail / Glow      bright head with fading tail / line glow
Z-Axis           beam brightness follows speed or level
Color            the VIZ gradient or 8 palettes
Each style keeps its own values, so switching between braille and image never loses your tuning.
[R] resets only the style in use.
Starting points for braille: crisp scope = Decay 0.65, Dot threshold 0.35, Tail 0.60.
Dreamy glow = 0.90, 0.20, 0.25.
)WIZ",
R"WIZ(Oscilloscope music (e.g. Jerobeam Fenderson): Osci music mode on, Beam CRT, Decay about 0.10,
Image resolution full, Image detail high -- close to osci-render. Lossless files work best.

THE BIG OSCILLOSCOPE: [[Header]]
[SHIFT+0] (the [=] key; German keyboard SHIFT+0) opens it over the whole screen: YT (amplitude over
time) on the left, XY on the right. [SHIFT+o] opens the options on top.
[SHIFT+a] grid and frame on/off     [SHIFT+v] axis values: percent, milliseconds under YT
[SHIFT+c] grid color (0-255)        [SHIFT+p] grid metrics: time unit, divisions, calm readouts
With Osci style: image and a terminal that speaks Kitty graphics or Sixel both pictures are real
pixel images; otherwise they are drawn in braille dots. 100 % on the axes is the loudest part of the
signal (full scale in Osci music mode).


[[New Page]]
5.3 SPECTROGRAM AND EXTRA WINDOWS [[Header]]

THE SPECTROGRAM: [[Header]]
Made the way Audacity makes its spectrogram view, with Audacity's defaults (2048-sample Hann window,
zero padding 2, Mel scale 0 to 20 000 Hz, gain 20 / range 80 dB, Roseus colors). Stereo is drawn
as two channels.
[SHIFT+i] options        [SHIFT+u] full screen (with frequency labels on the left and a time ruler on top)
Scale        linear / logarithmic / mel / bark / erb / period
Motion       sweep (a page at a time) / scroll (newest on the right)
Window size  256 .. 8192: bigger = sharper frequency lines, smaller = crisper clicks and drum hits
Time span    5 .. 60 s across the width
Colors       roseus / classic / grayscale / your VIZ gradient
[R] puts everything back to Audacity's defaults. A change is applied to the last minute of
audio at once, so you see the difference immediately.

THEIR OWN WINDOWS: [[Header]]
Both visualizations can leave the terminal and open a real window, redrawn at your monitor's
refresh rate (60, 120, 165 Hz ...):
[)]   scope window            German keyboard SHIFT+9, English keyboard SHIFT+0
[(]   spectrogram window      German keyboard SHIFT+8, English keyboard SHIFT+9
In the window: [F] or [F11] switches to fullscreen, [T] always on top, [ESC] or [Q] close.
Each window is a separate process that gets the audio through a pipe, so a graphics driver
problem can only close the window, never stall the music or the terminal.
Position and size are remembered, and the windows stay open when you switch player <-> radio.

NOTE: They need SDL2 (Windows packages include it; Linux: libsdl2-2.0-0, macOS: brew install sdl2).[[Legend]]


[[New Page]]
6.1 EQUALIZER [[Header]]

OPEN:[[Header]] [SHIFT+e] opens the 10-band equalizer; the sound changes while you listen.
The bands are 31, 62, 125, 250, 500 Hz and 1, 2, 4, 8, 16 kHz, each from -12 to +12 dB.

KEYS IN THE EQUALIZER: [[Header]]
[<-/->]    pick a band                 [UP]/[DOWN]   +/- 1 dB
[,] / [.]  previous / next preset      [0]           set the band to 0 dB
[SPACE]    equalizer on / off          [R]           back to Flat
[S]        save the curve as your own preset (up to 24)
[DEL]      delete the selected custom preset (press twice)

PRESETS: [[Header]]
Twelve presets are built in (Flat, Bass Boost, Treble Boost, Vocal, Rock, Pop, Jazz, Classical,
Electronic, Hip-Hop, Acoustic, Loudness), then come your own.

PREAMP: [[Header]]
Boosting bands would clip, so the level is lowered by the height of your loudest point (e.g. -7.1 dB
for Bass Boost). A strong boost is therefore slightly quieter: turn the volume up. When the
equalizer is off it is bypassed completely.

NOTE:[[Header]] The radio keeps its own curve and its own presets (in radio_config.txt).


[[New Page]]
6.2 LOUDNESS NORMALIZATION AND STEREO/MONO [[Header]]

LOUDNESS NORMALIZATION: [[Header]]
Plays every track at the same perceived loudness, so a quiet album does not disappear after a loud one.
[v]        on / off, without opening anything
[SHIFT+v]  the overlay: on/off, target level, max boost

Normalize     off / on                      (default on)
Target level  -40 .. 0 LUFS                 (default -16)
Max boost     0 .. 24 dB                    (default 9)

-16 LUFS leaves headroom, -14 matches YouTube and Spotify. The grey live line under the rows shows
the measured loudness of the playing track and the gain actually applied, so you can see at once
when Max boost is holding a quiet track back. In the radio a stream has no end, so the loudness is
measured since you tuned in and the gain glides in over a second.

STEREO OR MONO: [[Header]]
Settings ->> ON/OFF ->> Stereo Sound. Off folds left and right together (and halves the memory a
loaded track needs). Turning it off is immediate; turning it on applies from the next track.
In the radio the footer of the dial then reads MONO.


[[New Page]]
6.3 SAMPLE RATE AND PLAYBACK ENGINE [[Header]]

SAMPLE RATE: NOTHING IS CONVERTED ON THE WAY: [[Header]]
Every file plays at its own sample rate (44.1, 48, 88.2, 96 kHz ...): it is decoded at that rate and
the audio device is opened with it. Above 96 kHz the track is brought down to 96 kHz. Where a
conversion cannot be avoided a long, sharp filter is used, not a cheap linear one.

LONG TRACKS NEED LITTLE MEMORY: [[Header]]
A short track is decoded completely into memory. A long mix is only decoded in a 2-minute window
around the position (90 s ahead, the last 30 s kept, so a short step back is instant).
A 200 MB file takes about 50 MB instead of gigabytes. The audio is exactly the same: same rate,
same 32-bit samples.

THE ORDER THINGS HAPPEN IN: [[Header]]
decoded audio  >  equalizer  >  normalization  >  limiter  >  volume
The scopes can tap the signal before all of it when the Osci music mode in the [SHIFT+o] overlay
is turned on. The spectrogram always decodes before all of the above. The spectrum bar and
the sphere use the (mono) mix of a track so they use the end signal of the above (after volume).


[[New Page]]
6.4 SLEEP TIMER [[Header]]

OPEN:[[Header]] [SHIFT+z] opens the sleep timer overlay (the playback keeps running).

15 / 30 / 60 / 90 / 120 minutes   set the timer
Stop after current song           player only
Fade out                          a toggle, the overlay stays open
Off                               cancel a running timer

[UP]/[DOWN] pick, [ENTER] set it and close, [ESC] or [SHIFT+z] close without changing anything.

WHILE A TIMER RUNS: [[Header]]
The remaining time is shown in the title of the search bar: SEARCH LOCAL [SLEEP 24:10].

FADE OUT: [[Header]]
Fade out (on by default) glides the volume down over the last 10 % of the time, at least 30 s and at
most 10 minutes, so the music fades away instead of stopping abruptly.

WHEN THE TIME IS UP: [[Header]]
The player pauses (the position is kept, [p] resumes), the radio stops the stream. The timer never
changes the play mode, and it is gone after a restart.


[[New Page]]
7.1 LISTENING HISTORY [[Header]]

OPEN: [[Header]] [SHIFT+h] opens the history; [ESC], [q] or [SHIFT+h] close it.
Jump to a tab with [1] - [4] or move with [<-/->].

THE FOUR TABS: [[Header]]
[1] HISTORY          the last 100 plays: when, what, how long, and PLAY / done / skip
[2] TOP TRACKS       one row per title with its play count
[3] TOP PLAYLISTS    the top 5 saved playlists of the week, month, quarter, year and overall
[4] HABITS           sessions, time per day, tracks per session, skips, replays, completion

KEYS: [[Header]]
[UP]/[DOWN]   move (on TOP PLAYLISTS and HABITS they scroll)
[ENTER]       on HISTORY or TOP TRACKS: play it now (the overlay stays open)
[r]           on TOP TRACKS: most played <-> least played
Nothing you do in here changes your music.

GOOD TO KNOW: [[Header]]
A play counts for a playlist when it was queued from a saved playlist ([ENTER] or [a] on a playlist
row). The history keeps the newest 1000 plays one by one and folds older ones into lifetime totals
per title, including the day and the time of day of every play, so the period and time-of-day lists
stay exact over years. It lives in history.json, in [s] Settings[[Header]] ->> PATHS[[Header]] ->> HISTORY PATH[[Header]].
)WIZ",
R"WIZ(
[[New Page]]
7.2 SMART HISTORY LISTS [[Header]]

ADD SMART HISTORY TO QUEUE: [[Header]]
Under the TOP TRACKS list sits a pane with sixteen ready-made lists in four columns:

TOP TRACKS    TOP OF THE...    TIME OF DAY    REDISCOVER
Top 10        the week         Morning        newly added
Top 25        the month        Day            least played
Top 50        the quarter      Evening        least: month
Top 100       the year         Night          least: year

HOW TO USE THEM: [[Header]]
[2] (or [->])     go to TOP TRACKS
[TAB]             move into the pane (a [◀] appears behind its title)
arrow keys        pick a list; while the pane has the focus [<-/->] move between the columns
[ENTER]           add that list to the end of the queue
[ESC] or [TAB]    back to the track list

TIMES OF DAY: [[Header]]
Morning 7-11, Day 11-18, Evening 18-22, Night 22-7.

REDISCOVER: [[Header]]
Least played counts never-played tracks as 0 and shuffles ties every time, which makes it a good way
to rediscover forgotten music.

Files that moved or were deleted are skipped, and the status line says how many tracks were queued.

[[New Page]]
8.1 SETTINGS BASICS [[Header]]

OPEN:[[Header]] [s] opens the settings, always on the first tab. Six tabs:
COLORS; ON/OFF; ANIMA/VIZ; PATHS; REFERENCE; ABOUT APP ([TAB] steps through them)

MOVING AND EDITING: [[Header]]
[UP]/[DOWN]  previous / next row         [<-/->]  step through the values of a row, applied at once
[ENTER]      edit the value as free text

SAVING AND LEAVING: [[Header]]
Every change is live: you see and hear it while you are still in the settings. But only [s] writes
it to config.txt and closes. [ESC] or [q] discards. With unsaved changes the settings ask first:
[s] save, [d] discard, [ESC] keep editing. Discarding puts every row back the way it was when you
opened the settings, paths included. While there are unsaved changes a note says so.

TEXT EDITING, IN EVERY FIELD: [[Header]]
[<-/->] caret            [SHIFT+<-/->] mark          [HOME] [END] jump
[DEL] [BACKSPACE] delete [CTRL+c] [CTRL+x] [CTRL+v]  copy, cut, paste
[ENTER] confirm          [ESC] cancel the edit
[TAB] and the arrows never leave a field that is being edited.


[[New Page]]
8.2 COLORS AND COLOR THEMES [[Header]]

THE COLORS TAB: [[Header]]
A THEME row and the color rows, each with one or two value cells, a swatch and a live preview.
Colors are numbers of the 256-color terminal palette (1-255) in every field, text or background;
0 or empty means "no color" (the terminal's own). Black is 16.
BORDER_COLOR, DISK, METADATA, VIZ, PROGRESS_BAR, LIST, QUEUE, LYRICS, HEADER, LEGEND, TAB_NAMES,
CHANGE (a field you type in) and FOCUS (the [◀] behind the title of an active pane).
[<-/->] pick the cell, then [ENTER] to type a number.
The border can fade from the top to the bottom color line by line (BorderGradient smooth / rough).

COLOR THEMES: [[Header]]
The first row, THEME, holds ready-made schemes (the default MeowerShell, Serious MeowerShell and
many more) and the themes you made yourself.
[ENTER]        on the theme name: the list; [ENTER] applies a theme
[SHIFT+d]      in that list: delete a theme of your own (asks first)
[ENTER]        on CREATE THEME: the theme editor
[e]            edit the theme in use in that editor
[SHIFT+e / i]  export / import a theme as a .theme file
In the editor type a name (25 characters at most), optionally pick a template, change any cell and
press [s] to save and apply it. A theme of your own can be overwritten (Mousiki asks first), a
built-in theme needs a new name. [ESC] with unsaved changes asks first, leaving the settings does too.


[[New Page]]
8.3 ON/OFF, ANIMA/VIZ AND PATHS [[Header]]

ON/OFF TAB: [[Header]] every row is changed with [<-/->]
MODES: Playback (like [m]), Start Mode (player / radio: which of the two Mousiki opens with)
Eliment Disk, Dummy Buttons, Queue Display, Progress Waveform, Spectrum Bars, Stereo Sound,
Normalize Volume, Replace Emoji, Marquee Scrolling
VIZ/LYRICS FIELD: Viz/Lyrics Field (false = no field, the meta data take its place) and the switches
Use Lyrics Engine / Oscilloscope / Spectrogram / Sphere. False leaves that view out of the [,/.] cycle.

ANIMA/VIZ TAB: [[Header]]
Lyrics Alignment and Lyrics Animation, Disk Speed (0.01 .. 1.00), Bars Fluidity / Degradation /
Viscosity (1-10), Waveform Style (raw / smooth, like [w]), Lyrics/Viz Default, No-Lyrics Viz and
Viz Order 1..4 (the order of the views in the [.] cycle).

PATHS TAB: [[Header]]
LOCAL PATH             the folders scanned for music (several, plus "+ new path")
DOWNLOAD PATH          where [y] saves streams
PLAYLIST PATH          one folder for all playlists
PLAYLIST EXPORT PATH   where [e] writes M3U8 / M3U
HISTORY PATH           holds history.json
CONVERSION EXPORT PATH where converted files go
Changing LOCAL PATH rescans at once. Changing the playlist or history folder copies what is there;
nothing is moved or deleted. Changed folders only stick when you save with [s]. A warning will
appear in case you unsaved changes are present. 


[[New Page]]
8.4 HOTKEYS, REFERENCE AND ABOUT [[Header]]

REFERENCE TAB: [[Header]]
The longest tab. It lists every rebindable command, grouped like the cheat sheet (PLAYBACK,
NAVIGATION & VIEW, LYRICS & KARAOKE, VISUALIZATIONS, SEARCH, QUEUE, PLAYLISTS, META EDITOR, HISTORY,
EQUALIZER, DOWNLOADS, LIBRARY, SYSTEM), and the FONT MAP (read-only).
[ENTER] on a hotkey row, type the new key, [ENTER] again.

HOW A KEY IS WRITTEN: [[Header]]
As the character itself (n, T, #, +; a capital letter is SHIFT + the letter) or as a name: ENTER,
TAB, SPACE, BACKSPACE, ARROW_KEY_UP / DOWN / LEFT / RIGHT. A combination ([ALT+L], [CTRL+L],
[CTRL+SHIFT+u], [CTRL+SHIFT+z]) is entered by pressing it. An empty field unbinds the command.

SAFETY: [[Header]]
A key that is already used by another command is refused with a message naming it, so you can
never lose a command by accident. [*] and [ESC] are reserved. Keys that are not listed (the keys of
editors and overlays, [ENTER], the arrows, [CTRL+s] ...) are fixed. The cheat sheet [?] lists
those, and it always shows the default keys.

ABOUT APP TAB: [[Header]]
The credits and versions, scrolled with [UP]/[DOWN].


[[New Page]]
RADIO PLAYER MODE: [[Header]]

The radio player is the second half of Mousiki Plus. Switch with [SHIFT and +] (German keyboard) or
[SHIFT+8] (English keyboard) in both directions.

PLAYER -> RADIO:[[Header]] the player is paused and kept in memory (queue, position and screen).
RADIO -> PLAYER:[[Header]] the radio is closed completely, so it only uses resources while you are in it.
Files added to your music folders meanwhile are found after [SHIFT+r] in the player.
[q] (or [CTRL+c]) in the radio quits the program, both modes end.

WHAT THE RADIO SHARES WITH THE PLAYER: [[Header]]
Equalizer [SHIFT+e], oscilloscope [SHIFT+o] / [SHIFT+0], spectrogram [SHIFT+i] / [SHIFT+u], scope and
spectrogram windows [)] / [(], loudness normalization [v] / [SHIFT+v], sleep timer [SHIFT+z], text fields,
the cheat sheet [?] and this wizard [SHIFT+w] work with the same keys and overlays as described in
chapters 5 to 8. Differences: the radio keeps its own equalizer curve, oscilloscope values and
settings in radio_config.txt; the sleep timer stops the stream (no "Stop after current song"); the
visualization field is the scope block right of the station info and [,/.] cycles oscilloscope,
sphere and spectrogram (there are no lyrics).

LETTERS ARE MORE OFTEN CASE SENSITIVE IN THE RADIO: [[Header]]
[r] recalls a preset slot, [R] reconnects the station.

[[New Page]]
9.1 THE RADIO SCREEN AND ITS KEYS [[Header]]

FROM TOP TO BOTTOM: [[Header]]
TOP PANE:[[Header]] the ON AIR sign (OFF AIR while nothing is live), the station info (station, title,
artist, genre, bitrate, sampling rate, type, country), a spectrum under it and the oscilloscope,
sphere or spectrogram on the right.
FREQUENCY BAND (MHz):[[Header]] a tuning dial. The needle shows the tuned station, dots mark the other
)WIZ",
R"WIZ(stations of your list. Below it: signal strength, STEREO or MONO, LIVE and the time since you tuned in
(PAUSED -m:ss while paused, TIMESHIFT -m:ss while you listen behind live) and REC mm:ss while recording.
SEARCH STATIONS:[[Header]] with the play mode letter on its right (S shuffle / L list, [m] toggles; it
only affects [b]).
STATIONS and PRESETS:[[Header]] your station list and the 16 slots of the active preset side by side.

KEYS ON THE MAIN SCREEN: [[Header]]
[UP]/[DOWN]  move in the list           [ENTER]  tune the hovered station
[n] / [b]    next / previous channel    [#]      a random channel
[p]          mute / unmute              [x]      stop          [R]  reconnect
[+] / [-]    volume                     [/]      search        [SHIFT+t]  sort list order <-> A-Z
[SHIFT+k] presets   [SHIFT+s] radio browser   [SHIFT+p] station lists   [SHIFT+l] big stations overlay
[h] history   [s] settings   [?] cheat sheet   [SHIFT+w] this wizard   [q] quit

THE BIG STATIONS OVERLAY: [[Header]]
[SHIFT+l] shows the STATIONS pane over the whole screen plus a PRESET NAME column.
[a] adds a station by its stream URL ([TAB] switches URL <-> NAME; an empty name becomes the host name).
[SHIFT+c] gives the hovered station a preset name: a shorter second name shown in the PRESETS pane.

[[New Page]]
9.2 PRESETS [[Header]]

A preset is a named set of 16 station slots on the keys:
1  2  3  4  5  6  7  8  9  0  e  r  t  d  f  g
Press one on the main screen to recall that slot. [SHIFT+<-/->] switch to the previous / next
preset. The name of the active preset is in the pane title: e.g. PRESETS (Morning).

THE PRESETS MENU:[[Header]] [SHIFT+k] opens it. A small [◀] behind a pane's title shows which pane is
focused, just as in the music palyer. Names are 1 to 24 characters and unique. Every change is written 
to presets.txt at once.

KEYS IN THE PRESETS MENU: [[Header]]
[TAB]        SEARCH > SELECT PRESET > STATIONS
[ENTER]      in STATIONS: tune it. In SELECT PRESET: open that preset and tune its first filled slot
[1]..[0], [e r t d f g]   in STATIONS: put the hovered station in that slot; the same key again clears it
[DEL]        remove the hovered station from the preset
[SHIFT+n]    new preset              [SHIFT+c]  rename the hovered one
[SHIFT+d]    delete it (press twice; the last one stays)
[/]          back to the search box           [ESC]  close

NOTE: A station sits in one slot only: putting it in another slot moves it instead of duplicating it.[[Legend]]


[[New Page]]
10.1 SEARCHING STATIONS AND THE RADIO BROWSER [[Header]]

SEARCHING YOUR OWN STATIONS: [[Header]]
[/] opens the search box under the frequency band. The STATIONS pane filters live as you type; 
[ENTER] tunes the highlighted station and [ESC] clears the search.
(a name, genre or country)   filters your stations
[s:]                         back to searching stations
[p:]                         your saved STATION LISTS; [ENTER] fills the pane with that list and tunes
                             its first station
While a list is shown the pane title reads STATIONS (LIST: name - ESC: all) and [n], [b] and [#] surf
only inside that list. [ESC] clears a search still in the box; the next [ESC] returns to all stations.
The search is fuzzy and typo tolerant; the characters - _ . / count as spaces.

FINDING NEW STATIONS: THE RADIO BROWSER: [[Header]]
[SHIFT+s] searches the public directory radio-browser.info (needs curl).
Fields: NAME, TAGS (comma separated, every tag has to match), COUNTRY (name or two-letter code), STATE,
LANGUAGE, BITRATE (128 = at least 128, 64-192 = range, -192 = at most), then the RESULTS.
[TAB]    NAME > TAGS > COUNTRY > STATE > LANGUAGE > BITRATE > RESULTS > NAME
[ENTER]  in a field: search. In RESULTS: tune the station.
[a]      in RESULTS: add it to your station list (a dot in front means you already have it)
[ESC]    clear a field; on an empty field or in RESULTS: close
Every search hides broken streams, sorts by clicks and returns at most 200 results.

[[New Page]]
10.2 STATION LISTS [[Header]]

Named, ordered collections of stations (a "jazz" list, a "morning" list), built exactly like the
playlist editor of the music player.

OPEN AND CHANGE TABS: [[Header]] [SHIFT+p] opens the menu, [<-/->] switch the two tabs ([ALT+<-/->]
from every pane, also while typing). Lists are stored in stationlists.txt.

KEYS IN THE STATION LISTS MENU: [[Header]]
[TAB]           name > search > STATIONS > LIST CONTENTS
[ENTER]         in STATIONS: add the station to the end of the list.
                In LIST CONTENTS: tune it, to audition the list
[4] / [5]       move the hovered station up / down
[DEL] or [d]    remove it
[SHIFT+t]       sort STATIONS: list order <-> name A-Z
[CTRL+s]        save under the typed name and stay in the menu
[ESC]           leave a text box; again: close (it asks about unsaved changes)
A dot behind a station means it is already in the list being built.

SAVED LISTS TAB: [[Header]]
[ENTER] loads a list back into the first tab, [DEL] deletes it after a Yes/No question.

USE A LIST: [[Header]]
Type [p:] in the search box on the main screen. [n], [b] and [#] then surf only inside that list.

[[New Page]]
11.1 TIMESHIFT [[Header]]

From the moment you tune a station the radio keeps the last minutes of it in a rolling buffer on
disk, so live radio can be treated like a recording.

KEYS: [[Header]]
[SPACE]   pause. The station keeps arriving in the buffer; [SPACE] again resumes where you stopped
the square bracket keys (left / right)   back / forward 30 seconds
                                         (German keyboard: [ALT GR+8] / [ALT GR+9])
the curly bracket keys (left / right)    back 5 minutes / back to live
                                         (German keyboard: [ALT GR+7] / [ALT GR+0])
Every jump is shown for 4 seconds under the scope block, e.g. -0:30 | 2:30 behind live.
You can never go back before the moment you tuned in: internet radio only delivers from "now".
Catching up with the forward key hands back to the live stream without a gap. Tuning another
station, [x] or quitting clears the buffer.

THE BUFFER: [[Header]]
The size is [s] Settings[[Header]] ->> ON/OFF[[Header]] ->> Timeshift buffer (5, 15, 30, 45 or 60 minutes,
default 30). The file is radio_timeshift.pcm in ~/.cache/mousiki/ and takes about 11.5 MB per minute
(about 345 MB for 30 minutes). It is written by the stream thread and never touched by the screen
loop, so the interface stays as fast as without it. It is deleted when the radio closes.
If a pause lasts longer than the buffer, the oldest part is overwritten and playback continues
from the oldest audio that is still there.



[[New Page]]
11.2 RECORDING [[Header]]

RECORD, INCLUDING THE MINUTES BEFORE YOU PRESSED: [[Header]]
[y] opens the RECORD overlay. It freezes the moment you pressed, so no time is lost while you choose:
-> From now on (when this opened)
-> The last 1 / 5 / 10 / 15 / 30 minutes
-> Everything buffered
A row says "only m:ss" when the buffer does not reach that far back and then starts at the oldest audio.

[UP]/[DOWN] choose   [ENTER] or [y] start   [ESC] cancel   [y] again while it runs: stop
While it runs the footer of the dial shows REC mm:ss.

WHAT YOU GET: [[Header]]
The recording is written from the buffer as a WAV, which ffmpeg then converts in the background to
YYYY-MM-DD_HH-MM-SS.mp3, named by the time the first recorded second was on air. It is taken before
volume, mute and the mono fold.

WHERE IT GOES: [[Header]]
Into the download folder of the radio ([s] Settings[[Header]] ->> PATHS[[Header]]), which by default is
the one of the music player. After a rescan ([SHIFT+r] in the player) the recordings are part of
your local library.


[[New Page]]
11.3 RADIO HISTORY [[Header]]

THE RADIO'S OWN HISTORY: [[Header]]
[h] opens it; [ESC], [q] or [h] close it. [1] [2] [3], [<-/->] or [TAB] switch the tab, [UP]/[DOWN]
move, [HOME]/[END] jump to the ends.
[1] HISTORY        WHEN / CHANNEL / ARTIST / TITLE / HEARD
)WIZ",
R"WIZ([2] TOP CHANNELS   channels by time listened ([r] flips the order)
[3] HABITS         sessions, time per day, channels, and listening by hour and by weekday
A new HISTORY line starts whenever the channel, the artist or the title changes. Lines heard for less
than 3 seconds are dropped. A session ends after a gap of 30 minutes. The newest 10 000 lines are
kept one by one; older ones are folded into lifetime totals, so TOP CHANNELS and HABITS stay lifetime
figures.

HEARD A SONG ON THE RADIO? DOWNLOAD IT: [[Header]]
The radio writes down the artist and title the stream sends. On the HISTORY tab [y] on a line opens an
overlay that searches YouTube for "artist title". [ENTER] downloads the hovered result into the
download folder (yt-dlp, opus), [TAB] edits the search query, [ESC] closes. A result is marked
[ok] done, [..] downloading, [!!] failed.
This is the online search of the music player reached from the radio, so a song you only heard once
on air ends up in your local library (after a rescan with [SHIFT+r] in the player).


[[New Page]]
12.1 RADIO SETTINGS: COLORS, ON/OFF AND ANIMA/VIZ [[Header]]

OPEN:[[Header]] [s] opens the radio settings. Same frame, tab strip and editing keys as the player's:
[TAB] next tab, [UP]/[DOWN] rows, [<-/->] or [ENTER] change a value, [s] saves and closes,
[ESC] / [q] discards (asks first when something changed).
They are stored in radio_config.txt, separate from the config.txt of the player. Colors are not
carried over between the two modes: the radio has its own palette and its own theme choice.

COLORS (ONLY THE RADIO HAS): [[Header]]
ON_AIR            the diagonal gradient of the ON AIR sign, the needle, the lamp and LIVE
OSCI              left / right color of the oscilloscope
FREQUENCY_BAR     the line and ticks of the dial / the numbers and dots
VOLUME            current / possible part of the volume bar
PRESETS           inactive slots (FG/BG) and the shortcut letters
BORDER_COLOR, METADATA, VIZ, LIST, HEADER, LEGEND and TAB_NAMES work as in the player, and so do the
THEME row, [e], [SHIFT+d] and the unsaved-changes questions.

ON/OFF: [[Header]]
Playback (shuffle / list, like [m]), Start Mode (player / radio), On air ascii, Pulse wave, Dummy buttons,
Spectrum Bars, Stereo sound, Normalize volume, Tuning noise (static while tuning, off by default and
never recorded), Timeshift buffer (5 / 15 / 30 / 45 / 60 minutes), Use oscilloscope / Use spectrogram.

ANIMA/VIZ: [[Header]]
Gradient wave speed and Pulse wave speed (the ON AIR sign), Bars Fluidity / Degradation / Viscosity,
Osci/sphere (osci / sphere / spectro / off) and Osci style (braille / image).

[[New Page]]
12.2 RADIO SETTINGS: PATHS, KEYS AND FILES [[Header]]

PATHS: [[Header]]
Every path is a base folder; the radio makes its own sub folders below it:
STATION LISTS PATH   stations/ and station_lists/          PRESETS PATH   presets/presets.txt
DOWNLOAD PATH        radio_downloads/ (or the same folder as the music player)
HISTORY PATH         radio_history/
Changed folders are copied, never moved, and nothing is deleted. Playback stops and everything reloads.

REFERENCE: REBINDING THE KEYS OF THE RADIO: [[Header]]
The keys that are not hard coded, grouped like the player's: PLAYBACK, MENUS, TIMESHIFT and SYSTEM.
[ENTER]  on a row, then type one character (case matters) or SPACE / TAB / BACKSPACE
[DEL]    restore the default of that key
[CTRL+SHIFT+u]  undo the last five key changes
The first row, Reset all keys to default, sits under the cheat sheet note at the top.
A key that is already in use is refused, and the fixed keys count too: the 16 preset keys (1..0 and
e r t d f g) and the vi-style [j] / [k]. The cheat sheet [?] always lists the default keys.

THE FILES OF THE RADIO: [[Header]]
radio_config.txt   settings, colors, equalizer, oscilloscope (keys as HKey<Action>= lines)
stations.txt       your station list                    presets.txt       the presets and the active one
stationlists.txt   the saved station lists              preset_names.txt  the short names of the PRESETS pane
radio_history/     history_radio.txt and archive_radio.txt
All of them live in the config folder: ~/.config/mousiki/ (Windows: %USERPROFILE%\.config\mousiki\).
)WIZ",
nullptr };
