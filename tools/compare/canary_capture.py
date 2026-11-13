"""Reference images from Xenia Canary (BSD-3) for the renderer's image comparison.

Starts exactly one Canary process with the disc image, sends key presses only to its own
Canary window (window found by PID, messages via PostMessage to this HWND; with --input sendinput only
when the own window is in the foreground and WindowFromPoint of the window centre belongs to the own process)
and triggers images at fixed times:
  - "canary": Canary's own screenshot (F12, guest output at render resolution, lands in <canary>/screenshots),
  - "window": PrintWindow(PW_RENDERFULLCONTENT) of the client area.
At the end only the self-started process is terminated, by PID.

  python -I tools/compare/canary_capture.py --canary third_party/xenia-canary-9369464/xenia_canary.exe
      --iso <image> --out work/reference/menu --seconds 120
      --keys "34:start,38:start,42:a" --shots "20,40,60" [--input post|sendinput] [--shot-mode canary|window|both]
  A key with @seconds is held ("90:w@9"); while it is held no other events run.

Keys (default mapping of the Canary configuration [HID.WinKey], keyboard_mode = 1):
  start = X, a = 0xBA (;), b = 0xDE ('), back = Z, up/down/left/right = direction via left stick W/S/A/D,
  dup/ddown/dleft/dright = D-pad (Ctrl+W/S/A/D), or raw: vk:0x41.
Images are derived game data: write them only to work/, never commit them.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time
import zlib

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)

WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_CONTROL, VK_F12 = 0x11, 0x7B
PW_RENDERFULLCONTENT = 2

KEYS = {
    "start": [0x58], "a": [0xBA], "b": [0xDE], "x": [0x4C], "y": [0x50], "back": [0x5A],
    "up": [0x57], "down": [0x53], "left": [0x41], "right": [0x44],
    "dup": [VK_CONTROL, 0x57], "ddown": [VK_CONTROL, 0x53], "dleft": [VK_CONTROL, 0x41],
    "dright": [VK_CONTROL, 0x44], "f12": [VK_F12],
    # right stick (arrow keys) and triggers (E/Q) of the default mapping
    "rup": [0x26], "rdown": [0x28], "rleft": [0x25], "rright": [0x27], "rt": [0x45], "lt": [0x51],
}

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.WindowFromPoint.argtypes = [wt.POINT]
user32.WindowFromPoint.restype = wt.HWND
user32.GetAncestor.argtypes = [wt.HWND, wt.UINT]
user32.GetAncestor.restype = wt.HWND
user32.GetForegroundWindow.restype = wt.HWND
user32.GetDC.restype = wt.HDC
user32.GetDC.argtypes = [wt.HWND]
user32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
user32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
gdi32.CreateCompatibleDC.restype = wt.HDC
gdi32.CreateCompatibleDC.argtypes = [wt.HDC]
gdi32.CreateCompatibleBitmap.restype = wt.HBITMAP
gdi32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
gdi32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
gdi32.SelectObject.restype = wt.HGDIOBJ
gdi32.DeleteObject.argtypes = [wt.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wt.HDC]
gdi32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p, ctypes.c_void_p, wt.UINT]


def window_title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, buf, 512)
    return buf.value


def window_class(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def dismiss_dialogs(pid):
    """Answers message boxes of the own Canary process (notice on the first start) with No."""
    hits = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd) and window_class(hwnd) == "#32770":
            hits.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    for h in hits:
        user32.PostMessageW(h, 0x0111, 7, 0)  # WM_COMMAND IDNO
        print(f"canary: dialog '{window_title(h)}' answered with No", flush=True)
    return len(hits)


def find_window(pid):
    found = []

    def cb(hwnd, _):
        p = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd) and window_class(hwnd) != "#32770":
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            if r.right - r.left >= 320:
                found.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return found[0] if found else None


def owner_pid(hwnd):
    p = wt.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
    return p.value


def key_lparam(vk, up):
    scan = user32.MapVirtualKeyW(vk, 0) & 0xFF
    lp = 1 | (scan << 16)
    if up:
        lp |= (1 << 30) | (1 << 31)
    return lp


class INPUT(ctypes.Structure):
    class _KI(ctypes.Structure):
        _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                    ("dwExtraInfo", ctypes.c_size_t)]

    class _U(ctypes.Union):
        pass

    _fields_ = [("type", wt.DWORD), ("ki", _KI), ("pad", ctypes.c_byte * 8)]


def own_window_ready(hwnd, pid):
    fg = user32.GetForegroundWindow()
    if not fg or owner_pid(fg) != pid:
        return False
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    pt = wt.POINT((r.left + r.right) // 2, (r.top + r.bottom) // 2)
    w = user32.WindowFromPoint(pt)
    return bool(w) and owner_pid(user32.GetAncestor(w, 2)) == pid


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def bring_to_front(hwnd):
    """SetForegroundWindow for the own Canary window; under a foreground lock via AttachThreadInput to the
    input thread of the current foreground window (only for the duration of the call)."""
    if user32.SetForegroundWindow(hwnd) and user32.GetForegroundWindow() == hwnd:
        return
    fg = user32.GetForegroundWindow()
    fg_tid = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    me = kernel32.GetCurrentThreadId()
    attached = bool(fg_tid) and fg_tid != me and user32.AttachThreadInput(me, fg_tid, True)
    try:
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    finally:
        if attached:
            user32.AttachThreadInput(me, fg_tid, False)


def press(hwnd, pid, name, mode, hold=0.12):
    vks = KEYS.get(name)
    if vks is None:
        if name.startswith("vk:"):
            vks = [int(name[3:], 0)]
        else:
            raise SystemExit(f"unknown key {name}")
    if mode == "post":
        # For keyboard input Canary only checks its own focus flag (WM_SETFOCUS/WM_KILLFOCUS);
        # the message goes only to the own window, the system focus stays unchanged.
        if name != "f12":
            user32.PostMessageW(hwnd, 0x0007, 0, 0)  # WM_SETFOCUS
            time.sleep(0.03)
        for vk in vks:
            user32.PostMessageW(hwnd, WM_KEYDOWN, vk, key_lparam(vk, False))
        time.sleep(hold)
        for vk in reversed(vks):
            user32.PostMessageW(hwnd, WM_KEYUP, vk, key_lparam(vk, True))
        return True
    for attempt in range(15):
        bring_to_front(hwnd)
        time.sleep(0.05 + 0.15 * attempt)
        if own_window_ready(hwnd, pid):
            break
    else:
        print(f"  key {name} NOT sent: own window not in the foreground/under the centre", flush=True)
        return False
    for vk in vks:
        inp = INPUT(type=1)
        inp.ki.wVk = vk
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    time.sleep(hold)
    for vk in reversed(vks):
        inp = INPUT(type=1)
        inp.ki.wVk = vk
        inp.ki.dwFlags = 2
        user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    return True


def write_png(path, width, height, rgb_rows):
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def print_window(hwnd, path):
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    w, h = r.right, r.bottom
    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    ok = user32.PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT | 1)
    bi = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, bi, 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    data = buf.raw
    rows = []
    for y in range(h):
        line = data[y * w * 4:(y + 1) * w * 4]
        row = bytearray(w * 3)
        row[0::3] = line[2::4]
        row[1::3] = line[1::4]
        row[2::3] = line[0::4]
        rows.append(bytes(row))
    write_png(path, w, h, rows)
    return ok, w, h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--canary", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--keys", default="")
    ap.add_argument("--shots", default="")
    ap.add_argument("--input", choices=["post", "sendinput"], default="post")
    ap.add_argument("--shot-mode", choices=["canary", "window", "both"], default="both")
    ap.add_argument("--repeat", default="", help="key:interval:from:to, e.g. a:1.0:70:90")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    events = []
    for item in filter(None, a.keys.split(",")):
        t, k = item.split(":", 1)
        events.append((float(t), "key", k))
    for item in filter(None, a.repeat.split(",")):
        k, step, t0, t1 = item.split(":")
        t = float(t0)
        while t <= float(t1):
            events.append((t, "key", k))
            t += float(step)
    for t in filter(None, a.shots.split(",")):
        events.append((float(t), "shot", t))
    events.sort(key=lambda e: e[0])

    exe = os.path.abspath(a.canary)
    p = subprocess.Popen([exe, os.path.abspath(a.iso)], cwd=os.path.dirname(exe))
    print(f"canary: PID {p.pid}", flush=True)
    t0 = time.time()
    hwnd = None
    try:
        for t, kind, arg in events + [(a.seconds, "end", "")]:
            while time.time() - t0 < t:
                if p.poll() is not None:
                    print(f"canary: process exited after {time.time() - t0:.1f} s, code {p.returncode}")
                    return 1
                if time.time() - t0 < 15 and int((time.time() - t0) * 50) % 25 == 0:
                    dismiss_dialogs(p.pid)
                time.sleep(0.02)
            if hwnd is None or not user32.IsWindow(hwnd):
                hwnd = find_window(p.pid)
                if hwnd:
                    print(f"canary: window {hwnd:#x} '{window_title(hwnd)}'", flush=True)
            if kind == "end":
                break
            if hwnd is None:
                print(f"  {t:7.2f} s: no window, {kind} {arg} skipped", flush=True)
                continue
            if kind == "key":
                name, _, hold = arg.partition("@")  # "w@9": hold the key for 9 s
                ok = press(hwnd, p.pid, name, a.input, hold=float(hold) if hold else 0.12)
                print(f"  {time.time() - t0:7.2f} s: key {arg} {'sent' if ok else 'dropped'}", flush=True)
            else:
                name = f"t{float(arg):06.1f}"
                if a.shot_mode in ("window", "both"):
                    ok, w, h = print_window(hwnd, os.path.join(a.out, name + "_win.png"))
                    print(f"  {time.time() - t0:7.2f} s: window image {name} {w}x{h} PrintWindow={ok} "
                          f"title '{window_title(hwnd)}'", flush=True)
                if a.shot_mode in ("canary", "both"):
                    press(hwnd, p.pid, "f12", "post", hold=0.05)  # the Canary hotkey also works via PostMessage
    finally:
        if p.poll() is None:
            p.kill()
            p.wait(10)
            print(f"canary: PID {p.pid} terminated after {time.time() - t0:.1f} s", flush=True)
        # Move Canary's own screenshots (timestamp names) to --out in capture order.
        sdir = os.path.join(os.path.dirname(exe), "screenshots")
        shots = [e for e in events if e[1] == "shot"]
        found = []
        for dp, _, files in os.walk(sdir):
            for f in files:
                fp = os.path.join(dp, f)
                if f.endswith(".png") and os.path.getmtime(fp) >= t0 - 1:
                    found.append(fp)
        found.sort(key=os.path.getmtime)
        for i, fp in enumerate(found):
            name = f"t{float(shots[i][2]):06.1f}_canary.png" if i < len(shots) else os.path.basename(fp)
            os.replace(fp, os.path.join(a.out, name))
        if found:
            print(f"canary: {len(found)} Canary screenshots to {a.out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
