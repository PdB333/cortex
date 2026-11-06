#!/usr/bin/env python3
"""Desktop acceptance: the shipped cortex.exe on a real Windows desktop.

What the headless GUI suites cannot see: a real Win32 window and Direct3D
device, real keyboard input, the system's file dialog, the shell opening a
guide, and an MCP client next to the desktop. The script drives the window
with SendInput, as a person would, and checks what it can from outside:

  1. icon: cortex.exe carries an icon resource and its window has one (what
     Explorer and the taskbar show);
  2. start page: the window opens; Ctrl+O shows the system file dialog, the
     chosen file is inspected (it joins the recent files in the settings);
  3. help: the palette's "Help: Getting started" opens docs/getting-started.md
     with the program associated with .md (Notepad here);
  4. attach and scan: the palette opens the process picker, typing a name and
     Enter attaches, typing a value and Enter runs a first scan;
  5. MCP: a client next to the desktop, read-only then with --allow-writes:
     a write is refused, then applied;
  6. the window closes cleanly, with no crash report.

Screenshots are saved to --out and, since CI artifacts are not always
reachable, also printed to the log as base64 PNG between
"==SCREENSHOT <name> <n>==" and "==END SCREENSHOT==" lines. Steps 4 and the
desktop's AI status are confirmed by reading those screenshots: the window
draws with Dear ImGui, which exposes no accessibility tree.

    python tests/desktop_acceptance.py --bundle stage/Cortex --out build/desktop-acceptance
"""

import argparse
import base64
import ctypes
import ctypes.wintypes as W
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import winreg

from PIL import Image, ImageGrab

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from context_isolation_e2e import Mcp, start_target, wait_for_runtime  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32")
shell32 = ctypes.WinDLL("shell32")
kernel32 = ctypes.WinDLL("kernel32")

user32.GetClassLongPtrW.restype = ctypes.c_size_t
user32.GetClassLongPtrW.argtypes = [W.HWND, ctypes.c_int]
user32.SendMessageW.restype = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
user32.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
user32.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, ctypes.c_void_p]
user32.GetParent.restype = W.HWND
user32.GetDlgItem.restype = W.HWND

failures = []
results = []


def check(condition, message, detail=None):
    results.append({"check": message, "ok": bool(condition), "detail": detail})
    print(("ok    " if condition else "FAIL  ") + message, flush=True)
    if not condition:
        if detail is not None:
            print("      " + json.dumps(detail, default=str)[:700], flush=True)
        failures.append(message)
    return condition


# ------------------------------------------------------------------ windows

EnumWindowsProc = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


def windows(pid=None):
    found = []

    def visit(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        owner = W.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if pid is not None and owner.value != pid:
            return True
        title = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, title, 512)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        found.append((hwnd, title.value, cls.value, owner.value))
        return True

    user32.EnumWindows(EnumWindowsProc(visit), 0)
    return found


def wait_for(predicate, timeout, step=0.25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(step)
    return None


def rect(hwnd):
    r = W.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def foreground(hwnd):
    # A process may only take the foreground under conditions; sharing the
    # input state of the current foreground thread satisfies them. (A
    # synthetic Alt press would too, but it opens the window's menu bar.)
    current = user32.GetForegroundWindow()
    mine = kernel32.GetCurrentThreadId()
    theirs = user32.GetWindowThreadProcessId(current, None) if current else 0
    if theirs and theirs != mine:
        user32.AttachThreadInput(mine, theirs, True)
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    if theirs and theirs != mine:
        user32.AttachThreadInput(mine, theirs, False)
    time.sleep(0.4)
    return user32.GetForegroundWindow() == hwnd


EnumChildProc = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


def file_name_box(dialog):
    """The File name edit box of a common Open dialog."""
    found = []

    def visit(hwnd, _):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        parent = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(user32.GetParent(hwnd), parent, 64)
        if cls.value == "Edit" and parent.value == "ComboBox":
            found.append(hwnd)
        return True

    user32.EnumChildWindows(dialog, EnumChildProc(visit), 0)
    return found[0] if found else None


# ------------------------------------------------------------------ input

class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", W.WORD), ("wScan", W.WORD), ("dwFlags", W.DWORD), ("time", W.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", W.LONG), ("dy", W.LONG), ("mouseData", W.DWORD), ("dwFlags", W.DWORD),
                ("time", W.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class _INPUTUNION(ctypes.Union):
    _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT), ("pad", ctypes.c_byte * 32)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", W.DWORD), ("u", _INPUTUNION)]


KEYEVENTF_KEYUP = 0x2
KEYEVENTF_UNICODE = 0x4


def send(*inputs):
    array = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(inputs), array, ctypes.sizeof(INPUT))


def key(vk, up=False):
    item = INPUT(type=1)
    item.u.ki = KEYBDINPUT(wVk=vk, wScan=0, dwFlags=KEYEVENTF_KEYUP if up else 0, time=0, dwExtraInfo=0)
    send(item)
    time.sleep(0.03)


def chord(*vks):
    for vk in vks:
        key(vk)
    for vk in reversed(vks):
        key(vk, up=True)
    time.sleep(0.3)


def type_text(text):
    for ch in text:
        down = INPUT(type=1)
        down.u.ki = KEYBDINPUT(wVk=0, wScan=ord(ch), dwFlags=KEYEVENTF_UNICODE, time=0, dwExtraInfo=0)
        up = INPUT(type=1)
        up.u.ki = KEYBDINPUT(wVk=0, wScan=ord(ch), dwFlags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, time=0,
                             dwExtraInfo=0)
        send(down, up)
        time.sleep(0.02)
    time.sleep(0.2)


VK_CONTROL, VK_SHIFT, VK_RETURN, VK_ESCAPE = 0x11, 0x10, 0x0D, 0x1B


def palette(command):
    """The command palette: Ctrl+Shift+P, the command's name, Enter."""
    chord(VK_CONTROL, VK_SHIFT, ord("P"))
    time.sleep(0.5)
    type_text(command)
    time.sleep(0.3)
    chord(VK_RETURN)
    time.sleep(0.8)


# ------------------------------------------------------------------ screenshots

def screenshot(out, name, box=None):
    image = ImageGrab.grab(bbox=box, all_screens=True)
    path = os.path.join(out, name + ".png")
    image.save(path)
    # The log copy is small: at most 1100 px wide and a 64-colour palette.
    small = image.convert("RGB")
    if small.width > 1100:
        small = small.resize((1100, int(small.height * 1100 / small.width)))
    small = small.quantize(colors=64)
    buffer = io.BytesIO()
    small.save(buffer, format="PNG", optimize=True)
    data = base64.b64encode(buffer.getvalue()).decode()
    chunks = [data[i:i + 4000] for i in range(0, len(data), 4000)]
    print("==SCREENSHOT %s %d==" % (name, len(chunks)), flush=True)
    for chunk in chunks:
        print(chunk, flush=True)
    print("==END SCREENSHOT==", flush=True)
    return path


def icon_image(hicon, size=64):
    """Renders an HICON into a PIL image (what Explorer draws)."""
    screen = user32.GetDC(None)
    dc = gdi32.CreateCompatibleDC(screen)
    bitmap = gdi32.CreateCompatibleBitmap(screen, size, size)
    old = gdi32.SelectObject(dc, bitmap)
    brush = gdi32.CreateSolidBrush(0x00FFFFFF)
    r = W.RECT(0, 0, size, size)
    user32.FillRect(dc, ctypes.byref(r), brush)
    user32.DrawIconEx(dc, 0, 0, W.HANDLE(hicon), size, size, 0, None, 3)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", W.DWORD), ("biWidth", W.LONG), ("biHeight", W.LONG), ("biPlanes", W.WORD),
                    ("biBitCount", W.WORD), ("biCompression", W.DWORD), ("biSizeImage", W.DWORD),
                    ("biXPelsPerMeter", W.LONG), ("biYPelsPerMeter", W.LONG), ("biClrUsed", W.DWORD),
                    ("biClrImportant", W.DWORD)]

    header = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), size, -size, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = ctypes.create_string_buffer(size * size * 4)
    gdi32.SelectObject(dc, old)
    gdi32.GetDIBits(dc, bitmap, 0, size, pixels, ctypes.byref(header), 0)
    gdi32.DeleteObject(brush)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(dc)
    user32.ReleaseDC(None, screen)
    return Image.frombuffer("RGBA", (size, size), pixels.raw, "raw", "BGRA", 0, 1).convert("RGB")


def distinct_colours(image):
    return len(set(image.getdata()))


# ------------------------------------------------------------------ steps

def check_icon_resource(exe, out):
    large = W.HICON()
    count = shell32.ExtractIconExW(exe, 0, ctypes.byref(large), None, 1)
    if not check(count >= 1 and large.value, "cortex.exe carries an icon resource (Explorer)"):
        return
    image = icon_image(large.value)
    image.save(os.path.join(out, "icon-resource.png"))
    # A real icon has many colours; a blank or the generic one has few.
    check(distinct_colours(image) > 20, "the icon resource is the Cortex mark, not blank (%d colours)"
          % distinct_colours(image))
    user32.DestroyIcon(large)


def register_md_viewer():
    """.md opens in Notepad for this user, so the guide link can be observed."""
    root = r"Software\Classes"
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, root + r"\.md") as k:
        winreg.SetValueEx(k, "", 0, winreg.REG_SZ, "CortexAcceptance.md")
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, root + r"\CortexAcceptance.md\shell\open\command") as k:
        notepad = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "notepad.exe")
        winreg.SetValueEx(k, "", 0, winreg.REG_SZ, '"%s" "%%1"' % notepad)
    shell32.SHChangeNotify(0x08000000, 0, None, None)


def recent_files(settings_path):
    try:
        with open(settings_path, encoding="utf-8") as handle:
            return json.load(handle).get("recentFiles", [])
    except (OSError, ValueError):
        return []


def read_hash(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return None


def error_code(content):
    text = json.dumps(content)
    for code in ("mutation_permission_required", "write_authority_required", "writes_disabled",
                 "read_only", "permission_denied"):
        if code in text:
            return code
    if isinstance(content, dict):
        inner = content.get("result", content)
        if isinstance(inner, dict) and inner.get("ok") is False:
            return inner.get("error") or "error"
        if content.get("_rpc_error") or content.get("_transport"):
            return "transport"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True, help="the portable folder with cortex.exe, runtime, e2e, docs")
    parser.add_argument("--fixture", default=os.path.join("tests", "fixtures", "electron-app.asar"))
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    bundle = os.path.abspath(args.bundle)
    exe = os.path.join(bundle, "cortex.exe")
    target_exe = os.path.join(bundle, "e2e", "cortex_test_target_x64.exe")
    fixture = os.path.abspath(args.fixture)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    workdir = tempfile.mkdtemp(prefix="cortex-desktop-")
    appdata = os.path.join(workdir, "appdata")
    os.makedirs(appdata)
    settings_path = os.path.join(appdata, "Cortex", "cortex-ui-settings.json")
    env = dict(os.environ, LOCALAPPDATA=appdata)
    width, height = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    print("screen %dx%d" % (width, height), flush=True)

    check_icon_resource(exe, out)
    register_md_viewer()

    desktop = subprocess.Popen([exe], cwd=bundle, env=env)
    target = None
    notepads = []
    try:
        found = wait_for(lambda: [w for w in windows(desktop.pid) if w[2] == "CortexWindow"], 30)
        if not check(found, "the desktop window opens (Direct3D 11, real window)"):
            screenshot(out, "no-window")
            raise RuntimeError("the desktop window never appeared")
        hwnd = found[0][0]
        user32.ShowWindow(hwnd, 9)
        user32.SetWindowPos(hwnd, None, 0, 0, min(width, 1440), min(height - 40, 900), 0x0004)
        time.sleep(1.5)
        check(foreground(hwnd), "the window takes the foreground")
        big = user32.SendMessageW(hwnd, 0x007F, 1, 0) or user32.GetClassLongPtrW(hwnd, -14)
        small = user32.SendMessageW(hwnd, 0x007F, 0, 0) or user32.GetClassLongPtrW(hwnd, -34)
        check(big and small, "the window has large and small icons (taskbar, Alt+Tab, title bar)")
        if big:
            image = icon_image(big, 32)
            image.save(os.path.join(out, "icon-window.png"))
            check(distinct_colours(image) > 10, "the window icon is drawn (%d colours)" % distinct_colours(image))
        screenshot(out, "01-home", rect(hwnd))
        screenshot(out, "00-desktop")

        # 2. Ctrl+O, the system file dialog, the file inspected.
        chord(VK_CONTROL, ord("O"))
        dialog = wait_for(lambda: [w for w in windows(desktop.pid) if w[2] == "#32770"], 15)
        if check(dialog, "Ctrl+O shows the system file dialog"):
            time.sleep(2.0)  # the dialog fills its views before it takes input
            screenshot(out, "02-file-dialog")
            box = wait_for(lambda: file_name_box(dialog[0][0]), 10)
            check(box, "the dialog has its File name box")
            if box:
                user32.SendMessageW(box, 0x000C, 0, ctypes.c_wchar_p(fixture))  # WM_SETTEXT
                time.sleep(0.3)
                user32.SendMessageW(user32.GetDlgItem(dialog[0][0], 1), 0x00F5, 0, None)  # Open: BM_CLICK
            check(wait_for(lambda: any(os.path.normcase(p) == os.path.normcase(fixture)
                                       for p in recent_files(settings_path)), 20),
                  "the chosen file is inspected and joins the recent files", recent_files(settings_path))
            time.sleep(1.0)
            screenshot(out, "03-files-layout", rect(hwnd))
        # A dialog left open (a failed step) would block every next step.
        for w in [w for w in windows(desktop.pid) if w[2] == "#32770"]:
            user32.PostMessageW(w[0], 0x0010, 0, 0)
        time.sleep(0.5)

        # 3. Help: the guide opens with the associated program.
        foreground(hwnd)
        palette("Help: Getting started")
        guide = wait_for(lambda: [w for w in windows() if "getting-started" in w[1].lower()], 20)
        if check(guide, "Help > Getting started opens docs/getting-started.md (in Notepad)",
                 [w[1] for w in windows()][:30]):
            notepads = guide
            screenshot(out, "04-guide")
            for w in guide:
                user32.PostMessageW(w[0], 0x0010, 0, 0)
            time.sleep(0.5)

        # 4. Attach from the palette and the picker, then a first scan.
        target, manifest = start_target([], target_exe, workdir, "desktop")
        time.sleep(1.0)
        foreground(hwnd)
        palette("Target: Select process")
        time.sleep(1.0)
        type_text("cortex_test_target_x64")
        time.sleep(0.5)
        screenshot(out, "05-picker", rect(hwnd))
        chord(VK_RETURN)
        time.sleep(3.0)
        screenshot(out, "06-attached", rect(hwnd))
        # The demo actor's health (100, on the heap) into the focused value
        # field. (g_cortex_u32 sits in a copy-on-write page the default scan
        # skips, as Cheat Engine's does.)
        type_text("100")
        chord(VK_RETURN)
        time.sleep(6.0)
        screenshot(out, "07-first-scan", rect(hwnd))
        check(desktop.poll() is None, "the desktop is still running after attach and scan")

        # 5. An MCP client next to the desktop.
        pid = manifest["pid"]
        inject = subprocess.run([exe, "inject", str(pid)], cwd=bundle, env=env, capture_output=True, text=True,
                                timeout=120)
        check(inject.returncode == 0 and wait_for_runtime(bundle, pid), "runtime loaded for the MCP client",
              inject.stdout + inject.stderr)
        address = manifest["u32"]
        hash_file = manifest["hash_file"]
        for allow in (False, True):
            command = [exe, "mcp", "--pid", str(pid), "--tools", "all"] + (["--allow-writes"] if allow else [])
            server = Mcp(command, bundle)
            try:
                check(server.initialize() is not None, "MCP client connected (%s)" %
                      ("--allow-writes" if allow else "read-only"))
                before = read_hash(hash_file)
                _, content = server.call("memory_write", {"address": address, "type": "u32", "value": 305419896,
                                                          "mutation_permission": True})
                time.sleep(1.0)
                after = read_hash(hash_file)
                if allow:
                    check(error_code(content) is None and after != before,
                          "with --allow-writes and mutation_permission the write is applied", content)
                else:
                    check(error_code(content) is not None and after == before,
                          "read-only: the write is refused and the target unchanged", content)
                time.sleep(1.5)
                screenshot(out, "08-mcp-%s" % ("writes" if allow else "readonly"), rect(hwnd))
            finally:
                server.close()

        # 6. A clean close.
        user32.PostMessageW(hwnd, 0x0010, 0, 0)
        try:
            code = desktop.wait(timeout=30)
        except subprocess.TimeoutExpired:
            code = None
        check(code == 0, "the window closes cleanly (exit code %s)" % code)
        crashes = [name for root, _, files in os.walk(bundle) for name in files if name.lower().endswith(".dmp")]
        check(not crashes, "no crash dump", crashes)
    finally:
        for w in notepads:
            user32.PostMessageW(w[0], 0x0010, 0, 0)
        if desktop.poll() is None:
            desktop.kill()
        if target is not None:
            target.kill()
        with open(os.path.join(out, "report.json"), "w") as handle:
            json.dump({"results": results, "failures": failures}, handle, indent=2)

    print("desktop acceptance: %s (%d checks, %d failed)" %
          ("PASS" if not failures else "FAIL", len(results), len(failures)), flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
