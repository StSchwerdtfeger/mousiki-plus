# Mousiki Remote (Android app)

Controls a Mousiki that runs on a computer in the same network: the spinning braille disk, the meta data, the working
dummy buttons (seek / play-pause), next / previous, random, volume, playback mode, the local list and the saved playlists (tap: play / into the queue), clear queue, the station list (tap: tune), the manual and the
switch between the music player and the radio -- in the colours of the theme Mousiki uses.

## How it works

* Mousiki has a small web server, **off by default** and built into the program (your own build works the same as an
  installed one): Settings -> ON/OFF -> MODES -> **Remote App** -> `ENTER` opens the REMOTE APP overlay -- switch it on,
  scan the QR code with the phone camera (opens the remote, logged in), or note the address and PIN for the app; port
  and PIN can be changed there. `mousiki --remote-info` prints the address and the PIN too.
* Mousiki draws the phone screen itself (`src/app_phone.cpp`, `build_phone_screen()` in `src_radio/radio_ui.cpp`) and
  sends it as lines of terminal text. The page `web/index.html` (built into Mousiki by `tools/web_to_inc.py`) paints them
  the way a terminal does -- braille as thick round dots, box lines as lines -- and sends back the command of the box
  you tap. So the app always looks like the Mousiki version it talks to.
* The Android app (`android/`) is a full-screen view of that page plus a connect screen (address, port, PIN, and a search
  for Mousiki in the network), the back button, haptic feedback and "keep the screen on".
* Without the app: open `http://<computer>:4747` in the phone's browser (same page; "Add to home screen" works too).

Long-press anywhere (or tap the title) for the remote's own settings: the 16 base colours (Windows Terminal, xterm,
GNOME -- match your terminal's scheme), the size of the braille dots, the PIN, the manual.

## Getting the APK

* **GitHub:** the workflow `.github/workflows/remote-app.yml` builds it on every change of `remote_app/android/` (or by
  hand: Actions -> "Remote app (Android)" -> Run workflow). The APK is under the run's *Artifacts* (`mousiki-remote-apk`).
* **Android Studio:** open `remote_app/android`, then Build -> Build APK(s).
* **Command line** (JDK 17 and the Android SDK installed, `ANDROID_HOME` set): `cd remote_app/android && ./gradlew assembleDebug`
  -> `app/build/outputs/apk/debug/app-debug.apk`.

Install it on the phone (allow "install unknown apps" for the file manager or browser you open it with).

## Security

The server only answers in the local network (it listens on the computer's addresses; a router does not forward it to
the internet unless you set that up). Every command needs the PIN; after 10 wrong PINs it answers nothing for a minute.
Only the fixed remote commands exist -- nothing can read or change files. Turn Remote App off when you do not use it.
