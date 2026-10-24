<div align="center">
  <h1>📻 Mousiki Plus - Music, Radio and Audio Book Player v.4.6.0 🎵</h1>
</div>

Installers and portable versions for **Windows, Linux and macOS (x64)**. Mousiki is a terminal music player, online radio and audio book player in one program; the key that types `*` switches between the three modes (`SHIFT` and `+` on a German keyboard, `SHIFT+8` on an English one; `SHIFT` and `-` goes back).

v4.6.0 adds the **audio book player**, the third mode, with bookmarks, notes and chapter cutting, and the **phone remote** learns it too. **New here? Press `SHIFT+w` for the wizard and `?` for the cheat sheet.** Changes since v4.5.0.

## What's new

- **Audio book player** (third mode; Settings → ON/OFF → MODES switches modes on and off and sets their order; Settings → PATHS → *AUDIO BOOK PATH* is the folder of your books):
  - Books (folders) on the left, chapters on the right; its own look, theme (starts with Rose Pine) and settings in `audiobook_config.txt`. The music player's library leaves the books out.
  - **Book cover** (or a default picture) in the disk's place: a real pixel image in Kitty / Sixel terminals, braille dots elsewhere.
  - **Auto-mark** (where you stopped), any number of **bookmarks** and **notes** you write while you listen (title and text, shown again when playback passes them); questions when you leave a chapter, resume choices, undo for deleted time stamps.
  - Big overlays: **library** (`SHIFT+s`), **time stamp list** (`SHIFT+k`), **notes** (`SHIFT+l`, small field ↔ big editor with `CTRL+n`), **reading history** (`SHIFT+h`), a **tag editor made for books** (`SHIFT+m`); the overlays switch to each other.
  - **Chapter marks and cutting** (`ALT+l`): set marks while you listen, cut a long file into chapters with ffmpeg (optionally trimming silence to 2 s); bookmarks, notes and auto-marks move to the new files.
  - Notes can be exported / imported; a note editor with soft wrap, marking, copy and paste (`CTRL+C` copies instead of ending Mousiki).
- **Phone remote:** the remote app follows the third mode. The tabs show `PLAYER RADIO BOOKS`; the audio book screen shows the cover or default picture at **full resolution**; **LISTS** is the library as a folder tree where a tap on a chapter plays it. New commands `book-open <n>` and `switch-mode -1`.
- **Fixed:** the music player's session no longer picks up the audio book that played last; several fixes in the audio book mode found while testing.
- **Documentation:** the manual, README (new screenshots), help wizard, cheat sheet and config templates cover all of the above. The full list of changes is in `Changes_from_the_Original.md`.

## New keys (your config is migrated automatically)

Keys you rebound yourself are kept. New keys that were not bound before are simply added.

The audio book keys (`SHIFT+b` bookmark, `SHIFT+n` note, `SHIFT+k` / `SHIFT+l` / `SHIFT+s` / `SHIFT+h` overlays, `ALT+l` chapter marks) are listed in the cheat sheet (`?`) and in chapter 12 of the manual.


The portable versions keep their settings in a `data/` folder next to the program.

## Requirements

- **Windows:** nothing to install. FFmpeg, yt-dlp, Python and SDL2 (for the scope and spectrogram windows) are included. Use Windows Terminal.
- **Linux:** the `.deb` installs FFmpeg and Python as dependencies. It recommends `libsdl2-2.0-0` for the scope and spectrogram windows. Media keys use `libdbus-1` when it is present. With the portable zip: `sudo apt install python3 libsdl2-2.0-0`.
- **macOS:** `brew install ffmpeg` is needed for radio streams and many formats. `brew install sdl2` adds the scope and spectrogram windows.
- **Optional:** `sidplayfp` on the `PATH` for C64 SID tunes. Lyrics translation and the version check need an internet connection.

## Notes

- Windows is the most tested platform; Linux was tested in WSL. The audio book mode and its phone remote are new: please report bugs in Issues or Discussions.
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
