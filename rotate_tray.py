import ctypes
import json
import os
import threading
import time
from ctypes import wintypes

import pystray
from PIL import Image, ImageDraw

# ---------- Ustawienia ----------
DEFAULT_DEVICE = r"\\.\DISPLAY3"      # używany, gdy nie ma jeszcze zapisanego wyboru

# Skrót 1: Ctrl + Alt + R
HOTKEY_MODS = 0x0001 | 0x0002 | 0x4000  # ALT | CTRL | NOREPEAT
HOTKEY_VK = 0x52                        # 'R'
# Skrót 2: klawisz multimedialny (None = wyłączony). Wpisz swój kod VK.
MEDIA_VK = 0xB4

CONFIG_DIR = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "RotateDisplay")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")
# --------------------------------

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

ENUM_CURRENT_SETTINGS = -1
DM_POSITION = 0x00000020
DM_DISPLAYORIENTATION = 0x00000080
DM_PELSWIDTH = 0x00080000
DM_PELSHEIGHT = 0x00100000
CDS_UPDATEREGISTRY = 0x01
CDS_TEST = 0x02
CDS_NORESET = 0x10000000
DISP_CHANGE_SUCCESSFUL = 0
DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x1
DISPLAY_DEVICE_PRIMARY_DEVICE = 0x4
WM_HOTKEY = 0x0312
WM_QUIT = 0x0012

LANDSCAPE, PORTRAIT = 0, 1


class DEVMODEW(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", wintypes.WCHAR * 32),
        ("dmSpecVersion", wintypes.WORD),
        ("dmDriverVersion", wintypes.WORD),
        ("dmSize", wintypes.WORD),
        ("dmDriverExtra", wintypes.WORD),
        ("dmFields", wintypes.DWORD),
        ("dmPositionX", wintypes.LONG),
        ("dmPositionY", wintypes.LONG),
        ("dmDisplayOrientation", wintypes.DWORD),
        ("dmDisplayFixedOutput", wintypes.DWORD),
        ("dmColor", wintypes.SHORT),
        ("dmDuplex", wintypes.SHORT),
        ("dmYResolution", wintypes.SHORT),
        ("dmTTOption", wintypes.SHORT),
        ("dmCollate", wintypes.SHORT),
        ("dmFormName", wintypes.WCHAR * 32),
        ("dmLogPixels", wintypes.WORD),
        ("dmBitsPerPel", wintypes.DWORD),
        ("dmPelsWidth", wintypes.DWORD),
        ("dmPelsHeight", wintypes.DWORD),
        ("dmDisplayFlags", wintypes.DWORD),
        ("dmDisplayFrequency", wintypes.DWORD),
        ("dmICMMethod", wintypes.DWORD),
        ("dmICMIntent", wintypes.DWORD),
        ("dmMediaType", wintypes.DWORD),
        ("dmDitherType", wintypes.DWORD),
        ("dmReserved1", wintypes.DWORD),
        ("dmReserved2", wintypes.DWORD),
        ("dmPanningWidth", wintypes.DWORD),
        ("dmPanningHeight", wintypes.DWORD),
    ]


class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("DeviceName", wintypes.WCHAR * 32),
        ("DeviceString", wintypes.WCHAR * 128),
        ("StateFlags", wintypes.DWORD),
        ("DeviceID", wintypes.WCHAR * 128),
        ("DeviceKey", wintypes.WCHAR * 128),
    ]


user32.EnumDisplaySettingsW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(DEVMODEW)]
user32.EnumDisplaySettingsW.restype = wintypes.BOOL
user32.ChangeDisplaySettingsExW.argtypes = [
    wintypes.LPCWSTR, ctypes.POINTER(DEVMODEW), wintypes.HWND, wintypes.DWORD, wintypes.LPVOID
]
user32.ChangeDisplaySettingsExW.restype = wintypes.LONG
user32.EnumDisplayDevicesW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(DISPLAY_DEVICEW), wintypes.DWORD
]
user32.EnumDisplayDevicesW.restype = wintypes.BOOL

_lock = threading.Lock()
_selected = DEFAULT_DEVICE
_icon = None
_config = {}


# ---------- Konfiguracja ----------
def load_config() -> None:
    global _config
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        _config = data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        _config = {}


def save_config() -> None:
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(_config, f, indent=2)
    except OSError:
        pass


# ---------- Monitory ----------
def get_current(device_name: str):
    dm = DEVMODEW()
    dm.dmSize = ctypes.sizeof(DEVMODEW)
    if not user32.EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, ctypes.byref(dm)):
        return None
    return dm


def list_monitors():
    """Aktywne monitory posortowane od lewej do prawej."""
    result = []
    i = 0
    while True:
        dd = DISPLAY_DEVICEW()
        dd.cb = ctypes.sizeof(dd)
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(dd), 0):
            break
        i += 1
        if not dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP:
            continue
        dm = get_current(dd.DeviceName)
        if dm is None:
            continue

        name = dd.DeviceString  # nazwa karty graficznej jako zapas
        mon = DISPLAY_DEVICEW()
        mon.cb = ctypes.sizeof(mon)
        if user32.EnumDisplayDevicesW(dd.DeviceName, 0, ctypes.byref(mon), 0):
            name = mon.DeviceString  # nazwa samego monitora

        result.append({
            "device": dd.DeviceName,
            "name": name,
            "w": dm.dmPelsWidth,
            "h": dm.dmPelsHeight,
            "x": dm.dmPositionX,
            "y": dm.dmPositionY,
            "primary": bool(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE),
        })
    result.sort(key=lambda m: (m["x"], m["device"]))
    return result


def rotate(target: int, device_name: str, persist: bool = True) -> bool:
    dm = get_current(device_name)
    if dm is None:
        return False

    was_portrait = dm.dmDisplayOrientation in (1, 3)
    will_be_portrait = target in (1, 3)

    # przejście poziom <-> pion wymaga zamiany wymiarów
    if was_portrait != will_be_portrait:
        dm.dmPelsWidth, dm.dmPelsHeight = dm.dmPelsHeight, dm.dmPelsWidth

    dm.dmDisplayOrientation = target
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT

    if user32.ChangeDisplaySettingsExW(device_name, ctypes.byref(dm), None, CDS_TEST, None) != DISP_CHANGE_SUCCESSFUL:
        return False

    flags = CDS_UPDATEREGISTRY if persist else 0
    return user32.ChangeDisplaySettingsExW(device_name, ctypes.byref(dm), None, flags, None) == DISP_CHANGE_SUCCESSFUL


# ---------- Układ monitorów ----------
def mode_of(device: str) -> str:
    dm = get_current(device)
    return "portrait" if dm is not None and dm.dmDisplayOrientation in (1, 3) else "landscape"


def snapshot_layout() -> dict:
    return {m["device"]: {"x": m["x"], "y": m["y"]} for m in list_monitors()}


def save_layout(mode: str | None = None, only_if_missing: bool = False) -> bool:
    mode = mode or mode_of(_selected)
    layouts = _config.setdefault("layouts", {})
    if only_if_missing and mode in layouts:
        return False
    layouts[mode] = snapshot_layout()
    save_config()
    return True


def apply_layout(mode: str) -> bool:
    layout = _config.get("layouts", {}).get(mode)
    if not layout:
        return False

    current = {m["device"]: m for m in list_monitors()}
    if set(layout) != set(current):
        return False  # zmieniła się liczba/numeracja monitorów - nie ruszamy niczego

    changed = False
    for dev, pos in layout.items():
        m = current[dev]
        if m["x"] == pos["x"] and m["y"] == pos["y"]:
            continue
        dm = get_current(dev)
        if dm is None:
            continue
        dm.dmPositionX = pos["x"]
        dm.dmPositionY = pos["y"]
        dm.dmFields = DM_POSITION
        r = user32.ChangeDisplaySettingsExW(dev, ctypes.byref(dm), None, CDS_UPDATEREGISTRY | CDS_NORESET, None)
        if r != DISP_CHANGE_SUCCESSFUL:
            return False
        changed = True

    if changed:
        # zatwierdza wszystkie zmiany pozycji naraz
        user32.ChangeDisplaySettingsExW(None, None, None, 0, None)
    return changed


# ---------- Akcje ----------
def notify(msg: str) -> None:
    try:
        if _icon is not None:
            _icon.notify(msg, "Obrót monitora")
    except Exception:
        pass


def toggle(*_):
    with _lock:
        monitors = {m["device"] for m in list_monitors()}
        if _selected not in monitors:
            notify(f"Wybrany monitor ({_selected}) nie jest dostępny. Wybierz inny z menu.")
            return
        dm = get_current(_selected)
        if dm is None:
            return

        is_portrait = dm.dmDisplayOrientation in (1, 3)
        mode_before = "portrait" if is_portrait else "landscape"
        new_mode = "landscape" if is_portrait else "portrait"

        # 1. zawsze zapisz aktualne pozycje monitorów dla obecnej orientacji
        save_layout(mode_before)

        # 2. zmień orientację
        if rotate(LANDSCAPE if is_portrait else PORTRAIT, _selected):
            time.sleep(0.4)  # daj Windowsowi chwilę na przeliczenie pulpitu
            # 3. ustaw zapisane pozycje dla nowej orientacji
            apply_layout(new_mode)
        else:
            notify("Nie udało się zmienić orientacji.")


def select_monitor(device: str):
    global _selected
    _selected = device
    _config["device"] = device
    save_config()
    if _icon is not None:
        _icon.update_menu()


def menu_save_layout(*_):
    with _lock:
        mode = mode_of(_selected)
        save_layout(mode)
        notify("Zapisano układ monitorów dla trybu: " + ("pionowego" if mode == "portrait" else "poziomego"))


def menu_restore_layout(*_):
    with _lock:
        mode = mode_of(_selected)
        if not _config.get("layouts", {}).get(mode):
            notify("Brak zapisanego układu dla tego trybu.")
        elif not apply_layout(mode):
            notify("Układ już jest zgodny z zapisanym albo nie da się go zastosować.")


# ---------- Menu w zasobniku ----------
def monitor_items():
    monitors = list_monitors()
    if not monitors:
        yield pystray.MenuItem("Brak wykrytych monitorów", None, enabled=False)
        return
    for n, m in enumerate(monitors, start=1):
        label = f"{n}. {m['name']}  {m['w']}x{m['h']}  [{m['device'].replace(chr(92), '')[-8:]}]"
        if m["primary"]:
            label += "  (główny)"
        yield pystray.MenuItem(
            label,
            (lambda dev: (lambda icon, item: select_monitor(dev)))(m["device"]),
            checked=(lambda dev: (lambda item: _selected == dev))(m["device"]),
            radio=True,
        )


def make_icon_image() -> Image.Image:
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((18, 6, 46, 58), radius=4, fill=(40, 120, 220, 255), outline=(255, 255, 255, 255), width=3)
    d.rectangle((24, 12, 40, 46), fill=(200, 225, 255, 255))
    return img


def main():
    global _selected, _icon

    load_config()
    saved = _config.get("device")
    if saved:
        _selected = saved
    elif not any(m["device"] == DEFAULT_DEVICE for m in list_monitors()):
        primary = next((m for m in list_monitors() if m["primary"]), None)
        if primary:
            _selected = primary["device"]

    main_tid = kernel32.GetCurrentThreadId()

    def quit_app(icon, item):
        icon.stop()
        user32.PostThreadMessageW(main_tid, WM_QUIT, 0, 0)

    _icon = pystray.Icon(
        "rotate_display",
        make_icon_image(),
        "Obrót monitora (Ctrl+Alt+R)",
        menu=pystray.Menu(
            pystray.MenuItem("Przełącz orientację", toggle, default=True),
            pystray.MenuItem("Monitor do obsługi", pystray.Menu(monitor_items)),
            pystray.MenuItem("Zapisz bieżący układ monitorów", menu_save_layout),
            pystray.MenuItem("Przywróć zapisany układ", menu_restore_layout),
            pystray.MenuItem("Zakończ", quit_app),
        ),
    )
    _icon.run_detached()

    # Skróty rejestrujemy w wątku z pętlą komunikatów
    if not user32.RegisterHotKey(None, 1, HOTKEY_MODS, HOTKEY_VK):
        _icon.title = "Skrót Ctrl+Alt+R zajęty"
    if MEDIA_VK is not None:
        user32.RegisterHotKey(None, 2, 0x4000, MEDIA_VK)

    msg = wintypes.MSG()
    while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        if msg.message == WM_HOTKEY:
            toggle()

    user32.UnregisterHotKey(None, 1)
    user32.UnregisterHotKey(None, 2)


if __name__ == "__main__":
    main()