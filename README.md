# RotateDisplay

Mała aplikacja w zasobniku systemowym Windows, która jednym skrótem klawiszowym przełącza orientację wybranego monitora między poziomą a pionową i przywraca zapamiętany układ monitorów względem siebie.

## Funkcje

- Przełączanie orientacji (poziom <-> pion) wybranego monitora przez ikonę w zasobniku lub skrót klawiszowy.
- Globalny skrót **Ctrl+Alt+R** oraz opcjonalnie dowolny klawisz multimedialny (np. na klawiaturze Logitech K270).
- Automatyczne wykrywanie monitorów, wybór obsługiwanego monitora z menu (zaznaczony ptaszkiem).
- Zapamiętywanie pozycji monitorów względem siebie osobno dla trybu poziomego i pionowego:
  przy każdym przełączeniu program zapisuje bieżący układ, zmienia orientację i ustawia zapisany układ dla nowego trybu.
- Ustawienia zapisywane w `%APPDATA%\RotateDisplay\config.json`.

## Wymagania

- Windows 10 / 11
- Python 3.10+ (tylko do uruchamiania ze źródeł lub budowania exe)
- Biblioteki: `pystray`, `pillow`

```
pip install -r requirements.txt
```

## Uruchamianie

Ze źródeł, bez okna konsoli:

```
pythonw rotate_tray.py
```

## Budowanie exe

```
pip install pyinstaller
pyinstaller --onefile --noconsole rotate_tray.py
```

Gotowy plik znajdziesz w `dist\rotate_tray.exe`. Niektóre antywirusy potrafią fałszywie flagować exe z PyInstallera, wtedy trzeba dodać wyjątek.

## Autostart

Aplikacja musi działać w sesji użytkownika (nie jako usługa Windows, bo usługi nie mają dostępu do pulpitu).

Skopiuj exe w stałe miejsce (np. `C:\Tools\rotate_tray.exe`), a następnie:

```
reg add HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v RotateDisplay /t REG_SZ /d "\"C:\Tools\rotate_tray.exe\"" /f
```

Albo wrzuć skrót do folderu otwieranego przez `Win+R` -> `shell:startup`.

## Konfiguracja

Na górze pliku `rotate_tray.py`:

| Ustawienie       | Opis                                                                 |
|------------------|----------------------------------------------------------------------|
| `DEFAULT_DEVICE` | Monitor używany, gdy nie ma jeszcze zapisanego wyboru (`\\.\DISPLAY3`) |
| `HOTKEY_MODS`, `HOTKEY_VK` | Skrót klawiszowy (domyślnie Ctrl+Alt+R)                    |
| `MEDIA_VK`       | Kod VK klawisza multimedialnego, `None` wyłącza ten skrót            |

Kod VK klawisza można odczytać np. skryptem z `GetAsyncKeyState`. Typowe wartości: `0xB4` (Email), `0xAA` (Search), `0xB7` (Kalkulator).

Jeśli oprogramowanie Logitech (SetPoint / Options) przechwytuje klawisz multimedialny, ustaw dla niego akcję „brak".

### Plik `config.json`

```json
{
  "device": "\\\\.\\DISPLAY3",
  "layouts": {
    "landscape": { "\\\\.\\DISPLAY1": { "x": 0, "y": 0 } },
    "portrait":  { "\\\\.\\DISPLAY1": { "x": 0, "y": 0 } }
  }
}
```

Skasowanie pliku przywraca ustawienia domyślne.

## Uwagi

- Jeśli zmieni się liczba monitorów albo ich numeracja `DISPLAYn` (np. po aktualizacji sterownika GPU), zapisany układ nie zostanie zastosowany. Zapisz go ponownie z menu ikony (**Zapisz bieżący układ monitorów**).
- Podczas zmiany ekrany mogą mignąć drugi raz, bo pozycje są ustawiane osobno po obrocie.
- Niektóre sterowniki GPU mogą odrzucić zmianę orientacji. Program najpierw testuje zmianę (`CDS_TEST`).

## Jak to działa

Program korzysta wyłącznie z WinAPI przez `ctypes`: `EnumDisplayDevicesW`, `EnumDisplaySettingsW` i `ChangeDisplaySettingsExW`. Skróty są rejestrowane przez `RegisterHotKey`.
