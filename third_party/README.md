# third_party/ -- libraries shipped as source

Everything in this folder is copied from the upstream projects and compiled into Mousiki (nothing is downloaded when building).

| Folder / file | Library | Upstream |
|---|---|---|
| `miniaudio.h` | miniaudio (audio output and decoding) | https://github.com/mackron/miniaudio |
| `kissfft/` | KISS FFT (spectrum) | https://github.com/mborgerding/kissfft |
| `chromaprint/` (with its own `kissfft/`) | Chromaprint (AcoustID fingerprint, `fpcalc`) | https://github.com/acoustid/chromaprint |
| `miniz/` | miniz (deflate for the oscilloscope picture) | https://github.com/richgel999/miniz |
| `stb_truetype.h` | stb_truetype (karaoke letters from a font) | https://github.com/nothings/stb |

## Keeping them current -- what happens by itself

- Every Monday the workflow **vendored-watch** compares the versions here with the newest upstream releases and opens (or
  updates / closes) the issue **"Vendored libraries: updates available"**. You can also start it by hand (Actions tab).
- The **version of each library is read from its own files** (`tools/vendored_versions.py`, rules in `VENDORED.json`), so
  after replacing a library nothing has to be edited for the check or for the SBOM of the next release.
- Locally: `python tools/check_vendored.py` prints the same report.

## What to do after you updated a library (checklist)

1. Replace the library's files with the new release (keep the same file names; for `chromaprint/` read its `README.md` first --
   it lists which files are used and what was changed).
2. Build and run the tests (`cmake -DMOUSIKI_BUILD_TESTS=ON`, then `mousiki_tests`); play a file, open the spectrogram (KISS FFT),
   fetch meta data with SHIFT+b (Chromaprint), start the oscilloscope in the image style (miniz), open the karaoke overlay with
   Japanese text (stb_truetype).
3. Run `python tools/check_vendored.py --update-readme`: it rewrites the dependency table in the root `README.md` (versions read
   from the files, newest upstream looked up now, today's date). Needs the network. Also check `THIRD_PARTY_LICENSES.txt`
   (version numbers; a changed licence text if upstream changed it). The rows for SDL2, Python and the bundled tools are written
   by hand in `VENDORED.json` (`readme_extra_rows`).
4. **KISS FFT only:** its files carry no version number. Write the release you copied into `VENDORED.json`
   (`"version": "131.2.0"` in its entry) so the SBOM names it, and remove the `"unknown"`.
5. Commit with a message such as `Update miniz to 3.1.3`. The next release's SBOM lists the new version on its own.

## Adding a new vendored library

Add an entry to `VENDORED.json` (name, upstream `owner/repo`, licence, path, use) and, if its files contain a version, a
`detect` rule (file + regular expressions; see the existing entries). Then add it to the table above and to
`THIRD_PARTY_LICENSES.txt`.
