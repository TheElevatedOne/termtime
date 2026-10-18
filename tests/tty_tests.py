"""Interactive tests for termtime, run in a pseudo terminal.

Usage: python3 tests/tty_tests.py ./termtime
"""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./termtime")
checks = 0
failures = 0


def check(cond, msg):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", msg, file=sys.stderr)


class Term:
    def __init__(self, config, rows=30, cols=100, job_control=False, env=None):
        self.pid, self.fd = pty.fork()
        self.reaper = None
        if self.pid == 0:
            # A 256 color UTF-8 terminal unless a test says otherwise
            os.environ.pop("COLORTERM", None)
            os.environ.update({"TERM": "xterm-256color", "LC_ALL": "C.UTF-8"})
            os.environ.update(env or {})
            if job_control:
                self._run_like_a_shell(config)
            os.execv(BIN, [BIN, "-c", config])
        self.resize(rows, cols, signal_child=False)
        self.out = b""
        if job_control:
            # The shell stand-in prints the real pid first
            self.reaper = self.pid
            while b"\n" not in self.out:
                self.pump(0.05)
            line, self.out = self.out.split(b"\n", 1)
            self.pid = int(line.strip().split(b":")[1])

    @staticmethod
    def _run_like_a_shell(config):
        """Runs termtime as a foreground job, like an interactive shell
        does, so its process group is not orphaned and SIGTSTP stops it."""
        child = os.fork()
        if child == 0:
            os.setpgid(0, 0)
            os.execv(BIN, [BIN, "-c", config])
        os.setpgid(child, child)
        signal.signal(signal.SIGTTOU, signal.SIG_IGN)
        os.tcsetpgrp(0, child)
        os.write(1, b"PID:%d\n" % child)
        while True:
            pid, status = os.waitpid(child, os.WUNTRACED)
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                os._exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1)

    def state(self):
        with open(f"/proc/{self.pid}/stat") as f:
            return f.read().rsplit(")", 1)[1].split()[0]

    def resize(self, rows, cols, signal_child=True):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        if signal_child:
            os.kill(self.pid, signal.SIGWINCH)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self.out += data

    def take(self):
        out, self.out = self.out, b""
        return out

    def send(self, data):
        os.write(self.fd, data)

    def wait(self, timeout=2.0):
        target = self.reaper or self.pid
        end = time.time() + timeout
        while time.time() < end:
            pid, status = os.waitpid(target, os.WNOHANG)
            if pid:
                self.pump(0.05)
                os.close(self.fd)
                return status
            self.pump(0.05)
        os.kill(self.pid, signal.SIGKILL)
        if self.reaper:
            os.kill(self.reaper, signal.SIGKILL)
        os.waitpid(target, 0)
        os.close(self.fd)
        return None


FRAME_START = b"\x1b[?2026h"


def frames(out):
    return out.count(FRAME_START)


def last_frame(out):
    return out[out.rfind(FRAME_START):]


def drawn_rows(frame):
    """Terminal rows (1-based) that got content after being cleared."""
    parts = re.split(rb"\x1b\[(\d+);1H\x1b\[2K", frame.replace(b"\x1b[?2026l", b""))
    # parts = [prefix, row, content, row, content, ...]
    return [int(parts[i]) for i in range(1, len(parts), 2) if parts[i + 1]]


def test_clock(config):
    t = Term(config)
    t.pump(2.6)
    out = t.take()
    check(b"\x1b[?1049h" in out, "enters the alternate screen")
    check(b"\x1b[?25l" in out, "hides the cursor")
    n = frames(out)
    check(2 <= n <= 4, f"redraws once per second (got {n} frames in 2.6s)")

    # line 1 "%H:%M:%S" at scale 2 is 8*14-2 = 110 cols > 100, so it is
    # shrunk to scale 1 (55 cols): 5 + 1 + 5 rows in 30 -> first row is 10
    rows = drawn_rows(last_frame(out))
    check(rows[:1] == [10] and len(rows) == 10, f"shrunk and vertically centered (rows {rows})")
    # line 1: (100-55)//2 = 22 -> column 23
    # line 2 "%Y-%m-%d" scale 1: 10*7-1 = 69 cols -> (100-69)//2 = 15 -> column 16
    check(re.search(rb"\x1b\[\d+;23H", last_frame(out)), "line 1 horizontally centered")
    check(re.search(rb"\x1b\[\d+;16H", last_frame(out)), "line 2 horizontally centered")

    t.resize(40, 140)
    t.pump(0.3)
    out = t.take()
    check(frames(out) >= 1, "redraws on resize")
    # 110 cols fit into 140 again: 10 + 1 + 5 rows in 40 -> first row is 13
    rows = drawn_rows(last_frame(out))
    check(rows[:1] == [13] and len(rows) == 15, f"back to scale 2 after resize (rows {rows})")
    # (140-110)//2 = 15 -> column 16, (140-69)//2 = 35 -> column 36
    check(re.search(rb"\x1b\[\d+;16H", out) and re.search(rb"\x1b\[\d+;36H", out), "recentered horizontally")

    t.send(b"q")
    status = t.wait()
    out = t.take()
    check(status is not None and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          f"q quits cleanly (status {status})")
    check(out.endswith(b"\x1b[?1049l"), "restores the terminal on exit")


def test_reload_and_signals(tmp):
    config = os.path.join(tmp, "reload.toml")
    with open(config, "w") as f:
        f.write('[line1]\nformat = "A"\n[line2]\nenabled = false\n')
    t = Term(config, rows=20, cols=40, job_control=True)
    t.pump(0.4)
    t.take()

    with open(config, "w") as f:
        f.write('[line1]\nformat = "B"\ncolor = 196\n[line2]\nenabled = false\n')
    t.send(b"r")
    t.pump(0.4)
    out = t.take()
    check(b"\x1b[38;5;196m" in out, "r reloads the config")

    # A broken config on reload keeps the old one running
    with open(config, "w") as f:
        f.write('[line1]\nformat = "unterminated\n')
    os.kill(t.pid, signal.SIGUSR1)
    t.pump(0.4)
    check(t.state() in "RS", "survives a broken config on reload")
    out = t.take()
    check(b"\x1b[38;5;196m" in out, "keeps the previous config after a failed reload")

    # Ctrl-Z gives the terminal back, fg takes it again
    os.kill(t.pid, signal.SIGTSTP)
    t.pump(0.3)
    out = t.take()
    check(out.endswith(b"\x1b[?1049l"), "leaves the alternate screen when suspended")
    check(t.state() == "T", f"stops on SIGTSTP (state {t.state()})")
    os.kill(t.pid, signal.SIGCONT)
    t.pump(0.4)
    out = t.take()
    check(b"\x1b[?1049h" in out and frames(out) >= 1, "redraws after SIGCONT")

    os.kill(t.pid, signal.SIGTERM)
    status = t.wait()
    out = t.take()
    check(status is not None and os.WIFEXITED(status), f"SIGTERM exits cleanly (status {status})")
    check(out.endswith(b"\x1b[?1049l"), "restores the terminal on SIGTERM")


def test_orphaned_suspend(config):
    # Without job control the stop is discarded, the clock must keep going
    t = Term(config, rows=20, cols=60)
    t.pump(0.3)
    t.take()
    os.kill(t.pid, signal.SIGTSTP)
    t.pump(1.2)
    out = t.take()
    check(out.rfind(b"\x1b[?1049h") > out.rfind(b"\x1b[?1049l") >= 0,
          "takes the terminal back when the stop is discarded")
    check(frames(out) >= 1, "keeps drawing when the stop is discarded")
    t.send(b"q")
    check(t.wait() is not None, "quits after a discarded stop")


def test_linux_console(config):
    # The Linux console: 16 colors, no alternate screen, maybe no UTF-8
    t = Term(config, rows=25, cols=80, env={"TERM": "linux", "LC_ALL": "C"})
    t.pump(0.4)
    out = t.take()
    frame = last_frame(out)
    check(b"\x1b[38;5;" not in frame, "linux console: no 256 color escapes")
    check(b"\x1b[7m" in frame and "█".encode() not in frame,
          "linux console without UTF-8: reverse video blocks")
    t.send(b"q")
    check(t.wait() is not None, "linux console: q quits")
    out = t.take()
    # Without an alternate screen the clock has to be wiped on exit
    check(b"\x1b[H\x1b[2J" in out and out.endswith(b"\x1b[?1049l"),
          "linux console: screen cleared on exit")


def test_small_terminal(config):
    t = Term(config, rows=6, cols=20)
    t.pump(0.4)
    out = t.take()
    rows = drawn_rows(last_frame(out))
    check(rows and max(rows) <= 6, f"never draws below the screen (rows {rows})")
    # Arrow keys, wheel scrolling (sent as arrows on the alternate
    # screen), Home/F1 and Alt+x are escape sequences, not Esc
    for seq in (b"\x1b[A", b"\x1b[B\x1b[B", b"\x1b[1;5C", b"\x1bOP", b"\x1b[H", b"\x1bx"):
        t.send(seq)
        t.pump(0.15)
    check(os.waitpid(t.pid, os.WNOHANG) == (0, 0), "escape sequences do not quit")
    t.send(b"\x1b")
    check(t.wait() is not None, "Esc quits")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        config = os.path.join(tmp, "clock.toml")
        with open(config, "w") as f:
            f.write('[general]\nspacing = 1\n'
                    '[line1]\nformat = "%H:%M:%S"\nscale = 2\n'
                    '[line2]\nformat = "%Y-%m-%d"\nscale = 1\n')
        test_clock(config)
        test_reload_and_signals(tmp)
        test_orphaned_suspend(config)
        test_linux_console(config)
        test_small_terminal(config)

    print(f"tty tests: {checks} checks, {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
