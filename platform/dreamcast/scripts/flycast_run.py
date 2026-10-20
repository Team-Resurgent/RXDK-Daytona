#!/usr/bin/env python3
"""Run a Dreamcast program in Flycast and collect what it prints (Windows).

The program's debug output goes to Flycast's serial console, a console
window of its own that stdout redirection does not capture; its text is read
from the console buffer instead (AttachConsole, in a detached helper process
so the caller keeps its own console). Flycast runs from a portable copy in
the build directory with its own emu.cfg (serial console on, crash-log
upload off), so the user's Flycast settings are never changed.

  flycast_run.py FLYCAST.EXE PROGRAM.elf WORKDIR [--until TEXT ...] [--timeout S]
"""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

DEFAULT_FLYCAST = Path.home() / "Downloads" / "flycast-win64-2.7" / "flycast.exe"


def portable(flycast, workdir):
    """A copy of Flycast in workdir, with an emu.cfg that logs the serial port."""
    workdir.mkdir(parents=True, exist_ok=True)
    exe = workdir / "flycast.exe"
    if not exe.is_file() or exe.stat().st_mtime < flycast.stat().st_mtime:
        shutil.copy2(flycast, exe)
    cfg = workdir / "emu.cfg"
    if not cfg.is_file() and (flycast.parent / "emu.cfg").is_file():
        shutil.copy2(flycast.parent / "emu.cfg", cfg)  # e.g. the BIOS path
    lines = cfg.read_text().splitlines() if cfg.is_file() else ["[config]"]
    want = {"Debug.SerialConsoleEnabled": "yes", "UploadCrashLogs": "no"}
    out = []
    for line in lines:
        key = line.split("=")[0].strip()
        if key in want:
            line = f"{key} = {want.pop(key)}"
        out.append(line)
    if want:
        i = next((n + 1 for n, l in enumerate(out) if l.strip() == "[config]"), len(out))
        out[i:i] = [f"{k} = {v}" for k, v in want.items()]
    cfg.write_text("\n".join(out) + "\n")
    return exe


class COORD(ctypes.Structure):
    _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [("Left", ctypes.c_short), ("Top", ctypes.c_short), ("Right", ctypes.c_short), ("Bottom", ctypes.c_short)]


class CSBI(ctypes.Structure):
    _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD), ("wAttributes", ctypes.c_ushort),
                ("srWindow", SMALL_RECT), ("dwMaximumWindowSize", COORD)]


def read_console(pid):
    """The text in process pid's console (run in a process without a console)."""
    k = ctypes.windll.kernel32
    k.FreeConsole()
    if not k.AttachConsole(pid):
        return ""
    try:
        k.CreateFileW.restype = ctypes.c_void_p
        h = k.CreateFileW("CONOUT$", 0xC0000000, 3, None, 3, 0, None)
        if h in (None, ctypes.c_void_p(-1).value):
            return ""
        info = CSBI()
        k.GetConsoleScreenBufferInfo(ctypes.c_void_p(h), ctypes.byref(info))
        width, rows = info.dwSize.X, info.dwCursorPosition.Y + 1
        buf = ctypes.create_unicode_buffer(width)
        n = ctypes.c_ulong()
        text = []
        for y in range(rows):
            k.ReadConsoleOutputCharacterW(ctypes.c_void_p(h), buf, width, COORD(0, y), ctypes.byref(n))
            text.append(buf.value[: n.value].rstrip())
        k.CloseHandle(ctypes.c_void_p(h))
        return "\n".join(text)
    finally:
        k.FreeConsole()


def console_text(pid):
    DETACHED_PROCESS = 0x00000008
    r = subprocess.run([sys.executable, __file__, "--read-console", str(pid)], capture_output=True, text=True,
                       creationflags=DETACHED_PROCESS)
    return r.stdout


def run(flycast, program, workdir, until=(), timeout=30.0):
    """Run program in Flycast until one of `until` is printed or timeout
    seconds pass; returns (text, the marker seen or None)."""
    exe = portable(Path(flycast), Path(workdir))
    close_all(exe)  # anything left from an earlier run
    # Shown without taking the focus (the user may be working). Not
    # minimised: Direct3D then renders at the minimised window's size, and
    # the picture is a blur once the window is restored.
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 4  # SW_SHOWNOACTIVATE
    p = subprocess.Popen([str(exe), str(Path(program).resolve())], cwd=workdir, startupinfo=info)
    text, seen = "", None
    log = Path(workdir) / "serial.txt"  # what was read, kept if Flycast dies with it
    history = []
    try:
        end = time.monotonic() + timeout
        while time.monotonic() < end and p.poll() is None:
            time.sleep(0.5)
            now = console_text(p.pid)
            if now and now != text:
                # The console can be wiped (KOS's abort); keep what came
                # before it as well.
                if text and not now.startswith(text[:200]):
                    history.append(text)
                text = now
                log.write_text("\n".join(history + [text]), encoding="utf-8")
            seen = next((u for u in until if u in text), None)
            # KOS stopping the system ends the run too (not a pass).
            if seen or any(m in text for m in ("arch: aborting", "ASSERTION FAILURE", "Out of memory")):
                break
    finally:
        p.kill()
        p.wait()
        close_all(exe)
    return text, seen


def close_all(exe):
    """Close every Flycast running from this portable copy (matched by path;
    the user's own Flycast is not touched). After a crash its windows can
    outlive the process this script started."""
    path = str(Path(exe).resolve()).replace("'", "''")
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process -Filter \"Name='flycast.exe'\" | "
                    f"Where-Object {{ $_.ExecutablePath -eq '{path}' }} | "
                    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"],
                   capture_output=True)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--read-console":
        sys.stdout.write(read_console(int(sys.argv[2])))
        return
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("flycast")
    ap.add_argument("program")
    ap.add_argument("workdir")
    ap.add_argument("--until", action="append", default=[])
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()
    text, seen = run(args.flycast, args.program, args.workdir, args.until, args.timeout)
    print(text)
    sys.exit(0 if seen or not args.until else 1)


if __name__ == "__main__":
    main()
