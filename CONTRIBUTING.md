# Contributing to Mousiki Plus

This page is for people who want to read or change the code. For using the program see the
[user manual](Mousiki_User_Manual/Mousiki_User_Guide.md); for building it see [Quick start](README.md#quick-start) in the README.

## Build and test in five lines

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOUSIKI_BUILD_TESTS=ON      # add -DMOUSIKI_WERROR=ON to make warnings fail
cmake --build build --target mousiki mousiki_tests --parallel 4
./build/mousiki_tests                                                        # the unit tests (seconds)
python3 -m pip install pyte                                                  # a terminal emulator for the UI tests
python3 tests/ui_smoke.py build/mousiki --only settings_general_texts        # one UI scenario; no --only runs all (minutes)
```

`ctest --test-dir build --output-on-failure` runs everything the CI runs (Linux, macOS, Windows, and Linux with ASan/UBSan).
Never start a build while you edit a header: the precompiled header (`app.h`) goes stale and the build fails with
"redefinition" errors; just build again.

## The map

| Where | What lives there |
|:---|:---|
| `src/main.cpp` | Entry point. Handles the command-line modes (`--cmd`, `--status`, `--selftest-fetch`, the extra windows) and then runs the active mode. |
| `src/` | The music player **and** the shared plumbing (settings, terminal drawing, themes, MilkDrop, video, remote, CLI ...). Everything is compiled once into the library `mousiki_core`; `mousiki` (and the tests) link it. |
| `src_radio/` | The radio mode: its own App (`radio_app_*.cpp`), its own screens (`radio_ui*.cpp`), its own settings screen (`radio_settings*.cpp`). |
| `src_audio_book/` | Audio book additions (chapters, notes, `.cue` files). The audio book player is the **same class `App`** as the music player with `audiobook_ == true`. |
| `scripts/` | Python helpers the program starts (lyrics, covers, meta data, kanji reader, name lookup). They are optional and checked by pinned hashes (`pins.json`, `kanji_pins.json`). |
| `tests/` | `test_*.cpp` unit tests (picked up by a glob, `TEST(name) { CHECK(...); }`) and `ui_smoke.py` (the real program in a pseudo terminal, read through `pyte`). |
| `tools/` | Developer scripts: `wizard_to_inc.py`, `make_manual_pdf.py`, `make_corner_font.py` (the tiny font "Mousiki Corners" for Thick Borders = thick round; writes `packaging/fonts/MousikiCorners.ttf` and `src/corner_font_data.inc`), generators for tables. |
| `packaging/` | Installers, the release toolkit (`release.py`), signing, AUR. |
| `Mousiki_User_Manual/`, `Mousiki_Help_Wizard.txt`, `config.txt` | The manual, the in-program help text and the example config (all three are tested or generated, see below). |

### Three modes, one program

The music player (0), the radio (1) and the audio book player (2) are switched with `*` / `_`
(`src/mode_switch.h`). A mode ends by returning `kExitSwitchMode`, and `main()` starts the next one. The radio is destroyed
completely on every switch; the player is only *suspended* so coming back is instant.

### The music player in one paragraph

`class App` (`src/app.h`) is the whole player UI and is split by feature into `app_*.cpp`
(`app_playback`, `app_playlist`, `app_video`, `app_milkdrop`, `app_settings*`, `app_meta*` ...). `App::run()` loops:
read keys, `run_tick()` (poll background jobs: searches, loads, waveform, gapless), build one frame, write it.
The screen is one big string per frame (`app_render.cpp`, `build_*_panel()` functions); `terminal_ui.cpp` diffs it against the
previous frame and writes only what changed, so the terminal output stays small. Pictures (Kitty / Sixel) are drawn on top
(`terminal_gfx.cpp`).

* **Which screen is open** is `enum class Mode` (`Browse`, `Search`, `Settings`, `Playlist`, `MetaEdit` ...). `handle_key()`
  in `app_keys.cpp` sends a key to the handler of the open mode (`handle_<mode>_key`); a few things (the help wizard, a
  pending confirmation) take the key first.
* **Hotkeys** are looked up by action name (`key_is("HKeyWizard", key)`), never by character, so users can rebind every key.
  The defaults are in `settings.cpp`; the cheat sheet reads the same table.
* **Colours and corners** come from the settings (`settings_.border_color`, `header_color` ...); draw with `ansi_for(...)`,
  never with fixed escape codes, so themes work.

### Audio

`player.cpp` decodes a whole track into a `StreamingPcm` buffer (a ring window for very long files) and a miniaudio callback
(`Player::data_callback_impl` -> `render()`) mixes it: volume, loudness normalisation, EQ, soft limiter, pause ramp,
sleep-timer fade, fades and crossfade (`audio_fade.h`), the audio book time stretch. The callback never allocates or locks for
long: parameters are `std::atomic`s the UI thread writes. Gapless playback preloads the next track into the same player
(`app_gapless.cpp`). For tests, `MOUSIKI_NULL_AUDIO=1` with `MOUSIKI_AUDIO_TAP=<file>` records what would reach the device.

### Other shared parts

* `remote_control.*` (`mousiki --cmd ...` through a folder), `remote_server.*` (the phone app), `cli_core.*` (the command line
  language, pure functions so they are unit tested).
* `bg_tasks.*` (detached helper threads with a cap), `par_pool.*` (worker threads for the video looks), `tag_cache.*` (tags
  between runs), `console_log.*` (the console and `--perf-log`).
* `translit*.cpp` (Latin letters for Japanese, Korean and Chinese), `lyrics_*.cpp`, `video_*.cpp`, `milkdrop*.cpp`.

## Adding or changing a setting

This is the most common change, and it touches several places. The tests tell you if one is forgotten
(`example_config_lists_every_setting_the_program_writes`, the settings round-trip tests).

1. **The value:** a field in `struct Settings` (`src/settings.h`) with its default.
2. **Load and save** (`src/settings.cpp`): one line in the `kSimpleSettings` table (a flag `SIMPLE_FLAG`, a number
   `SIMPLE_NUM`, a colour, a text ...) **and** a `write_simple_setting(out, s, "Key")` in `write_settings()` (the order there is
   the order in `config.txt`). Anything else needs its own `if (key == "...")` in the reader and an `out << ...` in the writer.
   Write the `## ...` comment line: it is shown in `config.txt` (values and the default).
3. **Part of the look?** (colours, corners, layouts that the audio book mode keeps for itself): add it to `copy_look()` and the
   prefixes of `is_look_key()`.
4. **The example config:** add the key, with its `##` line, to `config.txt` in the project root (the test fails until you do).
5. **A row in Settings:** an id in `src/app_settings_tables.h` (`kSid...`), a line in the layout of the tab
   (`app_settings_tables.cpp`; the music player and the audio book layouts are separate), and in `src/app_settings.cpp` the
   value text (`settings_get_value`), the choices (`settings_options_for`), what a change does (`settings_commit_edit` /
   `settings_cycle`) and the status line. Use an `apply_...()` function if the change must reach the player or a worker.
   Add a description or a preview in `src/settings_preview.cpp` (`pv_desc` for a text, or a preview kind).
6. **The radio** has its own Settings rows (`src_radio/radio_settings*.cpp`). The Latin / transliteration previews are shared
   (`src/translit_preview.cpp`), so change them once.
7. **Docs:** a row in the settings tables of the manual, the wizard text (`Mousiki_Help_Wizard.txt`, then
   `python3 tools/wizard_to_inc.py`; never edit `src/wizard_text.inc` by hand), and one line in the current release notes.
8. **Tests:** a round trip in `tests/test_settings.cpp`; a smoke check in `tests/ui_smoke.py` when it is visible on screen.

## Tests

* Unit tests are plain functions: `TEST(my_thing) { CHECK_EQ(a, b); }` in any `tests/test_*.cpp`. `mt::TempHome home;` gives an
  empty config folder. Test pure logic (parsers, curves, settings) there; a function that needs a terminal belongs in the smoke test.
* `tests/ui_smoke.py` runs the real program in a pseudo terminal. A scenario is a function (`def my_scenario(binary, root, scripts)`)
  registered in `run_scenario()` and `SCENARIOS`; `check(condition, "what is true")` records a result. Use `sess.send(keys, wait)`,
  `sess.text()`, `sess.screen.display`, `sess.wait_for(lambda t: ...)`. A flaky wait is a bug in the test: wait for the real row.
* Audio tests record the output (`MOUSIKI_AUDIO_TAP`) and measure it in samples (see `gapless_playback`, `audio_fades`).

## Conventions that keep the code readable

* **Comments say why**, in plain English, next to the thing they explain; long ones are fine. Keep user-visible texts short.
* **Behaviour first:** a refactor must not change what the user sees; the smoke test is the proof.
* **Costs:** nothing that allocates or takes a lock in the audio callback; nothing that scans the library on a key press (use the
  tag cache and the background sweep); large strings built once per frame, not per row.
* **Untrusted text** (tags, file names, lyrics, network replies) goes through `TextSanitizer` before it is drawn.
* **No new dependency** without a reason in the pull request; the vendored ones are listed in `THIRD_PARTY_LICENSES.txt` and
  watched by `tools/check_vendored.py`.
* **Version and docs:** the version number is changed only for a release; user-facing changes get a line in
  `RELEASE_NOTES_<version>.md` and the manual.

## Reporting problems

Open an issue with the console log (`t` opens the console by default, or the log file in the config folder) and, for slow screens, a run with
`mousiki --perf-log`. For security problems see [SECURITY.md](SECURITY.md).
