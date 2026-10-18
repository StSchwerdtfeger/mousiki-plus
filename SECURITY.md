# Security policy

Mousiki Plus is a hobby project by one person, so there is no bug bounty and no guaranteed response time, but security
reports are taken seriously and fixed as soon as possible.

## Supported versions

Only the **latest release** gets security fixes. Please update before reporting.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem.

Use GitHub's private reporting instead: open the **Security** tab of this repository, choose **Report a vulnerability**
and describe what you found, how to reproduce it (a sample file or the steps) and which version and system you used.
You will get an answer there. Once a fix is released, the report can be made public together with credit to you, if you wish.

## What matters most

Areas where a flaw would be serious:

- **Files and data from outside:** audio and tag parsing, playlists, `.lrc` lyrics, WAV files read by the Creator, downloaded
  images and the internet-radio / YouTube responses (crashes, memory errors, writing outside the cache or library).
- **The Remote app** (off by default): a small web server on your local network with a PIN. Anything that bypasses the PIN
  or the lockout, or reaches files or commands through it.
- **Starting other programs:** `ffmpeg`, `ffprobe`, `yt-dlp`, `fpcalc` and Python scripts are started with quoted arguments;
  any way to inject extra arguments or commands through a file name, a tag or a URL counts.
- **The release files:** a mismatch between a package and `SHA256SUMS.txt`, or a bundled tool that is not what the build
  script pinned.

## Verifying a download

Every release lists the SHA-256 checksum of each file in `SHA256SUMS.txt`. Compare it with the file you downloaded
(`certutil -hashfile <file> SHA256` on Windows, `sha256sum <file>` on Linux, `shasum -a 256 <file>` on macOS). The Windows
installer is not code-signed, so Windows SmartScreen will warn; the checksum is how to check it is the file that was published.

## Third-party components

The bundled tools (FFmpeg, yt-dlp, Python, SDL2) and the vendored libraries (miniaudio, miniz, KISS FFT, stb_truetype,
Chromaprint) are listed in `THIRD_PARTY_LICENSES.txt`. Their versions are fixed by hash in `packaging/pins.json` and
`packaging/pylib-requirements.txt`. A problem in one of them is best reported to that project; tell me too, so a new
version can be bundled.
