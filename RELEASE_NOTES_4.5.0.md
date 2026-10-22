<div align="center">
  <h1>📻 Mousiki Plus - Music and Radio Player v.4.5.0 🎵</h1>
</div>

Installers and portable versions for **Windows, Linux and macOS (x64)**. Mousiki is a terminal music player and online radio in one program; the key that types `*` switches between the two (`SHIFT` and `+` on a German keyboard, `SHIFT+8` on an English one).

v4.5.0 is about **staying in touch with your system and faster playlist and tag work**: media keys and Now Playing integration, a notice when a new version is out, a sortable and folder-searchable playlist library, a smarter queue-to-playlist step and a meta editor that moves faster. **New here? Press `SHIFT+w` for the wizard and `?` for the cheat sheet.** Changes since v4.4.0.

## What's new

- **Media keys and Now Playing** (player and radio): Linux (MPRIS, shows in the desktop's media widgets and works with `playerctl`), macOS (Now Playing) and Windows (system media transport controls). Play / pause, next, previous, seek and volume from the keyboard, headset or lock screen. Settings → ON/OFF → *Media Keys* (`MediaKeys=`).
- **New-version notice:** once a day Mousiki checks GitHub for a newer release. A small overlay shows it once per version: `ESC` closes it, `o` opens the release page. Settings → ON/OFF → *Update Notice* (`UpdateCheck=`).
- **Playlist editor:**
  - The library list sorts with `SHIFT+t`: folder order, file names, titles, artists, albums A-Z. It also sorts search results.
  - `/f:` in the search field lists the tracks of matching folders.
  - The library pane is taller, the legend has a *Sort* row.
  - `CTRL+SHIFT+U` (queue to playlist) first asks: **New playlist** or **Existing playlist** (search field and list of saved playlists). The editor opens with the tracks added; you save with `CTRL+s`.
- **Meta editor:**
  - `SHIFT+↑↓` moves a page, `CTRL+↑↓` jumps to the first / last row.
  - `ESC` first removes a missing-tag filter (`x` / `SHIFT+t` / `a` / `y` / `l`), and in the search field first clears the query; a second `ESC` leaves.
  - `/f:` searches folders here too.
  - **Tracker modules and chiptunes can be tagged:** the title of `.mod`, `.xm`, `.s3m`, `.it`, `.mptm` and the title and artist of `.sid` are written into the file; other fields go to a small side file in a `chip_tracker_meta data` folder next to the music.
  - Legends use one space between commands; labels are clearer (*Single track*).
- **Fixed:**
  - Japanese and other double-width characters (and letters with combining marks such as `ば` = は + mark) under the text cursor no longer push the pane borders out of line.
  - The test program no longer aborts at exit (CI).
- **Behind the scenes:** the vendored libraries are checked weekly, every release carries an SBOM, cppcheck / clang-tidy run without blocking, and `app.cpp` was split into smaller files.
- **Documentation:** the manual, README, help wizard and config templates cover all of the above. The full list of changes is in `Changes_from_the_Original.md`.

## New keys (your config is migrated automatically)

Keys you rebound yourself are kept. New keys that were not bound before are simply added.

| What | Key |
| -- | -- |
| Sort the playlist editor's library | `SHIFT+t` (the main list's sort key) |
| Page / first-last row in the meta editor | `SHIFT+↑↓` / `CTRL+↑↓` |
| Search folders (playlist and meta editor) | `/f:` |
| New-version overlay | `ESC` close, `o` go to release |

The portable versions keep their settings in a `data/` folder next to the program.

## Requirements

- **Windows:** nothing to install. FFmpeg, yt-dlp, Python and SDL2 (for the scope and spectrogram windows) are included. Use Windows Terminal.
- **Linux:** the `.deb` installs FFmpeg and Python as dependencies. It recommends `libsdl2-2.0-0` for the scope and spectrogram windows. Media keys use `libdbus-1` when it is present. With the portable zip: `sudo apt install python3 libsdl2-2.0-0`.
- **macOS:** `brew install ffmpeg` is needed for radio streams and many formats. `brew install sdl2` adds the scope and spectrogram windows.
- **Optional:** `sidplayfp` on the `PATH` for C64 SID tunes. Lyrics translation and the version check need an internet connection.

## Notes

- Windows is the most tested platform; Linux was tested in WSL. The macOS and Windows media-key code is new: please report bugs in Issues or Discussions.
- The files are not signed with a purchased code-signing certificate, so Windows SmartScreen and macOS Gatekeeper may warn on first start. On macOS, right-click (or two-finger click) the file and choose *Open*.
- Third-party software and its licences are listed in `THIRD_PARTY_LICENSES.txt`, which is included in every package.

## Verifying your download

Release signing key (GPG, ed25519) fingerprint: `0C8F 861B 48F4 7145 EE64  30F6 DE24 4FF4 9A0F 0902`

```bash
gpg --import mousiki-release-key.asc
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum -c --ignore-missing SHA256SUMS.txt
gh attestation verify <file> --repo StSchwerdtfeger/mousiki-plus
```

Every package also has its own `.asc` signature and a GitHub build-provenance attestation.
