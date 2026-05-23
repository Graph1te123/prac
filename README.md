# Prac Tray Application

Win32 API tray application for Windows.

## Features

- Adds an icon to the taskbar notification area on startup.
- Left-clicking the tray icon opens the main window.
- Right-clicking the tray icon opens a menu with Open and Exit commands.
- Restores the tray icon after the Windows taskbar is recreated.
- Supports hidden startup with `--background`, `--hidden`, `--minimized`,
  `/background`, or `/tray`.
- Closing the main window hides it and keeps the app running in the background.
- The main window has a File menu with an Exit command.
- Uses a named mutex to prevent multiple instances for the current user.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be created at `build/Release/prac-tray.exe`.
