"""One MusicBrainz request per second, for every Mousiki process together.

MusicBrainz allows one request per second per client. The cover, tag and artist-name helpers each run as their own process
(and can run at the same time), so each waiting for itself is not enough. wait() takes a lock on a small file in the
temporary folder, sleeps until GAP seconds have passed since the last request of ANY process, writes the new time and lets
go. Processes queue up on the lock. If the file or the lock is not usable, it falls back to waiting inside this process only.
Standard library only.
"""
import os
import tempfile
import time

GAP = 1.1   # seconds between two requests; the limit is 1 per second, the rest is a margin for clock and network jitter
_own_last = [0.0]


def _path():
    try:
        who = str(os.getuid())
    except AttributeError:   # Windows
        who = os.environ.get("USERNAME", "user")
    safe = "".join(c if c.isalnum() else "_" for c in who)
    return os.path.join(tempfile.gettempdir(), "mousiki-musicbrainz-%s.stamp" % safe)


def _lock(f):
    try:
        import fcntl
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
    except ImportError:
        import msvcrt
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)


def _unlock(f):
    try:
        import fcntl
        fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    except ImportError:
        import msvcrt
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)


def _own_wait(gap):
    wait = gap - (time.time() - _own_last[0])
    if wait > 0:
        time.sleep(min(wait, gap))
    _own_last[0] = time.time()


def wait(gap=GAP, path=None):
    """Block until a MusicBrainz request may be sent now, and count it as sent."""
    try:
        with open(path or _path(), "a+b") as f:
            _lock(f)
            try:
                f.seek(0)
                try:
                    last = float(f.read(64).decode("ascii", "replace").strip() or 0)
                except ValueError:
                    last = 0.0
                now = time.time()
                if last > now + 2:  # the clock went back (or the file is from the future): don't wait for it
                    last = now - gap
                remaining = gap - (now - last)
                if remaining > 0:
                    time.sleep(min(remaining, gap))
                f.seek(0)
                f.truncate()
                f.write(("%.3f" % time.time()).encode("ascii"))
                f.flush()
            finally:
                _unlock(f)
        _own_last[0] = time.time()
    except (OSError, ImportError, AttributeError):
        _own_wait(gap)
