# RotateDisplay — wersja C++

Natywna wersja aplikacji dla Windows 10/11. Nie wymaga Pythona, Pillow,
pystray ani PyInstallera.

## Funkcje

- obracanie wybranego monitora skrótem `Ctrl+Alt+R`;
- opcjonalny skrót na klawiszu multimedialnym `0xB4`;
- wybór monitora z menu ikony w zasobniku;
- zapisywanie i przywracanie układu monitorów dla orientacji poziomej i pionowej;
- zgodność z konfiguracją wersji Python w `%APPDATA%\RotateDisplay\config.json`;
- ikona zasobnika rysowana bezpośrednio przez program.

## Budowanie w Visual Studio 2022

Zainstaluj workload **Desktop development with C++**, a następnie w terminalu
deweloperskim Visual Studio wykonaj z katalogu głównego repozytorium:

```powershell
cmake -S "wersja c++" -B "wersja c++\build" -A x64
cmake --build "wersja c++\build" --config Release
```

Gotowy program znajdziesz tutaj:

```text
wersja c++\build\Release\RotateDisplay.exe
```

Program jest aplikacją okienkową i nie otwiera konsoli.
