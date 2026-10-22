<!-- Paste this section into the project's README / home page. SignPath Foundation requires it. -->
## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

* The Windows installer (`Mousiki-<version>-windows-x64-setup.exe`) and the binaries `mousiki.exe` and `fpcalc.exe`
  are built by the public GitHub Actions workflow `.github/workflows/build-installers.yml` from the source in this repository.
  Only these files are signed. Bundled third-party tools (FFmpeg, yt-dlp, Python) are unmodified upstream downloads and are not signed by this project.
* Roles: **Author / committer / reviewer / approver:** [Steffen Schwerdtfeger](https://github.com/StSchwerdtfeger)
  (add further maintainers here; every signing request is approved by an approver in SignPath).
* **Privacy policy:** Mousiki does not collect or transmit any personal data. It only contacts the services you use through it
  (e.g. YouTube via yt-dlp, lyrics and AcoustID lookups when you trigger them).
* All release packages (Windows, Linux, macOS) are built by the same workflow, carry a GitHub build-provenance attestation
  (`gh attestation verify <file> --repo <owner/repo>`) and a detached GPG signature (`<file>.asc`, plus `SHA256SUMS.txt.asc`;
  key fingerprint: `0C8F 861B 48F4 7145 EE64  30F6 DE24 4FF4 9A0F 0902`). Linux and macOS packages are not signed with a code-signing certificate.
