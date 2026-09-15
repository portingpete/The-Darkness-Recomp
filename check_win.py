import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32
WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

pid = 84012
windows = []

def enum_cb(hwnd, lparam):
    wpid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
    if wpid.value == pid:
        length = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        visible = user32.IsWindowVisible(hwnd)
        windows.append((hwnd, buf.value, visible))
    return True

user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
for w in windows:
    print(f'HWND: 0x{w[0]:08X}, Title: \"{w[1]}\", Visible: {w[2]}')
if not windows:
    print('No windows found for PID', pid)
